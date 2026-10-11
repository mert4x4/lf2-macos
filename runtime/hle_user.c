// USER32, GDI32 and DDRAW: one window, a message queue fed by host events,
// 32-bit surfaces living in guest memory.
#include "hle.h"
#include "image.h"
#include "platform.h"

enum {
    WM_CREATE = 1, WM_DESTROY = 2, WM_ACTIVATE = 6, WM_SETFOCUS = 7,
    WM_KILLFOCUS = 8, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12,
    WM_ACTIVATEAPP = 0x1C, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102,
    WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202,
    WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205,
};

typedef struct Surface {
    u32 obj;    // guest COM object, 0 when free
    int w, h;
    u32 pixels; // guest address, 32-bit XRGB, pitch = w * 4
    int primary, has_key, refs;
    u32 key;
} Surface;

typedef struct Bitmap {
    int used, w, h;
    u32 *px; // host copy, top-down XRGB
} Bitmap;

typedef struct DC {
    int used;
    Surface *surf;
    Bitmap *bmp;
    u32 text_color, bk_color;
} DC;

#define HWND_MAIN 0x10010u
#define HDC_BASE 0x2000u
#define HBMP_BASE 0x3000u

static Surface surfaces[2048];
static Bitmap bitmaps[2048];
static DC dcs[64];
static Surface *g_primary;

static int g_window; // host window is open
static int g_win_w = 794, g_win_h = 550;
static u32 g_wndproc;
static int g_headless, g_frame, g_max_frames, g_dump_every;
static const char *g_dump_path;
static u8 key_down[256];

// ---- scripted input for automated runs: LF2_KEYS="frame:vk[:hold],..." -------
struct Script {
    int frame, vk, hold;
};
static struct Script script[256];
static int n_script;

static void load_env(void) {
    static int done;
    if (done) return;
    done = 1;
    g_headless = getenv("LF2_HEADLESS") != 0;
    g_dump_path = getenv("LF2_DUMP");
    g_dump_every = getenv("LF2_DUMP_EVERY") ? atoi(getenv("LF2_DUMP_EVERY")) : 0;
    g_max_frames = getenv("LF2_FRAMES") ? atoi(getenv("LF2_FRAMES")) : 0;
    const char *k = getenv("LF2_KEYS");
    while (k && *k && n_script < 256) {
        struct Script s = {0, 0, 3};
        char *end;
        s.frame = (int)strtol(k, &end, 10);
        if (end[0] == ':' && end[1] == 'm') { // frame:mX/Y = click at X,Y
            int x = (int)strtol(end + 2, &end, 10), y = 0;
            if (*end == '/') y = (int)strtol(end + 1, &end, 10);
            s.vk = 0x10000 | x | (y << 17);
        } else {
            if (*end == ':') s.vk = (int)strtol(end + 1, &end, 0);
            if (*end == ':') s.hold = (int)strtol(end + 1, &end, 10);
        }
        script[n_script++] = s;
        k = *end == ',' ? end + 1 : end;
        if (end == k && *k) break;
    }
}

// ---- message queue ----------------------------------------------------------
typedef struct Msg {
    u32 hwnd, msg, wp, lp;
} Msg;
static Msg queue[512];
static int q_head, q_tail;

static void post(u32 msg, u32 wp, u32 lp) {
    if (msg == WM_MOUSEMOVE && q_head != q_tail) { // coalesce pointer motion
        Msg *last = &queue[(q_tail + 511) % 512];
        if (last->msg == WM_MOUSEMOVE) {
            last->lp = lp;
            return;
        }
    }
    int n = (q_tail + 1) % 512;
    if (n == q_head) return;
    queue[q_tail] = (Msg){HWND_MAIN, msg, wp, lp};
    q_tail = n;
}

void win_post(u32 msg, u32 wp, u32 lp) { post(msg, wp, lp); }
int win_headless(void) {
    load_env();
    return g_headless;
}

static u64 g_key_time; // when the oldest not-yet-displayed key event arrived

static void key_event(int vk, int down) {
    if (!vk) return;
    if (!g_key_time) g_key_time = plat_micros();
    key_down[vk & 255] = (u8)down;
    post(down ? WM_KEYDOWN : WM_KEYUP, (u32)vk, down ? 1 : 0xC0000001u);
}

static void pump(void) {
    for (int i = 0; i < n_script; i++) {
        if (script[i].vk & 0x10000 && script[i].vk > 0) {
            u32 lp = (u32)(script[i].vk & 0xffff) | ((u32)(script[i].vk >> 17) << 16);
            int *stage = &script[i].hold; // 3 = not started
            if (*stage == 3 && g_frame >= script[i].frame) {
                post(WM_MOUSEMOVE, 0, lp);
                *stage = 4;
            } else if (*stage == 4 && g_frame >= script[i].frame + 2) {
                post(WM_LBUTTONDOWN, 1, lp);
                *stage = 5;
            } else if (*stage == 5 && g_frame >= script[i].frame + 4) {
                post(WM_LBUTTONUP, 0, lp);
                script[i].vk = 0;
            }
        } else if (script[i].frame == g_frame && script[i].vk > 0) {
            key_event(script[i].vk, 1);
            script[i].vk = -script[i].vk;
        } else if (script[i].vk < 0 && g_frame >= script[i].frame + script[i].hold) {
            key_event(-script[i].vk, 0);
            script[i].vk = 0;
        }
    }
    audio_pump();
    if (g_headless) return;
    PlatEvent e;
    while (plat_poll(&e)) {
        u32 pos = (u32)(e.x & 0xffff) | ((u32)e.y << 16);
        switch (e.type) {
        case PLAT_CLOSE:
            post(WM_CLOSE, 0, 0);
            break;
        case PLAT_KEY:
            key_event(e.vk, e.down);
            break;
        case PLAT_MOUSE_MOVE:
            post(WM_MOUSEMOVE, 0, pos);
            break;
        case PLAT_MOUSE_BUTTON:
            post(e.left ? (e.down ? WM_LBUTTONDOWN : WM_LBUTTONUP)
                        : (e.down ? WM_RBUTTONDOWN : WM_RBUTTONUP),
                 0, pos);
            break;
        case PLAT_FOCUS:
            if (e.down) {
                post(WM_ACTIVATEAPP, 1, 0);
                post(WM_ACTIVATE, 1, 0);
            } else {
                memset(key_down, 0, sizeof key_down);
                post(WM_ACTIVATE, 0, 0);
                post(WM_ACTIVATEAPP, 0, 0);
            }
            break;
        }
    }
}

static void write_msg(u32 p, Msg *m) {
    M32(p) = m->hwnd;
    M32(p + 4) = m->msg;
    M32(p + 8) = m->wp;
    M32(p + 12) = m->lp;
    M32(p + 16) = plat_ticks();
    M32(p + 20) = 0;
    M32(p + 24) = 0;
}

// ---- presentation -----------------------------------------------------------
static void save_bmp(const char *path, const u32 *px, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    u32 size = (u32)w * (u32)h * 4, hdr[13] = {0};
    hdr[0] = 54 + size;        // file size
    hdr[2] = 54;               // pixel data offset
    hdr[3] = 40;               // BITMAPINFOHEADER
    hdr[4] = (u32)w;
    hdr[5] = (u32)-h;          // top-down
    hdr[6] = 1 | (32u << 16);  // planes, bits per pixel
    hdr[8] = size;
    fwrite("BM", 1, 2, f);
    fwrite(hdr, 4, 13, f);
    fwrite(px, 4, (size_t)w * (size_t)h, f);
    fclose(f);
}
static void present(void) {
    if (!g_primary) return;
    g_frame++;
    if (g_dump_path && g_dump_every && g_frame % g_dump_every == 0 &&
        g_frame >= (getenv("LF2_DUMP_FROM") ? atoi(getenv("LF2_DUMP_FROM")) : 0)) {
        char name[1200];
        snprintf(name, sizeof name, "%s_%05d.bmp", g_dump_path, g_frame);
        save_bmp(name, HOSTP(g_primary->pixels), g_primary->w, g_primary->h);
    }
    if (g_max_frames && g_frame >= g_max_frames) {
        logf_("LF2_FRAMES reached (%d), exiting", g_frame);
        exit(0);
    }
    if (g_headless || !g_window) return;
    if (!g_verbose) g_key_time = 0;
    u64 t0 = plat_micros();
    plat_present(HOSTP(g_primary->pixels), g_primary->w, g_primary->h);
    if (g_verbose) { // frame pacing statistics, once every 100 frames
        static u64 last, sum_present, sum_frame, max_present, max_frame;
        u64 t1 = plat_micros();
        if (g_key_time) logf_("key -> next presented frame: %.1f ms", (t1 - g_key_time) / 1000.0);
        u64 dp = t1 - t0, df = last ? t1 - last : 0;
        last = t1;
        sum_present += dp;
        sum_frame += df;
        if (dp > max_present) max_present = dp;
        if (df > max_frame) max_frame = df;
        if (g_frame % 100 == 0) {
            logf_("frames: interval avg %.1f ms max %.1f ms, present avg %.1f ms max %.1f ms",
                  sum_frame / 100000.0, max_frame / 1000.0, sum_present / 100000.0,
                  max_present / 1000.0);
            sum_present = sum_frame = max_present = max_frame = 0;
        }
        g_key_time = 0;
    }
}

// ---- USER32 -----------------------------------------------------------------
HLE(USER32, GetSystemMetrics) {
    u32 i = ARG(0);
    RET_STD(1, i == 0 ? 1920 : i == 1 ? 1080 : 0);
}
HLE(USER32, LoadIconA) { RET_STD(2, 1); }
HLE(USER32, LoadCursorA) { RET_STD(2, 1); }
HLE(USER32, SetCursor) { RET_STD(1, 1); }
HLE(USER32, RegisterClassA) {
    g_wndproc = M32(ARG(0) + 4);
    LOG("RegisterClassA wndproc=%08x", g_wndproc);
    RET_STD(1, 0xC001);
}
HLE(USER32, CreateWindowExA) {
    load_env();
    int w = (int)ARG(6), h = (int)ARG(7);
    if (w > 0 && w < 4096) g_win_w = w;
    if (h > 0 && h < 4096) g_win_h = h;
    LOG("CreateWindowExA \"%s\" %dx%d style=%08x", ARGS(2), g_win_w, g_win_h, ARG(3));
    if (!g_headless && !g_window) {
        // Presentation is not synced to the display: the game paces itself at
        // 30 fps and waiting for vsync would only add latency.
        plat_window_open(ARGS(2), g_win_w, g_win_h);
        g_window = 1;
    }
    u32 sp = ESP;
    u32 cs = gcalloc(64);
    guest_call(c, g_wndproc, 4, HWND_MAIN, WM_CREATE, 0, cs);
    gfree(cs);
    ESP = sp;
    post(WM_ACTIVATEAPP, 1, 0);
    post(WM_ACTIVATE, 1, 0);
    post(WM_SETFOCUS, 0, 0);
    post(5 /* WM_SIZE */, 0, (u32)g_win_w | ((u32)g_win_h << 16));
    post(3 /* WM_MOVE */, 0, 0);
    RET_STD(12, HWND_MAIN);
}
HLE(USER32, ShowWindow) { RET_STD(2, 0); }
HLE(USER32, UpdateWindow) { RET_STD(1, 1); }
HLE(USER32, InvalidateRect) { RET_STD(3, 1); }
HLE(USER32, DestroyWindow) {
    guest_call(c, g_wndproc, 4, HWND_MAIN, WM_DESTROY, 0, 0);
    RET_STD(1, 1);
}
HLE(USER32, MessageBoxA) {
    const char *title = ARG(2) ? ARGS(2) : "", *text = ARG(1) ? ARGS(1) : "";
    u32 type = ARG(3) & 15;
    logf_("MessageBox [%s]: %s", title, text);
    u32 def = type == 4 || type == 3 ? 6 : 1; // IDYES for yes/no boxes, else IDOK
    if (g_headless || !g_window) RET_STD(4, def);
    int hit = plat_message_box(title, text, type == 4 || type == 3 ? PLAT_BOX_YESNO
                                            : type == 1 ? PLAT_BOX_OKCANCEL : PLAT_BOX_OK);
    memset(key_down, 0, sizeof key_down);
    RET_STD(4, hit);
}
HLE(USER32, GetKeyState) {
    u32 vk = ARG(0) & 255;
    RET_STD(1, key_down[vk] ? 0xFFFF8000u : 0);
}
HLE(USER32, PostMessageA) {
    post(ARG(1), ARG(2), ARG(3));
    RET_STD(4, 1);
}
HLE(USER32, PostQuitMessage) {
    post(WM_QUIT, ARG(0), 0);
    RET_STD(1, 0);
}
HLE(USER32, DefWindowProcA) {
    if (ARG(1) == WM_CLOSE) {
        guest_call(c, g_wndproc, 4, HWND_MAIN, WM_DESTROY, 0, 0);
    }
    RET_STD(4, 0);
}
HLE(USER32, ClientToScreen) { RET_STD(2, 1); }
HLE(USER32, GetClientRect) {
    u32 r = ARG(1);
    M32(r) = 0;
    M32(r + 4) = 0;
    M32(r + 8) = (u32)g_win_w;
    M32(r + 12) = (u32)g_win_h;
    RET_STD(2, 1);
}
HLE(USER32, SetRect) {
    u32 r = ARG(0);
    M32(r) = ARG(1);
    M32(r + 4) = ARG(2);
    M32(r + 8) = ARG(3);
    M32(r + 12) = ARG(4);
    RET_STD(5, 1);
}
HLE(USER32, PeekMessageA) {
    pump();
    if (q_head == q_tail) RET_STD(5, 0);
    write_msg(ARG(0), &queue[q_head]);
    if (ARG(4) & 1) q_head = (q_head + 1) % 512;
    RET_STD(5, 1);
}
HLE(USER32, GetMessageA) {
    for (;;) {
        pump();
        if (q_head != q_tail) break;
        plat_delay(1);
        if (g_headless) g_frame++; // let scripted runs make progress while idle
        if (g_max_frames && g_frame >= g_max_frames) exit(0);
    }
    Msg m = queue[q_head];
    q_head = (q_head + 1) % 512;
    write_msg(ARG(0), &m);
    RET_STD(4, m.msg != WM_QUIT);
}
HLE(USER32, TranslateMessage) { RET_STD(1, 0); }
HLE(USER32, DispatchMessageA) {
    u32 m = ARG(0);
    u32 r = guest_call(c, g_wndproc, 4, M32(m), M32(m + 4), M32(m + 8), M32(m + 12));
    RET_STD(1, r);
}

// ---- bitmaps ----------------------------------------------------------------
static u32 find_resource(u32 type, u32 name_arg) {
    u32 root = 0;
    for (const struct GuestSection *s = guest_sections; s->name; s++)
        if (!strcmp(s->name, ".rsrc")) root = s->va;
    if (!root) return 0;
    u32 dir = root, want[2] = {type, name_arg};
    for (int level = 0; level < 3; level++) {
        u32 named = M16(dir + 12), ids = M16(dir + 14), found = 0;
        for (u32 i = 0; i < named + ids && !found; i++) {
            u32 e = dir + 16 + 8 * i, id = M32(e), off = M32(e + 4);
            int match = level == 2;
            if (!match && (id & 0x80000000u)) {
                if (want[level] > 0xffff) { // compare with the name string
                    u32 s = root + (id & 0x7fffffffu);
                    const char *nm = HOSTP(want[level]);
                    u32 len = M16(s);
                    match = strlen(nm) == len;
                    for (u32 k = 0; match && k < len; k++)
                        match = (M16(s + 2 + 2 * k) | 0x20) == ((u8)nm[k] | 0x20);
                }
            } else if (!match) {
                match = id == want[level];
            }
            if (match) found = off;
        }
        if (!found) return 0;
        if (!(found & 0x80000000u)) return root + found; // data entry
        dir = root + (found & 0x7fffffffu);
    }
    return 0;
}

// Decodes a DIB (BITMAPINFOHEADER + palette + bits) into a new Bitmap.
static Bitmap *decode_dib(const u8 *hdr, const u8 *bits, const char *what) {
    u32 hsize, comp, clr;
    s32 w, h;
    u16 bpp;
    memcpy(&hsize, hdr, 4);
    memcpy(&w, hdr + 4, 4);
    memcpy(&h, hdr + 8, 4);
    memcpy(&bpp, hdr + 14, 2);
    memcpy(&comp, hdr + 16, 4);
    memcpy(&clr, hdr + 32, 4);
    if (comp != 0 && !(comp == 1 && bpp == 8))
        fatal("bitmap %s: compression %u unsupported", what, comp);
    const u8 *pal = hdr + hsize;
    if (bpp <= 8 && !clr) clr = 1u << bpp;
    if (!bits) bits = pal + (bpp <= 8 ? clr * 4 : 0);
    int flip = h > 0;
    if (h < 0) h = -h;
    u8 *rle = 0;
    if (comp == 1) { // BI_RLE8 -> plain 8-bit rows
        u32 st = ((u32)w + 3) & ~3u;
        rle = calloc((size_t)st, (size_t)h + 1);
        const u8 *p = bits;
        int x = 0, y = 0;
        while (y < h) {
            u8 n = *p++, v = *p++;
            if (n) {
                while (n--) {
                    if (x < w) rle[(size_t)y * st + (size_t)x] = v;
                    x++;
                }
            } else if (v == 0) {
                x = 0;
                y++;
            } else if (v == 1) {
                break;
            } else if (v == 2) {
                x += *p++;
                y += *p++;
            } else {
                for (int k = 0; k < v; k++, x++)
                    if (x < w && y < h) rle[(size_t)y * st + (size_t)x] = p[k];
                p += (v + 1) & ~1;
            }
        }
        bits = rle;
    }
    Bitmap *b = 0;
    for (int i = 1; i < 2048 && !b; i++)
        if (!bitmaps[i].used) b = &bitmaps[i];
    if (!b) fatal("too many bitmaps");
    b->used = 1;
    b->w = w;
    b->h = h;
    b->px = malloc((size_t)w * (size_t)h * 4 + 4);
    u32 stride = (((u32)w * bpp + 31) / 32) * 4;
    for (int y = 0; y < h; y++) {
        const u8 *row = bits + (size_t)stride * (size_t)(flip ? h - 1 - y : y);
        u32 *dst = b->px + (size_t)y * (size_t)w;
        for (int x = 0; x < w; x++) {
            u32 px;
            switch (bpp) {
            case 1: {
                const u8 *p = pal + 4 * ((row[x >> 3] >> (7 - (x & 7))) & 1);
                px = p[0] | (p[1] << 8) | (p[2] << 16);
                break;
            }
            case 4: {
                const u8 *p = pal + 4 * ((row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15);
                px = p[0] | (p[1] << 8) | (p[2] << 16);
                break;
            }
            case 8: {
                const u8 *p = pal + 4 * row[x];
                px = p[0] | (p[1] << 8) | (p[2] << 16);
                break;
            }
            case 16: {
                u32 v = row[2 * x] | (row[2 * x + 1] << 8);
                px = ((v & 0x7c00) << 9) | ((v & 0x3e0) << 6) | ((v & 0x1f) << 3);
                break;
            }
            case 24:
                px = row[3 * x] | (row[3 * x + 1] << 8) | (row[3 * x + 2] << 16);
                break;
            case 32:
                px = row[4 * x] | (row[4 * x + 1] << 8) | (row[4 * x + 2] << 16);
                break;
            default:
                fatal("bitmap %s: %d bpp unsupported", what, bpp);
            }
            dst[x] = px;
        }
    }
    free(rle);
    return b;
}

static Bitmap *get_bitmap(u32 h) {
    u32 i = h - HBMP_BASE;
    return (i > 0 && i < 2048 && bitmaps[i].used) ? &bitmaps[i] : 0;
}

HLE(USER32, LoadImageA) {
    u32 name = ARG(1), type = ARG(2), flags = ARG(5);
    if (type != 0) RET_STD(6, 0); // only IMAGE_BITMAP
    Bitmap *b = 0;
    if (flags & 0x10) { // LR_LOADFROMFILE
        char path[1024];
        host_path(HOSTP(name), path, sizeof path);
        FILE *f = fopen(path, "rb");
        if (!f) {
            LOG("LoadImageA: missing file %s", path);
            RET_STD(6, 0);
        }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        u8 *buf = malloc((size_t)sz);
        if (fread(buf, 1, (size_t)sz, f) != (size_t)sz || sz < 54 || buf[0] != 'B') {
            fclose(f);
            free(buf);
            LOG("LoadImageA: bad bitmap file %s", path);
            RET_STD(6, 0);
        }
        fclose(f);
        u32 off;
        memcpy(&off, buf + 10, 4);
        b = decode_dib(buf + 14, buf + off, path);
        free(buf);
    } else {
        u32 e = find_resource(2, name);
        if (!e) {
            if (name > 0xffff) LOG("LoadImageA: no bitmap resource \"%s\"", (char *)HOSTP(name));
            else LOG("LoadImageA: no bitmap resource #%u", name);
            RET_STD(6, 0);
        }
        b = decode_dib(HOSTP(guest_image_base + M32(e)), 0, "resource");
    }
    RET_STD(6, HBMP_BASE + (u32)(b - bitmaps));
}

// ---- GDI32 ------------------------------------------------------------------
static DC *get_dc(u32 h) {
    u32 i = h - HDC_BASE;
    return (i < 64 && dcs[i].used) ? &dcs[i] : 0;
}
static u32 new_dc(Surface *s) {
    for (int i = 1; i < 64; i++)
        if (!dcs[i].used) {
            dcs[i] = (DC){1, s, 0, 0, 0xffffff};
            return HDC_BASE + (u32)i;
        }
    fatal("too many DCs");
}
static u32 colorref_to_px(u32 cr) {
    return ((cr & 0xff) << 16) | (cr & 0xff00) | ((cr >> 16) & 0xff);
}

HLE(GDI32, CreateCompatibleDC) { RET_STD(1, new_dc(0)); }
HLE(GDI32, DeleteDC) {
    DC *d = get_dc(ARG(0));
    if (d) d->used = 0;
    RET_STD(1, 1);
}
HLE(GDI32, SelectObject) {
    DC *d = get_dc(ARG(0));
    Bitmap *b = get_bitmap(ARG(1));
    u32 old = 0;
    if (d && b) {
        old = d->bmp ? HBMP_BASE + (u32)(d->bmp - bitmaps) : HBMP_BASE;
        d->bmp = b;
    } else if (d && ARG(1) == HBMP_BASE) {
        d->bmp = 0;
    }
    RET_STD(2, old);
}
HLE(GDI32, DeleteObject) {
    Bitmap *b = get_bitmap(ARG(0));
    if (b) {
        free(b->px);
        b->used = 0;
    }
    RET_STD(1, 1);
}
HLE(GDI32, GetObjectA) {
    Bitmap *b = get_bitmap(ARG(0));
    u32 p = ARG(2);
    if (!b || ARG(1) < 24) RET_STD(3, 0);
    M32(p) = 0;
    M32(p + 4) = (u32)b->w;
    M32(p + 8) = (u32)b->h;
    M32(p + 12) = (u32)b->w * 4;
    M16(p + 16) = 1;
    M16(p + 18) = 32;
    M32(p + 20) = 0;
    RET_STD(3, 24);
}
HLE(GDI32, SetBkColor) {
    DC *d = get_dc(ARG(0));
    if (d) d->bk_color = ARG(1);
    RET_STD(2, 0);
}
HLE(GDI32, SetTextColor) {
    DC *d = get_dc(ARG(0));
    if (d) d->text_color = ARG(1);
    RET_STD(2, 0);
}

void text_draw(u32 *px, int w, int h, int x, int y, const char *s, int n, u32 color,
               u32 bk);

HLE(GDI32, TextOutA) {
    DC *d = get_dc(ARG(0));
    if (d && d->surf)
        text_draw(HOSTP(d->surf->pixels), d->surf->w, d->surf->h, (int)ARG(1), (int)ARG(2),
                  ARGS(3), (int)ARG(4), colorref_to_px(d->text_color),
                  colorref_to_px(d->bk_color));
    RET_STD(5, 1);
}

// Scaled copy with optional colour key and mirroring; clips to the destination.
static void blit(u32 *dp, int dW, int dH, int dx, int dy, int dw, int dh,
                 const u32 *sp, int sW, int sH, int sx, int sy, int sw, int sh,
                 int use_key, u32 key, int mirror_x, int mirror_y) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    key &= 0xffffff;
    for (int y = 0; y < dh; y++) {
        int ty = dy + y;
        if (ty < 0 || ty >= dH) continue;
        int fy = dh == sh ? y : (int)((s64)y * sh / dh);
        if (mirror_y) fy = sh - 1 - fy;
        fy += sy;
        if (fy < 0 || fy >= sH) continue;
        const u32 *srow = sp + (size_t)fy * (size_t)sW;
        u32 *drow = dp + (size_t)ty * (size_t)dW;
        for (int x = 0; x < dw; x++) {
            int tx = dx + x;
            if (tx < 0 || tx >= dW) continue;
            int fx = dw == sw ? x : (int)((s64)x * sw / dw);
            if (mirror_x) fx = sw - 1 - fx;
            fx += sx;
            if (fx < 0 || fx >= sW) continue;
            u32 v = srow[fx] & 0xffffff;
            if (use_key && v == key) continue;
            drow[tx] = v;
        }
    }
}

HLE(GDI32, StretchBlt) {
    DC *d = get_dc(ARG(0)), *s = get_dc(ARG(5));
    if (!d || !s) RET_STD(11, 0);
    u32 *dp, *sp;
    int dW, dH, sW, sH;
    if (d->surf) dp = HOSTP(d->surf->pixels), dW = d->surf->w, dH = d->surf->h;
    else if (d->bmp) dp = d->bmp->px, dW = d->bmp->w, dH = d->bmp->h;
    else RET_STD(11, 0);
    if (s->surf) sp = HOSTP(s->surf->pixels), sW = s->surf->w, sH = s->surf->h;
    else if (s->bmp) sp = s->bmp->px, sW = s->bmp->w, sH = s->bmp->h;
    else RET_STD(11, 0);
    blit(dp, dW, dH, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), sp, sW, sH,
         (int)ARG(6), (int)ARG(7), (int)ARG(8), (int)ARG(9), 0, 0, 0, 0);
    RET_STD(11, 1);
}

// ---- COM plumbing -----------------------------------------------------------
u32 make_vtbl(const char *iface, int slots, const struct Method *m, int n) {
    u32 v = gcalloc((u32)slots * 4);
    for (int i = 0; i < slots; i++) {
        HleFn fn = 0;
        for (int k = 0; k < n; k++)
            if (m[k].off == i * 4) fn = m[k].fn;
        char *label = malloc(strlen(iface) + 16);
        sprintf(label, "%s+0x%x", iface, i * 4);
        M32(v + 4 * (u32)i) = hle_thunk(fn, label);
    }
    return v;
}

void com_ok0(CPU *c) { RET_STD(1, 0); }
void com_ok1(CPU *c) { RET_STD(2, 0); }
void com_ok2(CPU *c) { RET_STD(3, 0); }
void com_ok3(CPU *c) { RET_STD(4, 0); }
void com_addref(CPU *c) { RET_STD(1, 2); }
void com_release(CPU *c) { RET_STD(1, 0); }
void com_noiface(CPU *c) {
    M32(ARG(2)) = 0;
    RET_STD(3, 0x80004002u);
}

// ---- IDirectDrawSurface -----------------------------------------------------
static u32 surface_vtbl(void);
static Surface *surf_of(u32 obj) {
    u32 i = obj ? M32(obj + 4) : 0;
    if (!i || i >= 2048 || surfaces[i].obj != obj) fatal("bad surface %08x", obj);
    return &surfaces[i];
}
static Surface *new_surface(int w, int h, int primary) {
    for (int i = 1; i < 2048; i++)
        if (!surfaces[i].obj) {
            Surface *s = &surfaces[i];
            memset(s, 0, sizeof *s);
            s->obj = gcalloc(8);
            M32(s->obj) = surface_vtbl();
            M32(s->obj + 4) = (u32)i;
            s->w = w;
            s->h = h;
            s->pixels = gcalloc((u32)w * (u32)h * 4 + 16);
            s->primary = primary;
            s->refs = 1;
            if (primary) g_primary = s;
            return s;
        }
    fatal("too many surfaces");
}
static void fill_pixel_format(u32 p) {
    M32(p) = 32;
    M32(p + 4) = 0x40; // DDPF_RGB
    M32(p + 8) = 0;
    M32(p + 12) = 32;
    M32(p + 16) = 0xff0000;
    M32(p + 20) = 0x00ff00;
    M32(p + 24) = 0x0000ff;
    M32(p + 28) = 0;
}
static void fill_desc(Surface *s, u32 d) {
    M32(d + 4) = 0x100f; // CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT
    M32(d + 8) = (u32)s->h;
    M32(d + 12) = (u32)s->w;
    M32(d + 16) = (u32)s->w * 4;
    M32(d + 0x24) = s->pixels;
    fill_pixel_format(d + 0x48);
    M32(d + 0x68) = s->primary ? 0x200 : 0x40;
}
static void rect_or_full(u32 r, int W, int H, int *x, int *y, int *w, int *h) {
    if (r) {
        *x = (s32)M32(r);
        *y = (s32)M32(r + 4);
        *w = (s32)M32(r + 8) - *x;
        *h = (s32)M32(r + 12) - *y;
    } else {
        *x = *y = 0;
        *w = W;
        *h = H;
    }
}

static void dds_Release(CPU *c) {
    Surface *s = surf_of(ARG(0));
    if (--s->refs <= 0) {
        if (g_primary == s) g_primary = 0;
        gfree(s->pixels);
        gfree(s->obj);
        s->obj = 0;
        RET_STD(1, 0);
    }
    RET_STD(1, s->refs);
}
static void dds_AddRef(CPU *c) { RET_STD(1, ++surf_of(ARG(0))->refs); }
static void dds_Blt(CPU *c) {
    Surface *d = surf_of(ARG(0));
    u32 flags = ARG(4), fx = ARG(5);
    int dx, dy, dw, dh;
    rect_or_full(ARG(1), d->w, d->h, &dx, &dy, &dw, &dh);
    if (g_verbose > 2)
        logf_("  Blt dst#%u%s (%d,%d %dx%d) src#%u rect=%d flags=%08x fill=%06x",
              M32(ARG(0) + 4), d->primary ? "P" : "", dx, dy, dw, dh,
              ARG(2) ? M32(ARG(2) + 4) : 0, ARG(3) != 0, flags, fx ? M32(fx + 0x50) : 0);
    if (flags & 0x400) { // DDBLT_COLORFILL
        u32 color = M32(fx + 0x50) & 0xffffff, *p = HOSTP(d->pixels);
        for (int y = dy < 0 ? 0 : dy; y < dy + dh && y < d->h; y++)
            for (int x = dx < 0 ? 0 : dx; x < dx + dw && x < d->w; x++)
                p[(size_t)y * (size_t)d->w + (size_t)x] = color;
    } else if (ARG(2)) {
        Surface *s = surf_of(ARG(2));
        int sx, sy, sw, sh;
        rect_or_full(ARG(3), s->w, s->h, &sx, &sy, &sw, &sh);
        u32 ddfx = (flags & 0x800) && fx ? M32(fx + 4) : 0;
        blit(HOSTP(d->pixels), d->w, d->h, dx, dy, dw, dh, HOSTP(s->pixels), s->w, s->h,
             sx, sy, sw, sh, (flags & 0x8000) && s->has_key, s->key, ddfx & 2, ddfx & 4);
    }
    if (d->primary) present();
    RET_STD(6, 0);
}
static void dds_BltFast(CPU *c) {
    Surface *d = surf_of(ARG(0)), *s = surf_of(ARG(3));
    int sx, sy, sw, sh;
    rect_or_full(ARG(4), s->w, s->h, &sx, &sy, &sw, &sh);
    blit(HOSTP(d->pixels), d->w, d->h, (s32)ARG(1), (s32)ARG(2), sw, sh, HOSTP(s->pixels),
         s->w, s->h, sx, sy, sw, sh, (ARG(5) & 1) && s->has_key, s->key, 0, 0);
    if (d->primary) present();
    RET_STD(6, 0);
}
static void dds_Flip(CPU *c) {
    present();
    RET_STD(3, 0);
}
static void dds_GetDC(CPU *c) {
    M32(ARG(1)) = new_dc(surf_of(ARG(0)));
    RET_STD(2, 0);
}
static void dds_ReleaseDC(CPU *c) {
    DC *d = get_dc(ARG(1));
    if (d) d->used = 0;
    if (surf_of(ARG(0))->primary) present();
    RET_STD(2, 0);
}
static void dds_GetPixelFormat(CPU *c) {
    fill_pixel_format(ARG(1));
    RET_STD(2, 0);
}
static void dds_GetSurfaceDesc(CPU *c) {
    fill_desc(surf_of(ARG(0)), ARG(1));
    RET_STD(2, 0);
}
static void dds_Lock(CPU *c) {
    fill_desc(surf_of(ARG(0)), ARG(2));
    RET_STD(5, 0);
}
static void dds_Unlock(CPU *c) {
    if (surf_of(ARG(0))->primary) present();
    RET_STD(2, 0);
}
static void dds_SetColorKey(CPU *c) {
    Surface *s = surf_of(ARG(0));
    s->has_key = ARG(2) != 0;
    if (ARG(2)) s->key = M32(ARG(2));
    RET_STD(3, 0);
}
static void dds_GetColorKey(CPU *c) {
    Surface *s = surf_of(ARG(0));
    M32(ARG(2)) = M32(ARG(2) + 4) = s->key;
    RET_STD(3, 0);
}
static u32 surface_vtbl(void) {
    static u32 v;
    static const struct Method m[] = {
        {0x00, com_noiface}, {0x04, dds_AddRef}, {0x08, dds_Release}, {0x14, dds_Blt},
        {0x1c, dds_BltFast}, {0x2c, dds_Flip}, {0x44, dds_GetDC},
        {0x54, dds_GetPixelFormat}, {0x58, dds_GetSurfaceDesc}, {0x60, com_ok0},
        {0x64, dds_Lock}, {0x68, dds_ReleaseDC}, {0x6c, com_ok0}, {0x70, com_ok1},
        {0x74, dds_SetColorKey}, {0x40, dds_GetColorKey}, {0x80, dds_Unlock},
    };
    if (!v) v = VTBL("IDirectDrawSurface", 36, m);
    return v;
}

// ---- IDirectDraw / IDirectDrawClipper ----------------------------------------
static void dd_CreateClipper(CPU *c) {
    static u32 vt;
    static const struct Method m[] = {
        {0x00, com_noiface}, {0x04, com_addref}, {0x08, com_release}, {0x20, com_ok2},
    };
    if (!vt) vt = VTBL("IDirectDrawClipper", 9, m);
    u32 obj = gcalloc(8);
    M32(obj) = vt;
    M32(ARG(2)) = obj;
    RET_STD(4, 0);
}
static void dd_CreateSurface(CPU *c) {
    u32 d = ARG(1), flags = M32(d + 4), caps = M32(d + 0x68);
    int primary = (flags & 1) && (caps & 0x200);
    int w = (flags & 4) ? (int)M32(d + 12) : g_win_w;
    int h = (flags & 2) ? (int)M32(d + 8) : g_win_h;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) RET_STD(4, 0x80070057u);
    Surface *s = new_surface(w, h, primary);
    if (flags & 0x10000) { // DDSD_CKSRCBLT
        s->has_key = 1;
        s->key = M32(d + 0x40);
    }
    M32(ARG(2)) = s->obj;
    RET_STD(4, 0);
}
static void dd_SetDisplayMode(CPU *c) {
    LOG("SetDisplayMode %ux%ux%u (ignored, staying windowed)", ARG(1), ARG(2), ARG(3));
    RET_STD(4, 0);
}
static void dd_GetDisplayMode(CPU *c) {
    u32 d = ARG(1);
    M32(d + 4) = 0x100e;
    M32(d + 8) = 1080;
    M32(d + 12) = 1920;
    M32(d + 16) = 1920 * 4;
    fill_pixel_format(d + 0x48);
    RET_STD(2, 0);
}

HLE(DDRAW, DirectDrawCreate) {
    static u32 vt;
    static const struct Method m[] = {
        {0x00, com_noiface}, {0x04, com_addref}, {0x08, com_release},
        {0x10, dd_CreateClipper}, {0x18, dd_CreateSurface}, {0x30, dd_GetDisplayMode},
        {0x4c, com_ok0}, {0x50, com_ok2}, {0x54, dd_SetDisplayMode}, {0x58, com_ok2},
    };
    if (!vt) vt = VTBL("IDirectDraw", 23, m);
    u32 obj = gcalloc(8);
    M32(obj) = vt;
    M32(ARG(1)) = obj;
    RET_STD(3, 0);
}
