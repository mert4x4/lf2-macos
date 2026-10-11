#!/usr/bin/env python3
"""lf2port: turn Little Fighter 2 (Windows) into a native macOS application.

    ./lf2port.py LF2_v2.0a.exe            # the original installer
    ./lf2port.py path/to/game-folder      # or an installed copy containing lf2.exe
    ./lf2port.py ... -o ~/Applications/LF2.app

The produced .app is self-contained: it links only macOS system libraries and
carries the game files inside it. This tool itself needs Python 3, the Xcode
command line tools (clang, objdump, codesign) and the ffmpeg program, which is
used once to decode the game's .wma music so it can be stored as .m4a. No
ffmpeg code ends up in this tool or in the app.
"""
import argparse
import bz2
import concurrent.futures
import hashlib
import os
import plistlib
import shutil
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.abspath(__file__))
BUNDLE_ID = 'local.lf2port.littlefighter2'
MIN_MACOS = '11.0'
# The lf2.exe this port was developed against (Little Fighter 2 v2.0a).
KNOWN_EXE_SHA256 = '12dfa00f6b767508612550e9ab27ab74b4201ff4cb9ff31d068925924eab8fc5'
FRAMEWORKS = ['Cocoa', 'QuartzCore', 'AudioToolbox', 'CoreText', 'CoreGraphics',
              'CoreFoundation']
SKIP_FILES = {'uninstal.exe', 'data/temporary.txt'}


def step(msg):
    print('==> ' + msg, flush=True)


def die(msg):
    sys.exit('lf2port: ' + msg)


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        die('command failed: %s\n%s%s' % (' '.join(cmd), r.stdout, r.stderr))
    return r.stdout


# ------------------------------------------------------------------ installer
def pe_sections(d):
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    n = struct.unpack_from('<H', d, pe + 6)[0]
    o = pe + 24 + struct.unpack_from('<H', d, pe + 20)[0]
    out = []
    for _ in range(n):
        name = d[o:o + 8].rstrip(b'\0').decode('latin-1')
        vsize, va, rsize, raw = struct.unpack_from('<IIII', d, o + 8)
        out.append((name, va, vsize, raw, rsize))
        o += 40
    return out


def is_installer(d):
    if d[:2] != b'MZ':
        return False
    end = max(raw + rsize for _, _, _, raw, rsize in pe_sections(d))
    return d[end:end + 6] == b'wwgT)H'


def extract_installer(path, out):
    """Unpacks a Clickteam Install Creator archive without running it."""
    d = open(path, 'rb').read()
    o = max(raw + rsize for _, _, _, raw, rsize in pe_sections(d)) + 6
    listing = base = None
    while o + 8 <= len(d):
        bid, _, size = struct.unpack_from('<HHI', d, o)
        body = o + 8
        if bid == 0x7f7f:
            base = body + 4
        elif bid == 0x143a:
            method = d[body + 4]
            raw = d[body + 5:body + size]
            listing = zlib.decompress(raw) if method == 1 else bz2.decompress(raw)
        o = body + size
    if listing is None or base is None:
        die('unrecognised installer layout in ' + path)
    out = os.path.realpath(out)
    count = struct.unpack_from('<I', listing, 0)[0]
    o, written = 4, 0
    for _ in range(count):
        size = struct.unpack_from('<H', listing, o)[0]
        kind, off, csize, _, usize = struct.unpack_from('<IIIII', listing, o + 2)
        name = listing[o + 62:o + size].split(b'\0')[0].decode('latin-1')
        o += size
        if kind != 0:
            continue
        parts = [p for p in name.replace('\\', '/').split('/') if p]
        if not parts or ':' in name or any(p in ('.', '..') for p in parts):
            die('unsafe path in installer: ' + name)
        blob = d[base + off:base + off + csize]
        raw = (zlib.decompress(blob[1:]) if blob[0] == 1 else
               bz2.decompress(blob[1:]) if blob[0] == 2 else blob[1:])
        if len(raw) != usize:
            die('corrupt entry in installer: ' + name)
        dst = os.path.realpath(os.path.join(out, *parts))
        if not dst.startswith(out + os.sep):
            die('unsafe path in installer: ' + name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(raw)
        written += 1
    return written


# ----------------------------------------------------------------------- icon
def exe_icon(exe, work):
    """Best effort: the exe's icon as an .icns file, or None."""
    try:
        d = open(exe, 'rb').read()
        rs = next(s for s in pe_sections(d) if s[0] == '.rsrc')
        root, rva = rs[3], rs[1]

        def entries(off):
            named, ids = struct.unpack_from('<HH', d, off + 12)
            return [struct.unpack_from('<II', d, off + 16 + 8 * i)
                    for i in range(named + ids)]

        def leaf(off):  # follow the first entry down to a data entry
            while off & 0x80000000:
                off = entries(root + (off & 0x7fffffff))[0][1]
            r, size = struct.unpack_from('<II', d, root + off)
            return d[r - rva + root:r - rva + root + size]

        types = dict(entries(root))
        icons = {i: leaf(o) for i, o in entries(root + (types[3] & 0x7fffffff))}
        group = leaf(types[14])
        count = struct.unpack_from('<H', group, 4)[0]
        ico = bytearray(struct.pack('<HHH', 0, 1, count))
        blobs, offset = [], 6 + 16 * count
        for i in range(count):
            e = group[6 + 14 * i:6 + 14 * i + 14]
            ident = struct.unpack_from('<H', e, 12)[0]
            ico += e[:12] + struct.pack('<I', offset)
            blobs.append(icons[ident])
            offset += len(icons[ident])
        ico_path = os.path.join(work, 'icon.ico')
        with open(ico_path, 'wb') as f:
            f.write(bytes(ico) + b''.join(blobs))
        iconset = os.path.join(work, 'AppIcon.iconset')
        shutil.rmtree(iconset, ignore_errors=True)
        os.makedirs(iconset)
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                name = 'icon_%dx%d%s.png' % (size, size, '@2x' if scale == 2 else '')
                px = str(size * scale)
                subprocess.run(['sips', '-s', 'format', 'png', '-z', px, px, ico_path,
                                '--out', os.path.join(iconset, name)],
                               capture_output=True, check=True)
        icns = os.path.join(work, 'AppIcon.icns')
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', icns],
                       capture_output=True, check=True)
        return icns
    except Exception:
        return None


# ---------------------------------------------------------------------- build
def compile_all(work, jobs):
    gen, obj = os.path.join(work, 'gen'), os.path.join(work, 'obj')
    os.makedirs(obj, exist_ok=True)
    rt = os.path.join(ROOT, 'runtime')
    sources = ([os.path.join(gen, f) for f in sorted(os.listdir(gen)) if f.endswith('.c')] +
               [os.path.join(rt, f) for f in sorted(os.listdir(rt))
                if f.endswith(('.c', '.m'))])
    headers = [os.path.join(rt, f) for f in os.listdir(rt) if f.endswith('.h')]
    headers.append(os.path.join(gen, 'lf2_funcs.h'))
    newest_header = max(os.path.getmtime(h) for h in headers)
    flags = ['-O2', '-mmacosx-version-min=' + MIN_MACOS, '-I' + rt, '-I' + gen,
             '-Wno-unused-label', '-Wno-unused-variable', '-Wno-unused-but-set-variable']

    def one(src):
        tag = 'gen_' if src.startswith(gen) else 'rt_'
        out = os.path.join(obj, tag + os.path.basename(src) + '.o')
        if (os.path.exists(out) and
                os.path.getmtime(out) > max(os.path.getmtime(src), newest_header)):
            return out
        extra = ['-fobjc-arc'] if src.endswith('.m') else []
        run(['clang'] + flags + extra + ['-c', src, '-o', out])
        return out

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        objects = list(ex.map(one, sources))
    binary = os.path.join(work, 'lf2')
    cmd = ['clang', '-mmacosx-version-min=' + MIN_MACOS, '-o', binary] + objects
    for fw in FRAMEWORKS:
        cmd += ['-framework', fw]
    cmd += ['-lm']
    run(cmd)
    return binary


def check_self_contained(binary):
    libs = [l.split()[0] for l in run(['otool', '-L', binary]).splitlines()[1:]]
    foreign = [l for l in libs if not l.startswith(('/System/Library/', '/usr/lib/'))]
    if foreign:
        die('the build still depends on non-system libraries:\n  ' + '\n  '.join(foreign))
    return len(libs)


def convert_music(game, work, ffmpeg, jobs):
    """.wma tracks -> .m4a: ffmpeg decodes, macOS's own encoder (afconvert) encodes.

    Returns {path relative to the game folder: converted file}."""
    out_dir = os.path.join(work, 'music')
    os.makedirs(out_dir, exist_ok=True)
    tracks = [os.path.join(dp, f) for dp, _, fs in os.walk(game) for f in fs
              if f.lower().endswith('.wma')]

    def one(src):
        rel = os.path.relpath(src, game)
        dst = os.path.join(out_dir, rel.replace(os.sep, '_')[:-4] + '.m4a')
        if os.path.exists(dst) and os.path.getmtime(dst) > os.path.getmtime(src):
            return rel, dst
        wav = dst[:-4] + '.wav'
        run([ffmpeg, '-nostdin', '-v', 'error', '-y', '-i', src, '-ar', '44100', '-ac', '2', wav])
        run(['afconvert', '-f', 'm4af', '-d', 'aac', '-b', '192000', wav, dst])
        os.remove(wav)
        return rel, dst

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        return dict(ex.map(one, tracks))


def check_output(app):
    """Refuses to replace anything that is not an app this tool built earlier."""
    if not app.endswith('.app'):
        die('the output name must end in .app: ' + app)
    if not os.path.lexists(app):
        return
    try:
        with open(os.path.join(app, 'Contents', 'Info.plist'), 'rb') as f:
            ours = plistlib.load(f).get('CFBundleIdentifier') == BUNDLE_ID
    except Exception:
        ours = False
    if not ours:
        die(app + ' already exists and was not built by this tool; '
            'remove it yourself or choose another output name')


def make_bundle(app, binary, game, icon, music):
    shutil.rmtree(app, ignore_errors=True)
    macos = os.path.join(app, 'Contents', 'MacOS')
    res = os.path.join(app, 'Contents', 'Resources')
    os.makedirs(macos)
    os.makedirs(res)
    shutil.copy2(binary, os.path.join(macos, 'lf2'))
    run(['strip', '-S', os.path.join(macos, 'lf2')])  # no local build paths in the app
    copied = 0
    for dirpath, _, files in os.walk(game):
        for name in files:
            src = os.path.join(dirpath, name)
            rel = os.path.relpath(src, game)
            if rel.lower() in SKIP_FILES or name.startswith('.'):
                continue
            if rel in music:  # ship the converted track in place of the .wma
                src, rel = music[rel], rel[:-4] + '.m4a'
            elif rel.lower().endswith('.wma'):
                continue  # macOS cannot play it
            dst = os.path.join(res, 'game', rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            copied += 1
    info = {
        'CFBundleName': 'Little Fighter 2',
        'CFBundleDisplayName': 'Little Fighter 2',
        'CFBundleIdentifier': BUNDLE_ID,
        'CFBundleExecutable': 'lf2',
        'CFBundlePackageType': 'APPL',
        'CFBundleShortVersionString': '2.0a',
        'CFBundleVersion': '1',
        'LSMinimumSystemVersion': MIN_MACOS,
        'NSHighResolutionCapable': True,
        'LSApplicationCategoryType': 'public.app-category.action-games',
    }
    if icon:
        shutil.copy2(icon, os.path.join(res, 'AppIcon.icns'))
        info['CFBundleIconFile'] = 'AppIcon'
    with open(os.path.join(app, 'Contents', 'Info.plist'), 'wb') as f:
        plistlib.dump(info, f)
    run(['codesign', '--force', '--sign', '-', app])
    return copied


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input', help='LF2 installer .exe, lf2.exe, or a folder containing it')
    ap.add_argument('-o', '--output', default='LF2.app', help='application to create')
    ap.add_argument('--work', default=os.path.join(ROOT, 'build'), help='build directory')
    ap.add_argument('--ffmpeg', default=shutil.which('ffmpeg') or 'ffmpeg',
                    help='ffmpeg program used to decode the .wma music')
    ap.add_argument('--no-music', action='store_true',
                    help='build without background music (ffmpeg is then not needed)')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    app = os.path.abspath(a.output)
    check_output(app)
    for tool in ('clang', 'objdump', 'otool', 'codesign', 'strip', 'afconvert'):
        if not shutil.which(tool):
            die(tool + ' not found: install the Xcode command line tools')
    if not a.no_music and not shutil.which(a.ffmpeg):
        die('ffmpeg not found. It is needed once to decode the .wma music: install it '
            '(brew install ffmpeg), pass --ffmpeg PATH, or use --no-music')
    os.makedirs(a.work, exist_ok=True)

    src = os.path.abspath(a.input)
    if os.path.isdir(src):
        game = src
    elif os.path.isfile(src) and is_installer(open(src, 'rb').read()):
        game = os.path.join(a.work, 'game')
        step('Unpacking installer')
        shutil.rmtree(game, ignore_errors=True)
        print('    %d files' % extract_installer(src, game))
    elif os.path.isfile(src):
        game = os.path.dirname(src)
    else:
        die('no such file or folder: ' + src)
    exe = os.path.join(game, 'lf2.exe')
    if not os.path.isfile(exe):
        die('lf2.exe not found in ' + game)
    with open(exe, 'rb') as f:
        if hashlib.sha256(f.read()).hexdigest() != KNOWN_EXE_SHA256:
            print('warning: this lf2.exe is not the v2.0a build the port was developed '
                  'against;\n         it will be translated, but the result is untested.')

    step('Translating lf2.exe to C')
    out = run([sys.executable, os.path.join(ROOT, 'recomp', 'recomp.py'), exe,
               os.path.join(a.work, 'gen'), os.path.join(ROOT, 'recomp', 'seeds.txt')])
    print('    ' + out.splitlines()[0])

    step('Compiling for %s' % os.uname().machine)
    binary = compile_all(a.work, a.jobs)
    print('    links %d system libraries, nothing else' % check_self_contained(binary))

    music = {}
    if not a.no_music:
        step('Converting music')
        music = convert_music(game, a.work, a.ffmpeg, a.jobs)
        print('    %d tracks' % len(music))

    step('Building ' + a.output)
    icon = exe_icon(exe, a.work)
    copied = make_bundle(app, binary, game, icon, music)
    size = sum(os.path.getsize(os.path.join(dp, f)) for dp, _, fs in os.walk(app) for f in fs)
    print('    %d game files, %.0f MB, icon: %s' % (copied, size / 1e6, 'yes' if icon else 'no'))
    print('Done: ' + app)


if __name__ == '__main__':
    main()
