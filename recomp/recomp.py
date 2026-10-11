#!/usr/bin/env python3
"""Static recompiler: lf2.exe (x86-32 PE) -> C.

Disassembly comes from llvm-objdump (Intel syntax). Every guest function becomes
`void sub_XXXXXXXX(CPU *c)` operating on the CPU struct and guest memory, so the
guest stack layout stays exactly as the original code expects.
"""
import os
import re
import struct
import subprocess
import sys

EXE, OUT = sys.argv[1], sys.argv[2]
EXTRA_SEEDS = sys.argv[3] if len(sys.argv) > 3 else None
os.makedirs(OUT, exist_ok=True)

# ---------------------------------------------------------------- PE image
data = open(EXE, 'rb').read()
pe = struct.unpack_from('<I', data, 0x3c)[0]
nsec = struct.unpack_from('<H', data, pe + 6)[0]
optsz = struct.unpack_from('<H', data, pe + 20)[0]
O = pe + 24
ENTRY = struct.unpack_from('<I', data, O + 16)[0]
BASE = struct.unpack_from('<I', data, O + 28)[0]
ENTRY += BASE
IMP_RVA = struct.unpack_from('<I', data, O + 96 + 8)[0]
sections = []
o = O + optsz
for _ in range(nsec):
    name = data[o:o + 8].rstrip(b'\0').decode()
    vs, va, rs, rp = struct.unpack_from('<IIII', data, o + 8)
    sections.append((name, BASE + va, vs, rp, rs))
    o += 40
TEXT = next(s for s in sections if s[0] == '.text')
TEXT_LO, TEXT_HI = TEXT[1], TEXT[1] + TEXT[2]


def va2off(va):
    for name, sva, vs, rp, rs in sections:
        if sva <= va < sva + max(vs, rs):
            return va - sva + rp
    return None


def rd32(va):
    off = va2off(va)
    if off is None or off + 4 > len(data):
        return None
    return struct.unpack_from('<I', data, off)[0]


def cstr(va):
    off = va2off(va)
    return data[off:data.index(b'\0', off)].decode('latin-1')


def in_text(a):
    return a is not None and TEXT_LO <= a < TEXT_HI


# ---------------------------------------------------------------- imports
imports = []  # (iat_va, dll, name)
o = va2off(BASE + IMP_RVA)
while True:
    oft, _, _, nrva, ft = struct.unpack_from('<IIIII', data, o)
    if not nrva:
        break
    dll = cstr(BASE + nrva)
    t = BASE + (oft or ft)
    slot = BASE + ft
    while True:
        v = rd32(t)
        if not v:
            break
        name = 'ord%d' % (v & 0xffff) if v >> 31 else cstr(BASE + v + 2)
        imports.append((slot, dll, name))
        t += 4
        slot += 4
    o += 20

# ---------------------------------------------------------------- decoding
LINE = re.compile(r'^\s*([0-9a-f]+):((?: [0-9a-f]{2})+)\s*\t(.*)$')
PREFIXES = ('rep', 'repe', 'repne', 'lock')


class Insn:
    __slots__ = ('addr', 'len', 'raw', 'prefix', 'mn', 'ops', 'text')


insns = {}


def parse_asm(text):
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        i = Insn()
        i.addr = int(m.group(1), 16)
        i.raw = bytes.fromhex(m.group(2).replace(' ', ''))
        i.len = len(i.raw)
        toks = [t for t in m.group(3).split('\t') if t.strip()]
        i.text = ' '.join(t.strip() for t in toks)
        i.prefix = None
        if toks and toks[0].strip() in PREFIXES:
            i.prefix = toks[0].strip()
            toks = toks[1:]
        i.mn = toks[0].strip() if toks else '(bad)'
        i.ops = split_ops(toks[1].strip()) if len(toks) > 1 else []
        insns.setdefault(i.addr, i)


def split_ops(s):
    return [x.strip() for x in s.split(',')] if s else []


def objdump(extra):
    return subprocess.run(
        ['objdump', '-d', '--x86-asm-syntax=intel', '--section=.text'] + extra +
        [EXE], capture_output=True, text=True, check=True).stdout


parse_asm(objdump([]))


def insn_at(a):
    if not in_text(a):
        return None
    if a not in insns:
        parse_asm(objdump(['--start-address=0x%x' % a,
                           '--stop-address=0x%x' % min(a + 0x800, TEXT_HI)]))
    return insns.get(a)


# ---------------------------------------------------------------- operands
R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
R16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
R8 = ['al', 'cl', 'dl', 'bl', 'ah', 'ch', 'dh', 'bh']
REGSZ = {**{r: 4 for r in R32}, **{r: 2 for r in R16}, **{r: 1 for r in R8}}
SIZEKW = {'byte': 1, 'word': 2, 'dword': 4, 'qword': 8, 'xword': 10, 'tbyte': 10}
MEMRE = re.compile(r'^(?:(\w+) ptr )?(?:(\w\w):)?\[(.*)\]$')
BITS = {1: 8, 2: 16, 4: 32, 8: 64}
MASK = {1: 0xff, 2: 0xffff, 4: 0xffffffff}
UT = {1: 'u8', 2: 'u16', 4: 'u32', 8: 'u64'}
ST_ = {1: 's8', 2: 's16', 4: 's32', 8: 's64'}


class Unsupported(Exception):
    pass


def parse_int(s):
    s = s.strip()
    neg = s.startswith('-')
    if neg:
        s = s[1:]
    v = int(s, 16) if s.startswith('0x') else int(s)
    return -v if neg else v


def parse_op(s):
    """-> ('reg', name, size) | ('mem', size, addr_c) | ('imm', v) | ('st', i)"""
    if s in REGSZ:
        return ('reg', s, REGSZ[s])
    if s == 'st':
        return ('st', 0)
    m = re.match(r'^st\((\d)\)$', s)
    if m:
        return ('st', int(m.group(1)))
    m = MEMRE.match(s)
    if m:
        size = SIZEKW.get(m.group(1)) if m.group(1) else None
        if m.group(1) and size is None:
            raise Unsupported('size ' + s)
        seg, expr = m.group(2), m.group(3)
        terms = []
        for sign, term in re.findall(r'([+-]?)\s*([^+-]+)', expr):
            term = term.strip()
            if term in REGSZ:
                t = term.upper()
            elif '*' in term:
                k, r = term.split('*')
                t = '%s*%du' % (r.strip().upper(), int(k))
            else:
                t = '0x%xu' % (parse_int(term) & 0xffffffff)
            terms.append((sign or '+') + ' ' + t)
        addr = ' '.join(terms).lstrip('+ ').strip()
        if seg == 'fs':
            addr = 'c->fs_base + ' + addr
        elif seg not in (None, 'es', 'ds', 'ss'):
            raise Unsupported('segment ' + s)
        return ('mem', size, '(u32)(%s)' % addr)
    m = re.match(r'^(-?(?:0x[0-9a-f]+|\d+))(?: <.*>)?$', s)
    if m:
        return ('imm', parse_int(m.group(1)))
    raise Unsupported('operand ' + s)


def osize(op):
    if op[0] == 'reg':
        return op[2]
    if op[0] == 'mem':
        return op[1]
    return None


def rd(op, size):
    if op[0] == 'reg':
        return op[1].upper()
    if op[0] == 'mem':
        return 'M%d(%s)' % (BITS[size], op[2])
    if op[0] == 'imm':
        return '(%s)0x%xu' % (UT[size], op[1] & MASK[size])
    raise Unsupported('rd ' + repr(op))


CC = {'o': 'O', 'no': 'NO', 'b': 'B', 'c': 'B', 'nae': 'B', 'ae': 'AE',
      'nb': 'AE', 'nc': 'AE', 'e': 'E', 'z': 'E', 'ne': 'NE', 'nz': 'NE',
      'be': 'BE', 'na': 'BE', 'a': 'A', 'nbe': 'A', 's': 'S', 'ns': 'NS',
      'p': 'P', 'pe': 'P', 'np': 'NP', 'po': 'NP', 'l': 'L', 'nge': 'L',
      'ge': 'GE', 'nl': 'GE', 'le': 'LE', 'ng': 'LE', 'g': 'G', 'nle': 'G'}
ALU = ('add', 'adc', 'sub', 'sbb', 'and', 'or', 'xor')
SHIFT = ('shl', 'sal', 'shr', 'sar', 'rol', 'ror')
STRSZ = {'b': 8, 'w': 16, 'd': 32}


def is_jcc(mn):
    return mn[0] == 'j' and mn[1:] in CC


def branch_target(i):
    try:
        op = parse_op(i.ops[0])
    except Unsupported:
        return None
    return op[1] & 0xffffffff if op[0] == 'imm' else None


def jump_table(i):
    """Targets of `jmp dword ptr [4*reg + TABLE]`, or None."""
    m = re.match(r'^dword ptr \[4\*\w+ \+ (0x[0-9a-f]+)\]$', i.ops[0])
    if not m:
        return None
    return int(m.group(1), 16)


table_starts = set()
for i in list(insns.values()):
    if i.mn == 'jmp' and i.ops:
        t = jump_table(i)
        if t:
            table_starts.add(t)


def table_targets(tbl):
    out = []
    a = tbl
    while True:
        v = rd32(a)
        if not in_text(v) or (a != tbl and a in table_starts):
            break
        out.append(v)
        a += 4
    return out


# ---------------------------------------------------------------- x87
def x87_reg(i):
    """Register-form x87 instructions decoded from the opcode bytes."""
    raw = i.raw
    k = 0
    while raw[k] in (0x9b, 0x66):
        k += 1
    b0, b1 = raw[k], raw[k + 1]
    if b1 < 0xc0:
        return None
    n, g = b1 & 7, (b1 >> 3) & 7
    S = 'ST(%d)' % n
    if b0 == 0xd8:
        return ['ST(0) = ST(0) + %s;' % S, 'ST(0) = ST(0) * %s;' % S,
                'fcom(c, ST(0), %s);' % S, 'fcom(c, ST(0), %s); fpop(c);' % S,
                'ST(0) = ST(0) - %s;' % S, 'ST(0) = %s - ST(0);' % S,
                'ST(0) = ST(0) / %s;' % S, 'ST(0) = %s / ST(0);' % S][g]
    if b0 == 0xd9:
        if g == 0:
            return '{ double v = %s; fpush(c, v); }' % S
        if g == 1:
            return '{ double v = %s; %s = ST(0); ST(0) = v; }' % (S, S)
        simple = {0xd0: ';', 0xe0: 'ST(0) = -ST(0);', 0xe1: 'ST(0) = fabs(ST(0));',
                  0xe4: 'fcom(c, ST(0), 0.0);', 0xe8: 'fpush(c, 1.0);',
                  0xeb: 'fpush(c, 3.14159265358979323846);',
                  0xee: 'fpush(c, 0.0);', 0xfa: 'ST(0) = sqrt(ST(0));',
                  0xfc: 'ST(0) = fround(c, ST(0));', 0xfe: 'ST(0) = sin(ST(0));',
                  0xff: 'ST(0) = cos(ST(0));',
                  0xf3: 'ST(1) = atan2(ST(1), ST(0)); fpop(c);',
                  0xf8: 'ST(0) = fmod(ST(0), ST(1)); c->fsw &= ~0x400;'}
        return simple.get(b1)
    if b0 == 0xda and b1 == 0xe9:
        return 'fcom(c, ST(0), ST(1)); fpop(c); fpop(c);'
    if b0 == 0xdb:
        if b1 in (0xe2, 0xe3):
            return 'c->fsw = 0;' + (' c->fcw = 0x37f; c->ftop = 0;' if b1 == 0xe3 else '')
        if g in (5, 6):
            return 'fcomi(c, ST(0), %s);' % S
    if b0 == 0xdc:
        return ['%s = %s + ST(0);' % (S, S), '%s = %s * ST(0);' % (S, S), None,
                None, '%s = ST(0) - %s;' % (S, S), '%s = %s - ST(0);' % (S, S),
                '%s = ST(0) / %s;' % (S, S), '%s = %s / ST(0);' % (S, S)][g]
    if b0 == 0xdd:
        return [';', None, '%s = ST(0);' % S, '%s = ST(0); fpop(c);' % S,
                'fcom(c, ST(0), %s);' % S, 'fcom(c, ST(0), %s); fpop(c);' % S,
                None, None][g]
    if b0 == 0xde:
        if b1 == 0xd9:
            return 'fcom(c, ST(0), ST(1)); fpop(c); fpop(c);'
        r = ['%s = %s + ST(0);' % (S, S), '%s = %s * ST(0);' % (S, S), None,
             None, '%s = ST(0) - %s;' % (S, S), '%s = %s - ST(0);' % (S, S),
             '%s = ST(0) / %s;' % (S, S), '%s = %s / ST(0);' % (S, S)][g]
        return r and r + ' fpop(c);'
    if b0 == 0xdf:
        if b1 == 0xe0:
            return 'AX = fstsw(c);'
        if g in (5, 6):
            return 'fcomi(c, ST(0), %s); fpop(c);' % S
    return None


def x87_mem(i):
    mn = i.mn
    op = parse_op(i.ops[0])
    size, a = op[1], op[2]
    if mn in ('fnstcw', 'fstcw'):
        return 'M16(%s) = c->fcw;' % a
    if mn == 'fldcw':
        return 'c->fcw = M16(%s);' % a
    if mn in ('fnstsw', 'fstsw'):
        return 'M16(%s) = fstsw(c);' % a
    isint = mn.startswith('fi')
    if isint:
        v = {2: '(double)(s16)M16(%s)', 4: '(double)(s32)M32(%s)',
             8: '(double)(s64)M64(%s)'}[size] % a
    else:
        if size not in (4, 8):
            raise Unsupported('x87 size')
        v = {4: '(double)MF32(%s)', 8: 'MF64(%s)'}[size] % a
    base = mn[2:] if isint else mn[1:]
    if base == 'ld':
        return 'fpush(c, %s);' % v
    if base in ('st', 'stp'):
        pop = ' fpop(c);' if base == 'stp' else ''
        if isint:
            return 'M%d(%s) = f2i%d(c, ST(0));%s' % (size * 8, a, size * 8, pop)
        if size == 4:
            return 'MF32(%s) = (float)ST(0);%s' % (a, pop)
        return 'MF64(%s) = ST(0);%s' % (a, pop)
    arith = {'add': 'ST(0) = ST(0) + %s;', 'mul': 'ST(0) = ST(0) * %s;',
             'sub': 'ST(0) = ST(0) - %s;', 'subr': 'ST(0) = %s - ST(0);',
             'div': 'ST(0) = ST(0) / %s;', 'divr': 'ST(0) = %s / ST(0);',
             'com': 'fcom(c, ST(0), %s);', 'comp': 'fcom(c, ST(0), %s); fpop(c);'}
    if base in arith:
        return '{ double m = %s; %s }' % (v, arith[base] % 'm')
    raise Unsupported('x87 ' + mn)


# ---------------------------------------------------------------- codegen
def gen(i, fn):
    """C statement(s) for one instruction. fn: the Function being emitted."""
    mn = i.mn
    nxt = i.addr + i.len

    if i.raw[0] in range(0xd8, 0xe0) or (i.raw[0] == 0x9b and len(i.raw) > 1):
        if i.raw == b'\x9b':
            return ';'
        has_mem = any('[' in o for o in i.ops)
        r = x87_mem(i) if has_mem else x87_reg(i)
        if r is None:
            raise Unsupported('x87')
        return r

    if mn in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb',
              'lodsw', 'lodsd', 'cmpsb', 'cmpsw', 'cmpsd', 'scasb', 'scasw',
              'scasd'):
        n = STRSZ[mn[-1]]
        base = mn[:-1]
        if i.prefix in ('rep', 'repe', 'repne'):
            if base in ('cmps', 'scas'):
                return 'rep_%s%d(c, %d);' % (base, n, 0 if i.prefix == 'repne' else 1)
            return 'rep_%s%d(c);' % (base, n)
        return '%s%d(c);' % (base, n)
    if i.prefix in ('rep', 'repe', 'repne'):
        if mn == 'ret':  # "rep ret" branch predictor idiom
            pass
        else:
            raise Unsupported('prefix')

    if mn == 'cvttsd2si':
        m = parse_op(i.ops[1])
        return '{ double v = MF64(%s); %s = (v >= -2147483648.0 && v <= 2147483647.0) ? (u32)(s32)v : 0x80000000u; }' % (m[2], i.ops[0].upper())
    ops = [parse_op(o) for o in i.ops]

    if mn in ('nop', 'fwait', 'wait'):
        return ';'
    if mn == 'int3':
        return 'guest_trap(c, 0x%xu);' % i.addr
    if mn == 'mov':
        size = osize(ops[0]) or osize(ops[1])
        return '%s = %s;' % (rd(ops[0], size), rd(ops[1], size))
    if mn == 'lea':
        return '%s = %s;' % (rd(ops[0], ops[0][2]), ops[1][2])
    if mn in ('movzx', 'movsx'):
        ds, ss = osize(ops[0]), osize(ops[1])
        src = rd(ops[1], ss)
        if mn == 'movsx':
            src = '(%s)(%s)(%s)%s' % (UT[ds], ST_[ds], ST_[ss], src)
        return '%s = %s;' % (rd(ops[0], ds), src)
    if mn in ALU:
        size = osize(ops[0]) or osize(ops[1])
        d = rd(ops[0], size)
        return '%s = %s%d(c, %s, %s);' % (d, mn, size * 8, d, rd(ops[1], size))
    if mn in ('cmp', 'test'):
        size = osize(ops[0]) or osize(ops[1])
        f = 'sub' if mn == 'cmp' else 'and'
        return '(void)%s%d(c, %s, %s);' % (f, size * 8, rd(ops[0], size),
                                         rd(ops[1], size))
    if mn in ('inc', 'dec', 'neg'):
        size = osize(ops[0])
        d = rd(ops[0], size)
        return '%s = %s%d(c, %s);' % (d, mn, size * 8, d)
    if mn == 'not':
        size = osize(ops[0])
        d = rd(ops[0], size)
        return '%s = (%s)~%s;' % (d, UT[size], d)
    if mn in SHIFT:
        size = osize(ops[0])
        d = rd(ops[0], size)
        cnt = '1' if len(ops) == 1 else rd(ops[1], 1)
        f = 'shl' if mn == 'sal' else mn
        return '%s = %s%d(c, %s, %s);' % (d, f, size * 8, d, cnt)
    if mn in ('shld', 'shrd'):
        d = rd(ops[0], 4)
        return '%s = %s32(c, %s, %s, %s);' % (d, mn, d, rd(ops[1], 4), rd(ops[2], 1))
    if mn == 'imul':
        if len(ops) == 1:
            size = osize(ops[0])
            return 'imul1_%d(c, %s);' % (size * 8, rd(ops[0], size))
        size = osize(ops[0])
        if size == 1:
            raise Unsupported('imul8')
        a, b = (ops[0], ops[1]) if len(ops) == 2 else (ops[1], ops[2])
        return '%s = imul%d(c, %s, %s);' % (rd(ops[0], size), size * 8,
                                            rd(a, size), rd(b, size))
    if mn in ('mul', 'div', 'idiv'):
        size = osize(ops[0])
        return '%s%d(c, %s);' % (mn, size * 8, rd(ops[0], size))
    if mn == 'cdq':
        return 'EDX = (s32)EAX < 0 ? 0xffffffffu : 0;'
    if mn == 'cwde':
        return 'EAX = (u32)(s32)(s16)AX;'
    if mn == 'cbw':
        return 'AX = (u16)(s16)(s8)AL;'
    if mn == 'cwd':
        return 'DX = (s16)AX < 0 ? 0xffff : 0;'
    if mn == 'push':
        size = osize(ops[0]) or 4
        if size != 4:
            raise Unsupported('push16')
        return 'PUSH32(%s);' % rd(ops[0], 4)
    if mn == 'pop':
        if osize(ops[0]) != 4:
            raise Unsupported('pop16')
        return '{ u32 v = M32(ESP); ESP += 4; %s = v; }' % rd(ops[0], 4)
    if mn == 'pushfd':
        return 'PUSH32(pack_eflags(c));'
    if mn == 'popfd':
        return '{ u32 v = M32(ESP); ESP += 4; unpack_eflags(c, v); }'
    if mn == 'pushal':
        return ('{ u32 s = ESP; PUSH32(EAX); PUSH32(ECX); PUSH32(EDX); PUSH32(EBX); '
                'PUSH32(s); PUSH32(EBP); PUSH32(ESI); PUSH32(EDI); }')
    if mn == 'popal':
        return ('{ EDI = M32(ESP); ESI = M32(ESP + 4); EBP = M32(ESP + 8); '
                'EBX = M32(ESP + 16); EDX = M32(ESP + 20); ECX = M32(ESP + 24); '
                'EAX = M32(ESP + 28); ESP += 32; }')
    if mn == 'leave':
        return '{ ESP = EBP; EBP = M32(ESP); ESP += 4; }'
    if mn == 'xchg':
        size = osize(ops[0]) or osize(ops[1])
        a, b = ops
        if a[0] == 'mem':
            a, b = b, a
        if b[0] == 'mem':
            return '{ u32 ea = %s; %s t = M%d(ea); M%d(ea) = %s; %s = t; }' % (
                b[2], UT[size], size * 8, size * 8, rd(a, size), rd(a, size))
        return '{ %s t = %s; %s = %s; %s = t; }' % (
            UT[size], rd(a, size), rd(a, size), rd(b, size), rd(b, size))
    if mn == 'xadd':
        size = osize(ops[0])
        d, s = rd(ops[0], size), rd(ops[1], size)
        return '{ %s a = %s, b = %s; %s = a; %s = add%d(c, a, b); }' % (
            UT[size], d, s, s, d, size * 8)
    if mn == 'cmpxchg':
        size = osize(ops[0])
        acc = {1: 'AL', 2: 'AX', 4: 'EAX'}[size]
        d, s = rd(ops[0], size), rd(ops[1], size)
        return ('{ %s o = %s; (void)sub%d(c, %s, o); if (c->zf) %s = %s; '
                'else %s = o; }') % (UT[size], d, size * 8, acc, d, s, acc)
    if mn == 'bswap':
        return '%s = __builtin_bswap32(%s);' % (rd(ops[0], 4), rd(ops[0], 4))
    if mn == 'bt':
        size = osize(ops[0])
        if ops[0][0] == 'mem' and ops[1][0] != 'imm':
            raise Unsupported('bt mem,reg')
        return 'c->cf = (%s >> (%s & %d)) & 1;' % (rd(ops[0], size),
                                                  rd(ops[1], size), size * 8 - 1)
    if mn.startswith('set') and mn[3:] in CC:
        return '%s = COND_%s;' % (rd(ops[0], 1), CC[mn[3:]])
    if mn.startswith('cmov') and mn[4:] in CC:
        size = osize(ops[0])
        return 'if (COND_%s) %s = %s;' % (CC[mn[4:]], rd(ops[0], size),
                                         rd(ops[1], size))
    if mn in ('cld', 'std'):
        return 'c->df = %d;' % (mn == 'std')
    if mn in ('clc', 'stc'):
        return 'c->cf = %d;' % (mn == 'stc')
    if mn == 'cmc':
        return 'c->cf = !c->cf;'
    if mn == 'sahf':
        return 'sahf(c);'
    if mn == 'cpuid':
        return 'guest_cpuid(c);'

    # ---- control flow
    if mn == 'call':
        if ops[0][0] == 'imm':
            t = ops[0][1] & 0xffffffff
            fn.calls.add(t)
            return 'PUSH32(0x%xu); sub_%08x(c);' % (nxt, t)
        return '{ u32 t = %s; PUSH32(0x%xu); call_indirect(c, t); }' % (
            rd(ops[0], 4), nxt)
    if mn == 'ret':
        n = ops[0][1] if ops else 0
        return 'ESP += %du; return;' % (4 + n)
    if mn == 'jmp':
        if ops[0][0] == 'imm':
            return goto(fn, ops[0][1] & 0xffffffff)
        tbl = jump_table(i)
        if tbl:
            tg = table_targets(tbl)
            cases = ' '.join('case 0x%xu: %s' % (t, goto(fn, t))
                             for t in sorted(set(tg)))
            return ('{ u32 t = %s; switch (t) { %s default: '
                    'guest_bad_jump(c, 0x%xu, t); } }') % (rd(ops[0], 4), cases,
                                                         i.addr)
        return '{ u32 t = %s; call_indirect(c, t); return; }' % rd(ops[0], 4)
    if is_jcc(mn):
        return 'if (COND_%s) %s' % (CC[mn[1:]], goto(fn, ops[0][1] & 0xffffffff))
    if mn == 'jecxz':
        return 'if (!ECX) %s' % goto(fn, ops[0][1] & 0xffffffff)
    if mn == 'loop':
        return 'if (--ECX) %s' % goto(fn, ops[0][1] & 0xffffffff)
    raise Unsupported('mnemonic')


def goto(fn, t):
    if t in fn.body:
        fn.labels.add(t)
        return 'goto L_%08x;' % t
    fn.calls.add(t)
    return '{ sub_%08x(c); return; }' % t


# ---------------------------------------------------------------- functions
class Function:
    def __init__(self, entry):
        self.entry = entry
        self.body = {}      # addr -> Insn
        self.labels = set()
        self.calls = set()
        self.ok = True


entries = set()
functions = {}


def successors(i):
    """Intra-procedural successors; other-function targets become tail calls."""
    mn = i.mn
    nxt = i.addr + i.len
    if mn in ('ret', 'int3', '(bad)'):
        return []
    if mn == 'jmp':
        t = branch_target(i)
        if t is not None:
            return [t]
        tbl = jump_table(i) if i.ops else None
        return table_targets(tbl) if tbl else []
    if is_jcc(mn) or mn in ('jecxz', 'loop'):
        return [nxt, branch_target(i)]
    return [nxt]


def discover(entry):
    fn = Function(entry)
    work = [entry]
    while work:
        a = work.pop()
        if a in fn.body:
            continue
        if a != entry and a in entries:
            continue  # reached another function's entry: tail call
        i = insn_at(a)
        if i is None:
            fn.ok = False
            continue
        fn.body[a] = i
        if i.mn == 'call':
            t = branch_target(i)
            if in_text(t):
                new_entry(t)
        for s in successors(i):
            if s is not None:
                work.append(s)
    return fn


pending = []


def new_entry(a):
    if a not in entries:
        entries.add(a)
        pending.append(a)


new_entry(ENTRY)
# Pointers stored in data sections (vtables, callbacks, EH tables).
for name, sva, vs, rp, rs in sections:
    if name in ('.rdata', '.data'):
        for off in range(0, min(vs, rs) - 3, 4):
            v = struct.unpack_from('<I', data, rp + off)[0]
            if in_text(v):
                new_entry(v)
if EXTRA_SEEDS and os.path.exists(EXTRA_SEEDS):
    for line in open(EXTRA_SEEDS):
        line = line.split('#')[0].strip()
        if line:
            new_entry(int(line, 16))

scanned = set()
while pending:
    e = pending.pop()
    fn = functions[e] = discover(e)
    # Code addresses used as immediates (push offset fn / mov [x], offset fn).
    for i in fn.body.values():
        if i.addr in scanned or i.mn in ('call', 'jmp') or is_jcc(i.mn):
            continue
        scanned.add(i.addr)
        for o_ in i.ops:
            if '[' in o_:
                continue
            m = re.match(r'^0x([0-9a-f]+)$', o_)
            if m and in_text(int(m.group(1), 16)):
                new_entry(int(m.group(1), 16))

# A function discovered early may have absorbed code that later turned out to
# be another function's entry; rebuild until stable.
changed = True
while changed:
    changed = False
    for e in sorted(functions):
        fn = functions[e]
        if any(a != e and a in entries for a in fn.body):
            functions[e] = discover(e)
            changed = True
    while pending:
        e = pending.pop()
        functions[e] = discover(e)
        changed = True

# ---------------------------------------------------------------- emit
unsupported = {}
chunks = []
cur, cur_n = [], 0
total = 0
for e in sorted(functions):
    fn = functions[e]
    lines = []
    for a in sorted(fn.body):
        i = fn.body[a]
        try:
            code = gen(i, fn)
        except (Unsupported, KeyError, IndexError, ValueError) as ex:
            key = '%s (%s)' % (i.mn, ex)
            unsupported.setdefault(key, []).append(i)
            code = 'guest_unimpl(c, 0x%xu, "%s");' % (a, i.text.replace('"', "'"))
        lines.append((a, code, i))
    out = ['void sub_%08x(CPU *c) {' % e]
    if lines and lines[0][0] != e:
        # Code reached only by jumps can sit below the entry point.
        fn.labels.add(e)
        out.append('    goto L_%08x;' % e)
    for a, code, i in lines:
        if a in fn.labels or a == e:
            if a in fn.labels:
                out.append('L_%08x:' % a)
            out.append('    T(0x%xu);' % a)
        out.append('    %s // %x: %s' % (code, a, i.text))
        nxt = a + i.len
        falls = i.mn not in ('jmp', 'ret', 'int3', '(bad)') and 'guest_unimpl' not in code
        if falls and nxt not in fn.body:
            if nxt in functions:
                out.append('    sub_%08x(c); return;' % nxt)
            else:
                out.append('    guest_bad_jump(c, 0x%xu, 0x%xu);' % (a, nxt))
    out.append('}')
    out.append('')
    total += len(lines)
    cur.extend(out)
    cur_n += len(lines)
    if cur_n > 6000:
        chunks.append(cur)
        cur, cur_n = [], 0


if cur:
    chunks.append(cur)
for n, ch in enumerate(chunks):
    with open(os.path.join(OUT, 'lf2_%02d.c' % n), 'w') as f:
        f.write('#include "cpu.h"\n#include "lf2_funcs.h"\n\n')
        f.write('\n'.join(ch))
for n in range(len(chunks), 100):
    p = os.path.join(OUT, 'lf2_%02d.c' % n)
    if os.path.exists(p):
        os.remove(p)

missing = sorted(t for fn in functions.values() for t in fn.calls
                 if t not in functions)
with open(os.path.join(OUT, 'lf2_funcs.h'), 'w') as f:
    for e in sorted(set(functions) | set(missing)):
        f.write('void sub_%08x(CPU *c);\n' % e)
with open(os.path.join(OUT, 'lf2_dispatch.c'), 'w') as f:
    f.write('#include "cpu.h"\n#include "lf2_funcs.h"\n#include "image.h"\n\n')
    for t in missing:
        f.write('void sub_%08x(CPU *c) { guest_bad_jump(c, c->eip, 0x%xu); }\n'
                % (t, t))
    f.write('\nint guest_dispatch(CPU *c, u32 target) {\n    switch (target) {\n')
    for e in sorted(functions):
        f.write('    case 0x%xu: sub_%08x(c); return 1;\n' % (e, e))
    f.write('    default: return 0;\n    }\n}\n\n')
    f.write('const u32 guest_entry = 0x%xu;\nconst u32 guest_image_base = 0x%xu;\n'
            % (ENTRY, BASE))
    f.write('const struct GuestSection guest_sections[] = {\n')
    for name, sva, vs, rp, rs in sections:
        f.write('    {"%s", 0x%xu, 0x%xu, 0x%xu, 0x%xu},\n' % (name, sva, vs, rp, rs))
    f.write('    {0, 0, 0, 0, 0},\n};\n')
    f.write('const struct GuestImport guest_imports[] = {\n')
    for slot, dll, name in imports:
        f.write('    {0x%xu, "%s", "%s"},\n' % (slot, dll, name))
    f.write('    {0, 0, 0},\n};\n')

print('functions %d, instructions %d, files %d, missing targets %d' %
      (len(functions), total, len(chunks), len(missing)))
bad = [e for e, fn in functions.items() if not fn.ok]
print('functions with undecodable code: %d' % len(bad))
for k in sorted(unsupported, key=lambda k: -len(unsupported[k])):
    ex = unsupported[k][0]
    print('  unsupported x%d: %s   e.g. %x: %s' % (len(unsupported[k]), k,
                                                 ex.addr, ex.text))
