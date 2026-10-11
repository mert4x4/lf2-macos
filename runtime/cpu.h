// Guest x86-32 CPU state and instruction helpers used by the recompiled code.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

typedef union {
    u32 d;
    u16 w;
    struct {
        u8 l, h;
    } b;
} Reg;

typedef struct CPU {
    Reg r[8]; // eax ecx edx ebx esp ebp esi edi
    u8 cf, pf, zf, sf, of, df;
    u16 fsw, fcw;
    u32 ftop;
    double st[8];
    u32 fs_base;
    u32 eip; // last block entered, for diagnostics only
    u32 stack_lo, stack_hi;
} CPU;

// The guest's 4 GiB address space is reserved at a fixed host address so a
// guest pointer is a constant offset away from its host pointer.
#define GUEST_BASE 0x10000000000ULL
#define MEM ((u8 *)GUEST_BASE)
#define HOSTP(a) ((void *)(MEM + (u32)(a)))
#define GUESTP(p) ((u32)((u8 *)(p) - MEM))

typedef u16 __attribute__((aligned(1), may_alias)) u16u;
typedef u32 __attribute__((aligned(1), may_alias)) u32u;
typedef u64 __attribute__((aligned(1), may_alias)) u64u;
typedef float __attribute__((aligned(1), may_alias)) f32u;
typedef double __attribute__((aligned(1), may_alias)) f64u;

#define M8(a) (*(u8 *)(MEM + (u32)(a)))
#define M16(a) (*(u16u *)(MEM + (u32)(a)))
#define M32(a) (*(u32u *)(MEM + (u32)(a)))
#define M64(a) (*(u64u *)(MEM + (u32)(a)))
#define MF32(a) (*(f32u *)(MEM + (u32)(a)))
#define MF64(a) (*(f64u *)(MEM + (u32)(a)))

#define EAX (c->r[0].d)
#define ECX (c->r[1].d)
#define EDX (c->r[2].d)
#define EBX (c->r[3].d)
#define ESP (c->r[4].d)
#define EBP (c->r[5].d)
#define ESI (c->r[6].d)
#define EDI (c->r[7].d)
#define AX (c->r[0].w)
#define CX (c->r[1].w)
#define DX (c->r[2].w)
#define BX (c->r[3].w)
#define SP (c->r[4].w)
#define BP (c->r[5].w)
#define SI (c->r[6].w)
#define DI (c->r[7].w)
#define AL (c->r[0].b.l)
#define CL (c->r[1].b.l)
#define DL (c->r[2].b.l)
#define BL (c->r[3].b.l)
#define AH (c->r[0].b.h)
#define CH (c->r[1].b.h)
#define DH (c->r[2].b.h)
#define BH (c->r[3].b.h)

#define T(a) (c->eip = (a))

#define PUSH32(v)                                                              \
    do {                                                                       \
        u32 v_ = (v);                                                          \
        ESP -= 4;                                                              \
        M32(ESP) = v_;                                                         \
    } while (0)

void call_indirect(CPU *c, u32 target);
void guest_trap(CPU *c, u32 addr) __attribute__((noreturn));
void guest_unimpl(CPU *c, u32 addr, const char *text) __attribute__((noreturn));
void guest_bad_jump(CPU *c, u32 addr, u32 target) __attribute__((noreturn));
void guest_cpuid(CPU *c);

#define PAR(x) ((u8)!__builtin_parity((u32)(x) & 0xff))

#define COND_O (c->of)
#define COND_NO (!c->of)
#define COND_B (c->cf)
#define COND_AE (!c->cf)
#define COND_E (c->zf)
#define COND_NE (!c->zf)
#define COND_BE (c->cf || c->zf)
#define COND_A (!c->cf && !c->zf)
#define COND_S (c->sf)
#define COND_NS (!c->sf)
#define COND_P (c->pf)
#define COND_NP (!c->pf)
#define COND_L (c->sf != c->of)
#define COND_GE (c->sf == c->of)
#define COND_LE (c->zf || c->sf != c->of)
#define COND_G (!c->zf && c->sf == c->of)

#define DEF_ARITH(N, UT, ST)                                                   \
    static inline void szp##N(CPU *c, UT r) {                                  \
        c->zf = !r;                                                            \
        c->sf = (ST)r < 0;                                                     \
        c->pf = PAR(r);                                                        \
    }                                                                          \
    static inline UT add##N(CPU *c, UT a, UT b) {                              \
        UT r = (UT)(a + b);                                                    \
        c->cf = r < a;                                                         \
        c->of = (((a ^ r) & (b ^ r)) >> (N - 1)) & 1;                          \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT adc##N(CPU *c, UT a, UT b) {                              \
        u64 w = (u64)a + b + c->cf;                                            \
        UT r = (UT)w;                                                          \
        c->cf = (w >> N) & 1;                                                  \
        c->of = (((a ^ r) & (b ^ r)) >> (N - 1)) & 1;                          \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT sub##N(CPU *c, UT a, UT b) {                              \
        UT r = (UT)(a - b);                                                    \
        c->cf = a < b;                                                         \
        c->of = (((a ^ b) & (a ^ r)) >> (N - 1)) & 1;                          \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT sbb##N(CPU *c, UT a, UT b) {                              \
        u64 bb = (u64)b + c->cf;                                               \
        UT r = (UT)(a - bb);                                                   \
        c->cf = (u64)a < bb;                                                   \
        c->of = (((a ^ b) & (a ^ r)) >> (N - 1)) & 1;                          \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT and##N(CPU *c, UT a, UT b) {                              \
        UT r = a & b;                                                          \
        c->cf = c->of = 0;                                                     \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT or##N(CPU *c, UT a, UT b) {                               \
        UT r = a | b;                                                          \
        c->cf = c->of = 0;                                                     \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT xor##N(CPU *c, UT a, UT b) {                              \
        UT r = a ^ b;                                                          \
        c->cf = c->of = 0;                                                     \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT inc##N(CPU *c, UT a) {                                    \
        UT r = (UT)(a + 1);                                                    \
        c->of = r == (UT)((UT)1 << (N - 1));                                   \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT dec##N(CPU *c, UT a) {                                    \
        UT r = (UT)(a - 1);                                                    \
        c->of = a == (UT)((UT)1 << (N - 1));                                   \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT neg##N(CPU *c, UT a) {                                    \
        UT r = (UT)(0 - a);                                                    \
        c->cf = a != 0;                                                        \
        c->of = a == (UT)((UT)1 << (N - 1));                                   \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT shl##N(CPU *c, UT a, u32 n) {                             \
        n &= 31;                                                               \
        if (!n) return a;                                                      \
        UT r = n < N ? (UT)((u32)a << n) : 0;                                  \
        c->cf = n <= N ? ((u32)a >> (N - n)) & 1 : 0;                          \
        c->of = ((r >> (N - 1)) & 1) ^ c->cf;                                  \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT shr##N(CPU *c, UT a, u32 n) {                             \
        n &= 31;                                                               \
        if (!n) return a;                                                      \
        UT r = n < N ? (UT)((u32)a >> n) : 0;                                  \
        c->cf = n <= N ? ((u32)a >> (n - 1)) & 1 : 0;                          \
        c->of = (a >> (N - 1)) & 1;                                            \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT sar##N(CPU *c, UT a, u32 n) {                             \
        n &= 31;                                                               \
        if (!n) return a;                                                      \
        s32 sa = (ST)a;                                                        \
        UT r = (UT)(sa >> (n < N ? n : N - 1));                                \
        c->cf = (sa >> (n <= N ? n - 1 : N - 1)) & 1;                          \
        c->of = 0;                                                             \
        szp##N(c, r);                                                          \
        return r;                                                              \
    }                                                                          \
    static inline UT rol##N(CPU *c, UT a, u32 n) {                             \
        n &= 31;                                                               \
        if (!n) return a;                                                      \
        u32 m = n % N;                                                         \
        UT r = m ? (UT)(((u32)a << m) | ((u32)a >> (N - m))) : a;              \
        c->cf = r & 1;                                                         \
        c->of = ((r >> (N - 1)) & 1) ^ c->cf;                                  \
        return r;                                                              \
    }                                                                          \
    static inline UT ror##N(CPU *c, UT a, u32 n) {                             \
        n &= 31;                                                               \
        if (!n) return a;                                                      \
        u32 m = n % N;                                                         \
        UT r = m ? (UT)(((u32)a >> m) | ((u32)a << (N - m))) : a;              \
        c->cf = (r >> (N - 1)) & 1;                                            \
        c->of = ((r >> (N - 1)) ^ (r >> (N - 2))) & 1;                         \
        return r;                                                              \
    }

DEF_ARITH(8, u8, s8)
DEF_ARITH(16, u16, s16)
DEF_ARITH(32, u32, s32)

static inline u32 imul32(CPU *c, u32 a, u32 b) {
    s64 w = (s64)(s32)a * (s64)(s32)b;
    u32 r = (u32)w;
    c->cf = c->of = w != (s64)(s32)r;
    return r;
}
static inline u16 imul16(CPU *c, u16 a, u16 b) {
    s32 w = (s32)(s16)a * (s32)(s16)b;
    u16 r = (u16)w;
    c->cf = c->of = w != (s32)(s16)r;
    return r;
}
static inline void imul1_32(CPU *c, u32 b) {
    s64 w = (s64)(s32)EAX * (s64)(s32)b;
    EAX = (u32)w;
    EDX = (u32)((u64)w >> 32);
    c->cf = c->of = w != (s64)(s32)EAX;
}
static inline void imul1_16(CPU *c, u16 b) {
    s32 w = (s32)(s16)AX * (s32)(s16)b;
    AX = (u16)w;
    DX = (u16)((u32)w >> 16);
    c->cf = c->of = w != (s32)(s16)AX;
}
static inline void imul1_8(CPU *c, u8 b) {
    s32 w = (s32)(s8)AL * (s32)(s8)b;
    AX = (u16)w;
    c->cf = c->of = w != (s32)(s8)AL;
}
static inline void mul32(CPU *c, u32 b) {
    u64 w = (u64)EAX * b;
    EAX = (u32)w;
    EDX = (u32)(w >> 32);
    c->cf = c->of = EDX != 0;
}
static inline void mul16(CPU *c, u16 b) {
    u32 w = (u32)AX * b;
    AX = (u16)w;
    DX = (u16)(w >> 16);
    c->cf = c->of = DX != 0;
}
static inline void mul8(CPU *c, u8 b) {
    AX = (u16)((u32)AL * b);
    c->cf = c->of = AH != 0;
}
static inline void div32(CPU *c, u32 b) {
    if (!b) guest_trap(c, c->eip);
    u64 n = ((u64)EDX << 32) | EAX;
    EAX = (u32)(n / b);
    EDX = (u32)(n % b);
}
static inline void div16(CPU *c, u16 b) {
    if (!b) guest_trap(c, c->eip);
    u32 n = ((u32)DX << 16) | AX;
    AX = (u16)(n / b);
    DX = (u16)(n % b);
}
static inline void div8(CPU *c, u8 b) {
    if (!b) guest_trap(c, c->eip);
    u16 n = AX;
    AL = (u8)(n / b);
    AH = (u8)(n % b);
}
static inline void idiv32(CPU *c, u32 b) {
    if (!b) guest_trap(c, c->eip);
    s64 n = (s64)(((u64)EDX << 32) | EAX);
    s64 d = (s32)b;
    EAX = (u32)(n / d);
    EDX = (u32)(n % d);
}
static inline void idiv16(CPU *c, u16 b) {
    if (!b) guest_trap(c, c->eip);
    s32 n = (s32)(((u32)DX << 16) | AX);
    s32 d = (s16)b;
    AX = (u16)(n / d);
    DX = (u16)(n % d);
}
static inline void idiv8(CPU *c, u8 b) {
    if (!b) guest_trap(c, c->eip);
    s32 n = (s16)AX;
    s32 d = (s8)b;
    AL = (u8)(n / d);
    AH = (u8)(n % d);
}
static inline u32 shld32(CPU *c, u32 a, u32 b, u32 n) {
    n &= 31;
    if (!n) return a;
    u32 r = (a << n) | (b >> (32 - n));
    c->cf = (a >> (32 - n)) & 1;
    c->of = ((a ^ r) >> 31) & 1;
    szp32(c, r);
    return r;
}
static inline u32 shrd32(CPU *c, u32 a, u32 b, u32 n) {
    n &= 31;
    if (!n) return a;
    u32 r = (a >> n) | (b << (32 - n));
    c->cf = (a >> (n - 1)) & 1;
    c->of = ((a ^ r) >> 31) & 1;
    szp32(c, r);
    return r;
}

static inline u32 pack_eflags(CPU *c) {
    return 0x202 | c->cf | (c->pf << 2) | (c->zf << 6) | (c->sf << 7) |
           (c->df << 10) | (c->of << 11);
}
static inline void unpack_eflags(CPU *c, u32 f) {
    c->cf = f & 1;
    c->pf = (f >> 2) & 1;
    c->zf = (f >> 6) & 1;
    c->sf = (f >> 7) & 1;
    c->df = (f >> 10) & 1;
    c->of = (f >> 11) & 1;
}

// ---- string instructions -------------------------------------------------
#define DEF_STR(N, UT, MX, ACC)                                                \
    static inline void movs##N(CPU *c) {                                       \
        MX(EDI) = MX(ESI);                                                     \
        s32 d = c->df ? -(N / 8) : (N / 8);                                    \
        ESI += d;                                                              \
        EDI += d;                                                              \
    }                                                                          \
    static inline void rep_movs##N(CPU *c) {                                   \
        while (ECX) {                                                          \
            movs##N(c);                                                        \
            ECX--;                                                             \
        }                                                                      \
    }                                                                          \
    static inline void stos##N(CPU *c) {                                       \
        MX(EDI) = ACC;                                                         \
        EDI += c->df ? -(N / 8) : (N / 8);                                     \
    }                                                                          \
    static inline void rep_stos##N(CPU *c) {                                   \
        while (ECX) {                                                          \
            stos##N(c);                                                        \
            ECX--;                                                             \
        }                                                                      \
    }                                                                          \
    static inline void lods##N(CPU *c) {                                       \
        ACC = MX(ESI);                                                         \
        ESI += c->df ? -(N / 8) : (N / 8);                                     \
    }                                                                          \
    static inline void cmps##N(CPU *c) {                                       \
        UT a = MX(ESI), b = MX(EDI);                                           \
        s32 d = c->df ? -(N / 8) : (N / 8);                                    \
        ESI += d;                                                              \
        EDI += d;                                                              \
        (void)sub##N(c, a, b);                                                 \
    }                                                                          \
    static inline void scas##N(CPU *c) {                                       \
        UT b = MX(EDI);                                                        \
        EDI += c->df ? -(N / 8) : (N / 8);                                     \
        (void)sub##N(c, ACC, b);                                               \
    }                                                                          \
    static inline void rep_cmps##N(CPU *c, int want_zf) {                      \
        if (!ECX) return;                                                      \
        s32 d = c->df ? -(N / 8) : (N / 8);                                    \
        UT a, b;                                                               \
        do {                                                                   \
            a = MX(ESI);                                                       \
            b = MX(EDI);                                                       \
            ESI += d;                                                          \
            EDI += d;                                                          \
            ECX--;                                                             \
        } while (ECX && (a == b) == want_zf);                                  \
        (void)sub##N(c, a, b);                                                 \
    }                                                                          \
    static inline void rep_scas##N(CPU *c, int want_zf) {                      \
        if (!ECX) return;                                                      \
        s32 d = c->df ? -(N / 8) : (N / 8);                                    \
        UT a = ACC, b;                                                         \
        do {                                                                   \
            b = MX(EDI);                                                       \
            EDI += d;                                                          \
            ECX--;                                                             \
        } while (ECX && (a == b) == want_zf);                                  \
        (void)sub##N(c, a, b);                                                 \
    }

DEF_STR(8, u8, M8, AL)
DEF_STR(16, u16, M16, AX)
DEF_STR(32, u32, M32, EAX)

// ---- x87 -----------------------------------------------------------------
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(CPU *c, double v) {
    c->ftop = (c->ftop - 1) & 7;
    c->st[c->ftop] = v;
}
static inline void fpop(CPU *c) { c->ftop = (c->ftop + 1) & 7; }
static inline void fcom(CPU *c, double a, double b) {
    u16 f = c->fsw & ~0x4500;
    if (a != a || b != b)
        f |= 0x4500;
    else if (a < b)
        f |= 0x0100;
    else if (a == b)
        f |= 0x4000;
    c->fsw = f;
}
static inline void fcomi(CPU *c, double a, double b) {
    c->of = c->sf = 0;
    if (a != a || b != b) {
        c->zf = c->pf = c->cf = 1;
    } else {
        c->zf = a == b;
        c->cf = a < b;
        c->pf = 0;
    }
}
static inline u16 fstsw(CPU *c) {
    return (u16)((c->fsw & ~0x3800) | (c->ftop << 11));
}
static inline double fround(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
static inline u32 f2i32(CPU *c, double v) {
    v = fround(c, v);
    if (!(v >= -2147483648.0 && v <= 2147483647.0)) return 0x80000000u;
    return (u32)(s32)v;
}
static inline u16 f2i16(CPU *c, double v) {
    v = fround(c, v);
    if (!(v >= -32768.0 && v <= 32767.0)) return 0x8000u;
    return (u16)(s16)v;
}
static inline u64 f2i64(CPU *c, double v) {
    v = fround(c, v);
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0))
        return 0x8000000000000000ull;
    return (u64)(s64)v;
}
static inline void sahf(CPU *c) {
    u8 f = AH;
    c->cf = f & 1;
    c->pf = (f >> 2) & 1;
    c->zf = (f >> 6) & 1;
    c->sf = (f >> 7) & 1;
}
