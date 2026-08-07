/* gbpython soft-float: IEEE-754 single precision on raw bit patterns,
   using only 32-bit integer math (GBDK-2020 ships no float library for
   the sm83). Truncating rounding; denormals flush to zero; no NaN/inf
   handling — the interpreter raises ZeroDivisionError before dividing by
   zero and never produces NaN. Lives in bank 0 so the lexer (bank 0),
   evaluator (bank 1) and builtins (bank 3) can all call it. */

#pragma bank 3

#include <gb/gb.h>
#include "gbpython.h"

#define F32_SIGN 0x80000000UL
#define F32_MABITS 0x007FFFFFUL
#define F32_IMPLICIT 0x00800000UL

typedef unsigned long u32;

uint8_t f32_is_zero(long a) BANKED {
    return ((u32)a & ~F32_SIGN) == 0;
}

long f32_neg(long a) BANKED {
    if (f32_is_zero(a)) return 0;
    return (long)((u32)a ^ F32_SIGN);
}

static u32 pack(uint8_t s, int e, u32 m) {
    /* m is a normalized 24-bit mantissa (implicit bit set), or 0 */
    if (m == 0 || e <= 0) return 0;               /* flush underflow */
    if (e >= 255) e = 254;                        /* clamp overflow  */
    return ((u32)s << 31) | ((u32)e << 23) | (m & F32_MABITS);
}

long f32_from_int(long v) BANKED {
    uint8_t s = 0;
    int e = 150; /* 127 + 23 */
    u32 m;
    if (v == 0) return 0;
    if (v < 0) {
        s = 1;
        v = -v;
    }
    m = (u32)v;
    while (m >= (F32_IMPLICIT << 1)) {
        m >>= 1;
        e++;
    }
    while (m < F32_IMPLICIT) {
        m <<= 1;
        e--;
    }
    return (long)pack(s, e, m);
}

/* Truncate toward zero to a 32-bit integer */
long f32_trunc(long a) BANKED {
    uint8_t s = ((u32)a >> 31) & 1;
    int e = (int)(((u32)a >> 23) & 0xFF) - 127;
    u32 m = ((u32)a & F32_MABITS) | F32_IMPLICIT;
    long v;
    if (((u32)a & ~F32_SIGN) == 0 || e < 0) return 0;
    if (e >= 31) return s ? -2147483647L - 1 : 2147483647L;
    if (e >= 23) v = (long)(m << (e - 23));
    else v = (long)(m >> (23 - e));
    return s ? -v : v;
}

/* -1, 0, 1 */
int8_t f32_cmp(long a, long b) BANKED {
    u32 ua = (u32)a;
    u32 ub = (u32)b;
    uint8_t sa, sb;
    if (f32_is_zero(a) && f32_is_zero(b)) return 0;
    sa = (uint8_t)(ua >> 31);
    sb = (uint8_t)(ub >> 31);
    if (sa != sb) return sa ? -1 : 1;
    if (ua == ub) return 0;
    if (sa) return ua > ub ? -1 : 1; /* both negative: bigger bits = smaller */
    return ua > ub ? 1 : -1;
}

long f32_add(long a, long b) BANKED {
    u32 ua = (u32)a;
    u32 ub = (u32)b;
    uint8_t sa, sb;
    int ea, eb;
    u32 ma, mb, m;
    if (f32_is_zero(a)) return b;
    if (f32_is_zero(b)) return a;
    sa = (uint8_t)(ua >> 31);
    sb = (uint8_t)(ub >> 31);
    ea = (int)((ua >> 23) & 0xFF);
    eb = (int)((ub >> 23) & 0xFF);
    ma = (ua & F32_MABITS) | F32_IMPLICIT;
    mb = (ub & F32_MABITS) | F32_IMPLICIT;
    /* align on the larger exponent (3 guard bits) */
    ma <<= 3;
    mb <<= 3;
    if (ea > eb) {
        int d = ea - eb;
        mb = d > 26 ? 0 : mb >> d;
    } else if (eb > ea) {
        int d = eb - ea;
        ma = d > 26 ? 0 : ma >> d;
        ea = eb;
    }
    if (sa == sb) {
        m = ma + mb;
    } else if (ma >= mb) {
        m = ma - mb;
    } else {
        m = mb - ma;
        sa = sb;
    }
    if (m == 0) return 0;
    /* normalize back from the 3-bit guard scale */
    while (m >= (F32_IMPLICIT << 4)) {
        m >>= 1;
        ea++;
    }
    while (m < (F32_IMPLICIT << 3)) {
        m <<= 1;
        ea--;
    }
    return (long)pack(sa, ea, m >> 3);
}

long f32_sub(long a, long b) BANKED {
    return f32_add(a, f32_neg(b));
}

long f32_mul(long a, long b) BANKED {
    u32 ua = (u32)a;
    u32 ub = (u32)b;
    uint8_t s;
    int e;
    u32 ma, mb, ah, al, bh, bl, r;
    if (f32_is_zero(a) || f32_is_zero(b)) return 0;
    s = (uint8_t)((ua >> 31) ^ (ub >> 31));
    e = (int)((ua >> 23) & 0xFF) + (int)((ub >> 23) & 0xFF) - 127;
    ma = (ua & F32_MABITS) | F32_IMPLICIT;
    mb = (ub & F32_MABITS) | F32_IMPLICIT;
    /* 24x24 -> keep the top bits of the 48-bit product via 12-bit halves */
    ah = ma >> 12;
    al = ma & 0xFFF;
    bh = mb >> 12;
    bl = mb & 0xFFF;
    r = (ah * bh << 1) + ((ah * bl + al * bh) >> 11) + ((al * bl) >> 23);
    /* r ~= (ma*mb) >> 23, in [2^23, 2^25) */
    if (r >= (F32_IMPLICIT << 1)) {
        r >>= 1;
        e++;
    }
    return (long)pack(s, e, r);
}

long f32_div(long a, long b) BANKED {
    u32 ua = (u32)a;
    u32 ub = (u32)b;
    uint8_t s, i;
    int e;
    u32 num, den, q;
    if (f32_is_zero(a)) return 0;
    if (f32_is_zero(b)) return 0; /* caller raises ZeroDivisionError first */
    s = (uint8_t)((ua >> 31) ^ (ub >> 31));
    e = (int)((ua >> 23) & 0xFF) - (int)((ub >> 23) & 0xFF) + 127;
    num = (ua & F32_MABITS) | F32_IMPLICIT;
    den = (ub & F32_MABITS) | F32_IMPLICIT;
    if (num < den) {
        num <<= 1;
        e--;
    }
    q = 0;
    for (i = 0; i < 24; i++) {
        q <<= 1;
        if (num >= den) {
            num -= den;
            q |= 1;
        }
        num <<= 1;
    }
    /* q in [2^23, 2^24) */
    return (long)pack(s, e, q);
}

/* Floor to a 32-bit integer (python // and %) */
long f32_floor(long a) BANKED {
    long t = f32_trunc(a);
    if (f32_cmp(f32_from_int(t), a) > 0) t--;
    return t;
}
