// MSVCR80 / MSVCP80: C runtime and the few iostream entry points LF2 imports.
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include "hle.h"

// ---- startup --------------------------------------------------------------
static u32 g_fmode, g_commode, g_argv, g_env;

HLE(MSVCR80, __set_app_type) { RET_CDECL(0); }
HLE(MSVCR80, __p__fmode) {
    if (!g_fmode) g_fmode = gcalloc(4);
    RET_CDECL(g_fmode);
}
HLE(MSVCR80, __p__commode) {
    if (!g_commode) g_commode = gcalloc(4);
    RET_CDECL(g_commode);
}
HLE(MSVCR80, __setusermatherr) { RET_CDECL(0); }
HLE(MSVCR80, _configthreadlocale) { RET_CDECL(0); }
HLE(MSVCR80, _controlfp_s) {
    if (ARG(0)) M32(ARG(0)) = 0x9001f;
    RET_CDECL(0);
}
HLE(MSVCR80, _encode_pointer) { RET_CDECL(ARG(0)); }
HLE(MSVCR80, _decode_pointer) { RET_CDECL(ARG(0)); }
HLE(MSVCR80, _lock) { RET_CDECL(0); }
HLE(MSVCR80, _unlock) { RET_CDECL(0); }
HLE(MSVCR80, _onexit) { RET_CDECL(ARG(0)); }
HLE(MSVCR80, __dllonexit) { RET_CDECL(ARG(0)); }
HLE(MSVCR80, _crt_debugger_hook) { RET_CDECL(0); }
HLE(MSVCR80, _ismbblead) { RET_CDECL(0); }
HLE(MSVCR80, _XcptFilter) { RET_CDECL(0); }
HLE(MSVCR80, _invoke_watson) { fatal("_invoke_watson"); }
HLE(MSVCR80, _amsg_exit) { fatal("_amsg_exit(%d)", ARG(0)); }
HLE_NAMED(MSVCR80, "?terminate@@YAXXZ", terminate) { fatal("terminate()"); }
HLE(MSVCR80, __CxxFrameHandler3) { fatal("C++ exception handler invoked"); }
HLE(MSVCR80, _except_handler4_common) { fatal("SEH handler invoked"); }
HLE(MSVCR80, exit) { exit((int)ARG(0)); }
HLE(MSVCR80, _exit) { _exit((int)ARG(0)); }
HLE(MSVCR80, _cexit) { RET_CDECL(0); }

HLE(MSVCR80, _initterm) {
    for (u32 p = ARG(0); p < ARG(1); p += 4)
        if (M32(p)) guest_call(c, M32(p), 0);
    RET_CDECL(0);
}
HLE(MSVCR80, _initterm_e) {
    u32 r = 0;
    for (u32 p = ARG(0); p < ARG(1) && !r; p += 4)
        if (M32(p)) r = guest_call(c, M32(p), 0);
    RET_CDECL(r);
}
HLE(MSVCR80, __getmainargs) {
    if (!g_argv) {
        g_argv = gcalloc(8);
        M32(g_argv) = gstrdup("lf2.exe");
        g_env = gcalloc(4);
    }
    M32(ARG(0)) = 1;
    M32(ARG(1)) = g_argv;
    M32(ARG(2)) = g_env;
    RET_CDECL(0);
}
__attribute__((constructor)) static void reg_data(void) {
    hle_register_data("MSVCR80", "_adjust_fdiv", 4, 0);
    hle_register_data("MSVCR80", "_acmdln", 16, 0); // filled in by the loader
}

// ---- memory ---------------------------------------------------------------
HLE(MSVCR80, malloc) { RET_CDECL(galloc(ARG(0))); }
HLE(MSVCR80, calloc) { RET_CDECL(gcalloc(ARG(0) * ARG(1))); }
HLE(MSVCR80, free) {
    gfree(ARG(0));
    RET_CDECL(0);
}
HLE_NAMED(MSVCR80, "??2@YAPAXI@Z", op_new) { RET_CDECL(galloc(ARG(0))); }
HLE_NAMED(MSVCR80, "??_U@YAPAXI@Z", op_new_arr) { RET_CDECL(galloc(ARG(0))); }
HLE_NAMED(MSVCR80, "??3@YAXPAX@Z", op_delete) {
    gfree(ARG(0));
    RET_CDECL(0);
}
HLE(MSVCR80, memset) {
    memset(HOSTP(ARG(0)), (int)ARG(1), ARG(2));
    RET_CDECL(ARG(0));
}
HLE(MSVCR80, memcpy) {
    memmove(HOSTP(ARG(0)), HOSTP(ARG(1)), ARG(2));
    RET_CDECL(ARG(0));
}

// ---- rand: the exact MSVC generator, replays depend on it --------------------
static u32 holdrand = 1;
HLE(MSVCR80, srand) {
    holdrand = getenv("LF2_SEED") ? (u32)atoi(getenv("LF2_SEED")) : ARG(0); // fixed seed for tests
    RET_CDECL(0);
}
HLE(MSVCR80, rand) {
    holdrand = holdrand * 214013u + 2531011u;
    RET_CDECL((holdrand >> 16) & 0x7fff);
}

// ---- time -----------------------------------------------------------------
HLE(MSVCR80, _time64) {
    s64 t = (s64)time(0);
    if (ARG(0)) M64(ARG(0)) = (u64)t;
    EDX = (u32)((u64)t >> 32);
    RET_CDECL((u32)t);
}
HLE(MSVCR80, _localtime64) {
    static u32 buf;
    if (!buf) buf = gcalloc(64);
    time_t t = (time_t)(s64)M64(ARG(0));
    struct tm *tm = localtime(&t);
    int v[9] = {tm->tm_sec, tm->tm_min, tm->tm_hour, tm->tm_mday, tm->tm_mon,
                tm->tm_year, tm->tm_wday, tm->tm_yday, tm->tm_isdst};
    memcpy(HOSTP(buf), v, sizeof v);
    RET_CDECL(buf);
}

// ---- directories ----------------------------------------------------------
HLE(MSVCR80, _chdir) {
    char p[1024];
    host_path(ARGS(0), p, sizeof p);
    RET_CDECL(chdir(p) ? (u32)-1 : 0);
}
HLE(MSVCR80, _getcwd) {
    // Report a Windows-looking path; relative opens are what actually matter.
    const char *cwd = "C:\\LF2";
    u32 buf = ARG(0) ? ARG(0) : galloc(260);
    strcpy(HOSTP(buf), cwd);
    RET_CDECL(buf);
}

// ---- formatted output -----------------------------------------------------
// Formats using arguments read from guest memory starting at `ap`.
static int gvformat(char *out, size_t cap, const char *fmt, u32 ap) {
    size_t n = 0;
#define PUT(ch)                                                                \
    do {                                                                       \
        char ch_ = (ch);                                                       \
        if (n + 1 < cap) out[n] = ch_;                                         \
        n++;                                                                   \
    } while (0)
    while (*fmt) {
        if (*fmt != '%') {
            PUT(*fmt++);
            continue;
        }
        char spec[32], tmp[512];
        int k = 0;
        spec[k++] = *fmt++;
        while (*fmt && strchr("-+ #0", *fmt) && k < 20) spec[k++] = *fmt++;
        if (*fmt == '*') {
            k += sprintf(spec + k, "%d", (s32)M32(ap));
            ap += 4;
            fmt++;
        }
        while (isdigit((unsigned char)*fmt) && k < 24) spec[k++] = *fmt++;
        if (*fmt == '.') {
            spec[k++] = *fmt++;
            if (*fmt == '*') {
                k += sprintf(spec + k, "%d", (s32)M32(ap));
                ap += 4;
                fmt++;
            }
            while (isdigit((unsigned char)*fmt) && k < 28) spec[k++] = *fmt++;
        }
        int is64 = 0;
        if (!strncmp(fmt, "I64", 3)) {
            is64 = 1;
            fmt += 3;
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L') fmt++;
        char cv = *fmt ? *fmt++ : 0;
        tmp[0] = 0;
        switch (cv) {
        case '%':
            strcpy(tmp, "%");
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (is64) {
                spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = cv; spec[k] = 0;
                snprintf(tmp, sizeof tmp, spec, (unsigned long long)M64(ap));
                ap += 8;
            } else {
                spec[k++] = cv; spec[k] = 0;
                snprintf(tmp, sizeof tmp, spec, M32(ap));
                ap += 4;
            }
            break;
        case 'f': case 'e': case 'E': case 'g': case 'G':
            spec[k++] = cv; spec[k] = 0;
            snprintf(tmp, sizeof tmp, spec, MF64(ap));
            ap += 8;
            break;
        case 's': {
            spec[k++] = cv; spec[k] = 0;
            u32 p = M32(ap);
            ap += 4;
            const char *s = p ? (const char *)HOSTP(p) : "(null)";
            size_t need = (size_t)snprintf(0, 0, spec, s);
            char *big = malloc(need + 1);
            snprintf(big, need + 1, spec, s);
            for (size_t i = 0; i < need; i++) PUT(big[i]);
            free(big);
            continue;
        }
        case 'p':
            snprintf(tmp, sizeof tmp, "%08X", M32(ap));
            ap += 4;
            break;
        default:
            fatal("printf: unsupported conversion %%%c", cv);
        }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
    return (int)n;
#undef PUT
}

HLE(MSVCR80, sprintf) {
    int n = gvformat(HOSTP(ARG(0)), 1 << 20, ARGS(1), ESP + 12);
    RET_CDECL(n);
}

// ---- stdio ----------------------------------------------------------------
// A guest FILE* is the address of a small guest block; the real state is here.
typedef struct GFile {
    u32 handle;
    u8 *buf; // whole file, for reading
    size_t len, pos;
    FILE *out; // for writing
    int eof;
} GFile;
static GFile files[64];

static GFile *gfile(u32 h) {
    static GFile *last;
    if (last && last->handle == h && h) return last;
    for (int i = 0; i < 64; i++)
        if (files[i].handle == h && h) return last = &files[i];
    for (int i = 0; i < 64; i++)
        if (files[i].handle == h && h) return &files[i];
    fatal("bad FILE* %08x", h);
}

HLE(MSVCR80, fopen) {
    char path[1024];
    const char *mode = ARGS(1);
    if (mode[0] == 'r' && !strchr(mode, '+')) host_path(ARGS(0), path, sizeof path);
    else host_path_write(ARGS(0), path, sizeof path);
    GFile *g = 0;
    for (int i = 0; i < 64 && !g; i++)
        if (!files[i].handle) g = &files[i];
    if (!g) fatal("too many open files");
    memset(g, 0, sizeof *g);
    if (mode[0] == 'r') {
        FILE *f = fopen(path, "rb");
        LOG("fopen(%s, %s) -> %s", path, mode, f ? "ok" : "missing");
        if (!f) RET_CDECL(0);
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        g->buf = malloc((size_t)sz + 1);
        g->len = fread(g->buf, 1, (size_t)sz, f);
        fclose(f);
        if (!strchr(mode, 'b')) { // text mode: CRLF -> LF, stop at Ctrl-Z
            size_t w = 0;
            for (size_t r = 0; r < g->len; r++) {
                if (g->buf[r] == 0x1a) break;
                if (g->buf[r] == '\r' && r + 1 < g->len && g->buf[r + 1] == '\n')
                    continue;
                g->buf[w++] = g->buf[r];
            }
            g->len = w;
        }
    } else {
        g->out = fopen(path, mode[0] == 'a' ? "ab" : "wb");
        LOG("fopen(%s, %s) -> %s", path, mode, g->out ? "ok" : "failed");
        if (!g->out) RET_CDECL(0);
    }
    g->handle = gcalloc(32);
    RET_CDECL(g->handle);
}
HLE(MSVCR80, fclose) {
    GFile *g = gfile(ARG(0));
    if (g->out) fclose(g->out);
    free(g->buf);
    gfree(g->handle);
    g->handle = 0;
    RET_CDECL(0);
}
HLE(MSVCR80, feof) { RET_CDECL(gfile(ARG(0))->eof); }
HLE(MSVCR80, fgets) {
    GFile *g = gfile(ARG(2));
    char *dst = ARGS(0);
    int n = (int)ARG(1), k = 0;
    while (k < n - 1) {
        if (g->pos >= g->len) {
            g->eof = 1;
            break;
        }
        char ch = (char)g->buf[g->pos++];
        dst[k++] = ch;
        if (ch == '\n') break;
    }
    if (!k) RET_CDECL(0);
    dst[k] = 0;
    RET_CDECL(ARG(0));
}
HLE(MSVCR80, fprintf) {
    GFile *g = gfile(ARG(0));
    const char *f = ARGS(1);
    if (f[0] == '%' && f[1] == 'c' && !f[2]) { // the decryptor writes one char at a time
        if (g->out) fputc((int)(u8)ARG(2), g->out);
        RET_CDECL(1);
    }
    int n = gvformat(0, 0, ARGS(1), ESP + 12);
    char *b = malloc((size_t)n + 1);
    gvformat(b, (size_t)n + 1, ARGS(1), ESP + 12);
    if (g->out) fwrite(b, 1, (size_t)n, g->out);
    free(b);
    RET_CDECL(n);
}

// ---- formatted input ------------------------------------------------------
typedef struct {
    const u8 *p;
    size_t len, pos;
    int hit_eof; // a read was attempted past the end, as feof() reports
} Src;
static int sget(Src *s) {
    if (s->pos < s->len) return s->p[s->pos++];
    s->hit_eof = 1;
    return -1;
}
static int speek(Src *s) {
    if (s->pos < s->len) return s->p[s->pos];
    s->hit_eof = 1;
    return -1;
}

// Returns the number of assigned fields, or -1 on input failure before any.
static int gvscan(Src *s, const char *fmt, u32 ap) {
    int assigned = 0;
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (speek(s) >= 0 && isspace(speek(s))) sget(s);
            continue;
        }
        if (*fmt != '%') {
            if (speek(s) != (unsigned char)*fmt) return assigned ? assigned : (speek(s) < 0 ? -1 : 0);
            sget(s);
            continue;
        }
        fmt++;
        int suppress = 0, width = 0, lng = 0, shrt = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'L') {
            lng++;
            fmt++;
        }
        while (*fmt == 'h') {
            shrt++;
            fmt++;
        }
        char cv = *fmt;
        if (cv == 'c') {
            int n = width ? width : 1;
            u32 dst = suppress ? 0 : M32(ap);
            for (int i = 0; i < n; i++) {
                int ch = sget(s);
                if (ch < 0) return assigned ? assigned : -1;
                if (dst) M8(dst + i) = (u8)ch;
            }
            if (!suppress) {
                ap += 4;
                assigned++;
            }
            continue;
        }
        while (speek(s) >= 0 && isspace(speek(s))) sget(s);
        if (speek(s) < 0) return assigned ? assigned : -1;
        char tok[512];
        int k = 0, max = width ? width : 510;
        if (max > 510) max = 510;
        if (cv == 's') {
            while (k < max && speek(s) >= 0 && !isspace(speek(s))) tok[k++] = (char)sget(s);
            tok[k] = 0;
            if (!suppress) {
                strcpy(HOSTP(M32(ap)), tok);
                ap += 4;
                assigned++;
            }
            continue;
        }
        int isflt = strchr("feEgG", cv) != 0;
        if (!isflt && !strchr("diuxX", cv)) fatal("scanf: unsupported conversion %%%c", cv);
        if (k < max && (speek(s) == '+' || speek(s) == '-')) tok[k++] = (char)sget(s);
        int digits = 0;
        int hex = cv == 'x' || cv == 'X';
        while (k < max && speek(s) >= 0) {
            int ch = speek(s);
            if (isdigit(ch) || (hex && isxdigit(ch))) digits++;
            else if (isflt && (ch == '.' || ((ch == 'e' || ch == 'E') && digits))) {
                if (ch != '.') {
                    tok[k++] = (char)sget(s);
                    if (k < max && (speek(s) == '+' || speek(s) == '-')) tok[k++] = (char)sget(s);
                    continue;
                }
            } else break;
            tok[k++] = (char)sget(s);
        }
        tok[k] = 0;
        if (!digits) return assigned;
        if (!suppress) {
            u32 dst = M32(ap);
            ap += 4;
            if (isflt) {
                double v = strtod(tok, 0);
                if (lng) MF64(dst) = v;
                else MF32(dst) = (float)v;
            } else {
                long long v = hex ? (long long)strtoull(tok, 0, 16)
                                  : cv == 'u' ? (long long)strtoull(tok, 0, 10)
                                              : strtoll(tok, 0, 10);
                if (shrt) M16(dst) = (u16)v;
                else M32(dst) = (u32)v;
            }
            assigned++;
        }
    }
    return assigned;
}

HLE(MSVCR80, sscanf) {
    const char *str = ARGS(0);
    Src s = {(const u8 *)str, strlen(str), 0, 0};
    RET_CDECL(gvscan(&s, ARGS(1), ESP + 12));
}
HLE(MSVCR80, fscanf) {
    GFile *g = gfile(ARG(0));
    const char *f = ARGS(1);
    if (f[0] == '%' && f[1] == 'c' && !f[2] && g->buf) { // and reads them back the same way
        if (g->pos >= g->len) {
            g->eof = 1;
            RET_CDECL((u32)-1);
        }
        M8(ARG(2)) = g->buf[g->pos++];
        RET_CDECL(1);
    }
    Src s = {g->buf, g->len, g->pos, 0};
    int r = gvscan(&s, ARGS(1), ESP + 12);
    g->pos = s.pos;
    if (s.hit_eof) g->eof = 1;
    RET_CDECL(r);
}
