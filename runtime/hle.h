// High-level emulation of the Windows APIs the guest imports.
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "cpu.h"

typedef void (*HleFn)(CPU *c);

// On entry to an HLE function M32(ESP) is the return address and the
// arguments follow it, exactly as the guest pushed them.
#define ARG(n) M32(ESP + 4 + 4 * (n))
#define ARGP(n) ((void *)(ARG(n) ? HOSTP(ARG(n)) : 0))
#define ARGS(n) ((char *)ARGP(n))
#define RET_STD(nargs, v)                                                      \
    do {                                                                       \
        u32 v_ = (u32)(v);                                                     \
        ESP += 4 + 4 * (nargs);                                                \
        EAX = v_;                                                              \
        return;                                                                \
    } while (0)
#define RET_CDECL(v) RET_STD(0, v)

void hle_register_import(const char *dll, const char *name, HleFn fn);
void hle_register_data(const char *dll, const char *name, u32 size, u32 init);
u32 hle_thunk(HleFn fn, const char *name); // guest-callable address for fn

#define HLE_NAMED(dll, sym, ident)                                             \
    static void hle_##ident(CPU *c);                                           \
    __attribute__((constructor)) static void reg_##ident(void) {               \
        hle_register_import(#dll, sym, hle_##ident);                           \
    }                                                                          \
    static void hle_##ident(CPU *c)
#define HLE(dll, name) HLE_NAMED(dll, #name, name)

u32 guest_call(CPU *c, u32 fn, int nargs, ...);
u32 galloc(u32 size);
u32 gcalloc(u32 size);
void gfree(u32 p);
u32 gstrdup(const char *s);
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_verbose;
#define LOG(...)                                                               \
    do {                                                                       \
        if (g_verbose) logf_(__VA_ARGS__);                                     \
    } while (0)

CPU *cpu_new_thread(u32 stack_size);
extern __thread CPU *g_cpu;
extern char g_game_dir[1024];
void host_path(const char *guest, char *out, size_t n);       // for reading
void host_path_write(const char *guest, char *out, size_t n); // for creating/modifying

// ---- COM helpers (hle_user.c) ----------------------------------------------
struct Method {
    int off;
    HleFn fn;
};
u32 make_vtbl(const char *iface, int slots, const struct Method *m, int n);
#define VTBL(name, slots, table) make_vtbl(name, slots, table, (int)(sizeof(table) / sizeof(table[0])))
void com_ok0(CPU *c);
void com_ok1(CPU *c);
void com_ok2(CPU *c);
void com_ok3(CPU *c);
void com_addref(CPU *c);
void com_release(CPU *c);
void com_noiface(CPU *c);

void win_post(u32 msg, u32 wp, u32 lp);
int win_headless(void);
void audio_pump(void); // main-thread hook, called once per message pump
