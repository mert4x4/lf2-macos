// Loader and core runtime: guest memory, heap, import thunks, diagnostics.
#include <errno.h>
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <pthread.h>
#include <signal.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include "hle.h"
#include "image.h"

#define HLE_BASE 0xF0000000u
#define MAX_THUNKS 4096
#define HEAP_LO 0x10000000u
#define HEAP_HI 0xE0000000u
#define RET_MAGIC 0xEFFF0000u

int g_verbose;
__thread CPU *g_cpu;
char g_game_dir[1024];

static HleFn thunk_fn[MAX_THUNKS];
static const char *thunk_name[MAX_THUNKS];
static int n_thunks;

struct Reg_ {
    const char *dll, *name;
    HleFn fn;
    u32 data_size, data_init;
};
static struct Reg_ regs[1024];
static int n_regs;

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int in_guest_code(u32 a) {
    for (const struct GuestSection *s = guest_sections; s->name; s++)
        if (!strcmp(s->name, ".text")) return a >= s->va && a < s->va + s->vsize;
    return 0;
}

static void dump_state(CPU *c) {
    if (!c) return;
    fprintf(stderr,
            "  eip~%08x eax=%08x ecx=%08x edx=%08x ebx=%08x\n"
            "  esp=%08x ebp=%08x esi=%08x edi=%08x\n  guest return addresses:",
            c->eip, EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI);
    int n = 0;
    for (u32 a = ESP & ~3u; a < c->stack_hi && n < 24; a += 4) {
        u32 v = M32(a);
        if (in_guest_code(v)) {
            fprintf(stderr, " %08x", v);
            n++;
        }
    }
    fputc('\n', stderr);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FATAL: ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    dump_state(g_cpu);
    _exit(3);
}

void guest_trap(CPU *c, u32 addr) { fatal("guest trap (int3/div0) at %08x", addr); }
void guest_unimpl(CPU *c, u32 addr, const char *text) {
    fatal("untranslated instruction at %08x: %s", addr, text);
}
void guest_bad_jump(CPU *c, u32 addr, u32 target) {
    fatal("jump from %08x to untranslated code %08x", addr, target);
}
void guest_cpuid(CPU *c) { EAX = EBX = ECX = EDX = 0; }

// ---- heap: power-of-two size classes, 8-byte header ------------------------
static u32 heap_top = HEAP_LO;
static u32 free_list[32];
static pthread_mutex_t heap_lock = PTHREAD_MUTEX_INITIALIZER;

u32 galloc(u32 size) {
    u32 need = size + 8;
    int k = 4;
    while ((1u << k) < need) k++;
    pthread_mutex_lock(&heap_lock);
    u32 p = free_list[k];
    if (p) {
        free_list[k] = M32(p + 8);
    } else {
        if ((u64)heap_top + (1u << k) > HEAP_HI) fatal("guest heap exhausted");
        p = heap_top;
        heap_top += 1u << k;
    }
    M32(p) = k;
    M32(p + 4) = size;
    pthread_mutex_unlock(&heap_lock);
    return p + 8;
}
u32 gcalloc(u32 size) {
    u32 p = galloc(size);
    memset(HOSTP(p), 0, size);
    return p;
}
void gfree(u32 p) {
    if (!p) return;
    p -= 8;
    u32 k = M32(p);
    if (k < 4 || k > 31) fatal("gfree: bad block %08x", p + 8);
    pthread_mutex_lock(&heap_lock);
    M32(p + 8) = free_list[k];
    free_list[k] = p;
    pthread_mutex_unlock(&heap_lock);
}
u32 gstrdup(const char *s) {
    u32 p = galloc((u32)strlen(s) + 1);
    strcpy(HOSTP(p), s);
    return p;
}

// ---- import thunks --------------------------------------------------------
u32 hle_thunk(HleFn fn, const char *name) {
    if (n_thunks >= MAX_THUNKS) fatal("too many thunks");
    thunk_fn[n_thunks] = fn;
    thunk_name[n_thunks] = name;
    return HLE_BASE + 4 * n_thunks++;
}
void hle_register_import(const char *dll, const char *name, HleFn fn) {
    regs[n_regs++] = (struct Reg_){dll, name, fn, 0, 0};
}
void hle_register_data(const char *dll, const char *name, u32 size, u32 init) {
    regs[n_regs++] = (struct Reg_){dll, name, 0, size, init};
}

void call_indirect(CPU *c, u32 target) {
    if (target >= HLE_BASE) {
        u32 id = (target - HLE_BASE) / 4;
        if (id >= (u32)n_thunks) fatal("call to bad thunk %08x", target);
        if (!thunk_fn[id]) fatal("unimplemented import %s", thunk_name[id]);
        if (g_verbose > 1) logf_("  [%08x] %s", M32(ESP), thunk_name[id]);
        thunk_fn[id](c);
        return;
    }
    if (!guest_dispatch(c, target))
        fatal("indirect call to unknown guest address %08x "
              "(add it to recomp/seeds.txt and rebuild)", target);
}

u32 guest_call(CPU *c, u32 fn, int nargs, ...) {
    u32 args[16];
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, u32);
    va_end(ap);
    u32 sp = ESP;
    for (int i = nargs - 1; i >= 0; i--) PUSH32(args[i]);
    PUSH32(RET_MAGIC);
    call_indirect(c, fn);
    ESP = sp;
    return EAX;
}

static int dll_eq(const char *a, const char *b) {
    size_t n = strlen(b);
    return !strncasecmp(a, b, n) && (a[n] == 0 || a[n] == '.');
}

static void bind_imports(void) {
    for (const struct GuestImport *im = guest_imports; im->slot; im++) {
        struct Reg_ *r = 0;
        for (int i = 0; i < n_regs; i++)
            if (!strcmp(regs[i].name, im->name) && dll_eq(im->dll, regs[i].dll))
                r = &regs[i];
        char *label = malloc(strlen(im->dll) + strlen(im->name) + 2);
        sprintf(label, "%s!%s", im->dll, im->name);
        if (r && !r->fn) {
            u32 p = gcalloc(r->data_size);
            M32(p) = r->data_init;
            if (!strcmp(im->name, "_acmdln")) M32(p) = gstrdup("lf2.exe");
            M32(im->slot) = p;
        } else {
            M32(im->slot) = hle_thunk(r ? r->fn : 0, label);
        }
    }
}

// ---- threads and memory ---------------------------------------------------
static u32 next_stack = 0x0F000000u;
CPU *cpu_new_thread(u32 stack_size) {
    CPU *c = calloc(1, sizeof *c);
    pthread_mutex_lock(&heap_lock);
    next_stack -= stack_size + 0x10000;
    c->stack_lo = next_stack;
    pthread_mutex_unlock(&heap_lock);
    c->stack_hi = c->stack_lo + stack_size;
    ESP = c->stack_hi - 64;
    c->fcw = 0x027f;
    u32 teb = gcalloc(0x1000);
    c->fs_base = teb;
    M32(teb) = 0xffffffff;
    M32(teb + 4) = c->stack_hi;
    M32(teb + 8) = c->stack_lo;
    M32(teb + 0x18) = teb;
    return c;
}

static void on_fault(int sig, siginfo_t *si, void *ctx) {
    u64 a = (u64)si->si_addr;
    fprintf(stderr, "FATAL: signal %d, host address %p", sig, si->si_addr);
    if (a >= GUEST_BASE && a < GUEST_BASE + (1ull << 32))
        fprintf(stderr, " = guest %08x", (u32)(a - GUEST_BASE));
    fputc('\n', stderr);
    dump_state(g_cpu);
    _exit(4);
}

// ---- file locations --------------------------------------------------------
// Inside an .app the game files are read-only bundle resources, so everything
// the game writes (settings, recordings, scratch files) goes to a per-user
// folder that is searched first when reading.
static char g_user_dir[1024];

static void normalise(const char *guest, char *out, size_t n) {
    while (*guest == '\\' || *guest == '/') guest++; // keep everything relative
    if (guest[0] == '.' && (guest[1] == '\\' || guest[1] == '/')) guest += 2;
    size_t i = 0;
    for (; guest[i] && i + 1 < n; i++) out[i] = guest[i] == '\\' ? '/' : guest[i];
    out[i] = 0;
}
void host_path(const char *guest, char *out, size_t n) {
    char rel[1024];
    normalise(guest, rel, sizeof rel);
    if (g_user_dir[0]) {
        snprintf(out, n, "%s/%s", g_user_dir, rel);
        if (!access(out, F_OK)) return;
    }
    snprintf(out, n, "%s", rel);
}
void host_path_write(const char *guest, char *out, size_t n) {
    char rel[1024];
    normalise(guest, rel, sizeof rel);
    if (!g_user_dir[0]) {
        snprintf(out, n, "%s", rel);
        return;
    }
    snprintf(out, n, "%s/%s", g_user_dir, rel);
    for (char *p = out + strlen(g_user_dir) + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(out, 0755);
            *p = '/';
        }
    if (access(out, F_OK) && !access(rel, R_OK)) { // seed it with the bundled copy
        FILE *in = fopen(rel, "rb"), *o = in ? fopen(out, "wb") : 0;
        char buf[65536];
        size_t k;
        while (o && (k = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, k, o);
        if (o) fclose(o);
        if (in) fclose(in);
    }
}

// Returns 1 and fills `dir` when running from inside an application bundle.
static int bundle_game_dir(char *dir, size_t n) {
    char exe[1024], real[1024];
    uint32_t len = sizeof exe;
    if (_NSGetExecutablePath(exe, &len) || !realpath(exe, real)) return 0;
    char *slash = strrchr(real, '/');
    if (!slash) return 0;
    *slash = 0;
    snprintf(dir, n, "%s/../Resources/game", real);
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/lf2.exe", dir);
    return !access(probe, R_OK);
}

int main(int argc, char **argv) {
    const char *dir = 0;
    char bundled[1100];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose++;
        else if (argv[i][0] != '-') dir = argv[i]; // Finder passes -psn_... arguments
    }
    if (!dir && bundle_game_dir(bundled, sizeof bundled)) {
        dir = bundled;
        const char *home = getenv("HOME");
        if (home) {
            snprintf(g_user_dir, sizeof g_user_dir, "%s/Library/Application Support", home);
            mkdir(g_user_dir, 0755);
            strlcat(g_user_dir, "/Little Fighter 2", sizeof g_user_dir);
            mkdir(g_user_dir, 0755);
            if (!isatty(2)) { // started from Finder: keep diagnostics in a log file
                char log[1100];
                snprintf(log, sizeof log, "%s/lf2.log", g_user_dir);
                if (freopen(log, "w", stderr)) setvbuf(stderr, 0, _IOLBF, 0);
            }
        }
    }
    if (!dir) dir = "game";
    if (!realpath(dir, g_game_dir)) fatal("game directory %s: %s", dir, strerror(errno));
    if (chdir(g_game_dir)) fatal("chdir %s", g_game_dir);

    void *m = mmap(MEM, 1ull << 32, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m != (void *)MEM) fatal("could not reserve the guest address space at %p", MEM);
    mprotect(MEM, 0x10000, PROT_NONE);

    FILE *f = fopen("lf2.exe", "rb");
    if (!f) fatal("lf2.exe not found in %s", g_game_dir);
    for (const struct GuestSection *s = guest_sections; s->name; s++) {
        fseek(f, s->raw, SEEK_SET);
        u32 n = s->rawsize < s->vsize ? s->rawsize : s->vsize;
        if (fread(HOSTP(s->va), 1, n, f) != n) fatal("short read of %s", s->name);
    }
    fseek(f, 0, SEEK_SET);
    if (fread(HOSTP(guest_image_base), 1, 0x1000, f) != 0x1000) fatal("short header");
    fclose(f);

    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGTERM, &sa, 0);

    bind_imports();
    CPU *c = g_cpu = cpu_new_thread(4 << 20);
    PUSH32(RET_MAGIC);
    call_indirect(c, guest_entry);
    return (int)EAX;
}
