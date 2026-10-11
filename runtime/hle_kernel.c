// KERNEL32, plus the small libraries we only stub (network, shell, COM).
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "hle.h"
#include "platform.h"

static u32 ticks(void) { return plat_ticks(); }

HLE(KERNEL32, GetStartupInfoA) {
    memset(ARGP(0), 0, 68);
    M32(ARG(0)) = 68;
    RET_STD(1, 0);
}
HLE(KERNEL32, GetModuleHandleA) { RET_STD(1, 0x400000); }
HLE(KERNEL32, GetVersionExA) {
    u32 p = ARG(0);
    M32(p + 4) = 5; // Windows XP
    M32(p + 8) = 1;
    M32(p + 12) = 2600;
    M32(p + 16) = 2; // VER_PLATFORM_WIN32_NT
    M8(p + 20) = 0;
    RET_STD(1, 1);
}
HLE(KERNEL32, GetThreadLocale) { RET_STD(0, 0x409); }
HLE(KERNEL32, GetLocaleInfoA) {
    if (ARG(3)) strcpy(ARGS(2), "1252");
    RET_STD(4, 5);
}
HLE(KERNEL32, GetACP) { RET_STD(0, 1252); }
HLE(KERNEL32, GetSystemTimeAsFileTime) {
    struct timeval tv;
    gettimeofday(&tv, 0);
    M64(ARG(0)) = ((u64)tv.tv_sec + 11644473600ull) * 10000000ull + (u64)tv.tv_usec * 10;
    RET_STD(1, 0);
}
HLE(KERNEL32, GetCurrentProcessId) { RET_STD(0, 1000); }
HLE(KERNEL32, GetCurrentThreadId) { RET_STD(0, 1001); }
HLE(KERNEL32, GetCurrentProcess) { RET_STD(0, 0xffffffff); }
HLE(KERNEL32, GetTickCount) { RET_STD(0, ticks()); }
HLE(KERNEL32, QueryPerformanceCounter) {
    M64(ARG(0)) = plat_micros();
    RET_STD(1, 1);
}
HLE(KERNEL32, IsDebuggerPresent) { RET_STD(0, 0); }
HLE(KERNEL32, SetUnhandledExceptionFilter) { RET_STD(1, 0); }
HLE(KERNEL32, UnhandledExceptionFilter) { fatal("UnhandledExceptionFilter"); }
HLE(KERNEL32, TerminateProcess) { exit((int)ARG(1)); }
HLE(KERNEL32, OutputDebugStringA) {
    LOG("debug: %s", ARGS(0));
    RET_STD(1, 0);
}
HLE(KERNEL32, lstrlenA) { RET_STD(1, ARG(0) ? strlen(ARGS(0)) : 0); }
HLE(KERNEL32, CloseHandle) { RET_STD(1, 1); }
HLE(KERNEL32, GetLastError) { RET_STD(0, 0); }
HLE(KERNEL32, Sleep) {
    // The main loop idles in Sleep(<=5) steps between 33 ms ticks. Waking every
    // millisecond instead keeps ticks on time and input polled promptly.
    // The loading screen calls its own pacing routine after every parsed item
    // and sleeps there; those sleeps only stretch the load, so skip them.
    u32 ret = M32(ESP);
    if (ret == 0x42469du || ret == 0x4246a9u) RET_STD(1, 0);
    plat_delay(ARG(0) > 1 && ARG(0) <= 5 ? 1 : ARG(0));
    RET_STD(1, 0);
}
HLE(KERNEL32, InterlockedExchange) {
    u32 old = M32(ARG(0));
    M32(ARG(0)) = ARG(1);
    RET_STD(2, old);
}
HLE(KERNEL32, InterlockedCompareExchange) {
    u32 old = M32(ARG(0));
    if (old == ARG(2)) M32(ARG(0)) = ARG(1);
    RET_STD(3, old);
}
static pthread_mutex_t big_lock;
__attribute__((constructor)) static void init_lock(void) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&big_lock, &a);
}
HLE(KERNEL32, InitializeCriticalSection) { RET_STD(1, 0); }
HLE(KERNEL32, EnterCriticalSection) {
    pthread_mutex_lock(&big_lock);
    RET_STD(1, 0);
}
HLE(KERNEL32, LeaveCriticalSection) {
    pthread_mutex_unlock(&big_lock);
    RET_STD(1, 0);
}
HLE(KERNEL32, GetLocalTime) {
    time_t t = time(0);
    struct tm *tm = localtime(&t);
    u16 v[8] = {(u16)(tm->tm_year + 1900), (u16)(tm->tm_mon + 1), (u16)tm->tm_wday,
                (u16)tm->tm_mday, (u16)tm->tm_hour, (u16)tm->tm_min, (u16)tm->tm_sec, 0};
    memcpy(ARGP(0), v, sizeof v);
    RET_STD(1, 0);
}
HLE(KERNEL32, MultiByteToWideChar) {
    const u8 *src = ARGP(2);
    int n = (int)ARG(3), cap = (int)ARG(5);
    if (n < 0) n = (int)strlen((const char *)src) + 1;
    if (!cap) RET_STD(6, n);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) M16(ARG(4) + 2 * i) = src[i];
    RET_STD(6, n);
}

// Win32 file handles (CreateFileA/WriteFile): only used for writing logs.
static FILE *hfiles[16];
HLE(KERNEL32, CreateFileA) {
    char path[1024];
    u32 access = ARG(1), disp = ARG(4);
    if (access & 0x40000000) host_path_write(ARGS(0), path, sizeof path);
    else host_path(ARGS(0), path, sizeof path);
    const char *mode = (access & 0x40000000) ? (disp == 2 ? "wb" : disp == 4 ? "ab" : "r+b") : "rb";
    FILE *f = fopen(path, mode);
    LOG("CreateFileA(%s, %s) -> %s", path, mode, f ? "ok" : "failed");
    if (!f) RET_STD(7, 0xffffffff);
    for (int i = 0; i < 16; i++)
        if (!hfiles[i]) {
            hfiles[i] = f;
            RET_STD(7, 0x500 + i);
        }
    fatal("too many CreateFileA handles");
}
HLE(KERNEL32, WriteFile) {
    u32 h = ARG(0) - 0x500;
    u32 n = 0;
    if (h < 16 && hfiles[h]) {
        n = (u32)fwrite(ARGP(1), 1, ARG(2), hfiles[h]);
        fflush(hfiles[h]);
    }
    if (ARG(3)) M32(ARG(3)) = n;
    RET_STD(5, 1);
}

struct ThreadStart {
    CPU *c;
    u32 fn, arg;
};
static void *thread_main(void *p) {
    struct ThreadStart ts = *(struct ThreadStart *)p;
    free(p);
    g_cpu = ts.c;
    guest_call(ts.c, ts.fn, 1, ts.arg);
    return 0;
}
HLE(KERNEL32, CreateThread) {
    struct ThreadStart *ts = malloc(sizeof *ts);
    ts->c = cpu_new_thread(1 << 20);
    ts->fn = ARG(2);
    ts->arg = ARG(3);
    LOG("CreateThread(start=%08x, arg=%08x)", ts->fn, ts->arg);
    pthread_t t;
    pthread_create(&t, 0, thread_main, ts);
    pthread_detach(t);
    if (ARG(5)) M32(ARG(5)) = 2000;
    RET_STD(6, 0x600);
}

// ---- WINMM ----------------------------------------------------------------
HLE(WINMM, timeGetTime) { RET_STD(0, ticks()); }
HLE(WINMM, joyGetNumDevs) { RET_STD(0, 0); }
HLE(WINMM, joyGetPosEx) { RET_STD(2, 167); } // JOYERR_UNPLUGGED
HLE(WINMM, joyGetDevCapsA) { RET_STD(3, 165); }
HLE(WINMM, joySetCapture) { RET_STD(4, 165); }
HLE(WINMM, joySetThreshold) { RET_STD(2, 165); }

// ---- things LF2 can live without for now ------------------------------------
HLE(ole32, CoInitialize) { RET_STD(1, 0); }
HLE(SHELL32, ShellExecuteA) {
    LOG("ShellExecuteA(%s) ignored", ARG(2) ? ARGS(2) : "");
    RET_STD(6, 33);
}
HLE(WININET, InternetOpenA) { RET_STD(5, 0); }
HLE(WININET, InternetOpenUrlA) { RET_STD(6, 0); }
HLE(WININET, InternetReadFile) { RET_STD(4, 0); }
HLE(WININET, InternetCloseHandle) { RET_STD(1, 1); }
HLE(COMDLG32, GetOpenFileNameA) { RET_STD(1, 0); }
HLE_NAMED(WSOCK32, "ord115", WSAStartup) { RET_STD(2, 10091); } // WSASYSNOTREADY
HLE_NAMED(WSOCK32, "ord116", WSACleanup) { RET_STD(0, 0); }

// Network play is not supported yet: every socket call fails cleanly.
#define SOCK_FAIL(ord, ident, nargs, ret)                                      \
    HLE_NAMED(WSOCK32, "ord" #ord, ident) { RET_STD(nargs, ret); }
SOCK_FAIL(1, accept, 3, -1)
SOCK_FAIL(2, bind, 3, -1)
SOCK_FAIL(3, closesocket, 1, 0)
SOCK_FAIL(4, connect, 3, -1)
SOCK_FAIL(10, inet_addr, 1, -1)
SOCK_FAIL(11, inet_ntoa, 1, 0)
SOCK_FAIL(12, ioctlsocket, 3, -1)
SOCK_FAIL(13, listen, 2, -1)
SOCK_FAIL(16, recv, 4, -1)
SOCK_FAIL(19, send, 4, -1)
SOCK_FAIL(20, sendto, 6, -1)
SOCK_FAIL(23, socket, 3, -1)
SOCK_FAIL(51, gethostbyaddr, 3, 0)
SOCK_FAIL(52, gethostbyname, 1, 0)
SOCK_FAIL(57, gethostname, 2, -1)
SOCK_FAIL(101, WSAAsyncSelect, 4, -1)
HLE_NAMED(WSOCK32, "ord9", htons) {
    u32 v = ARG(0);
    RET_STD(1, ((v & 0xff) << 8) | ((v >> 8) & 0xff));
}
