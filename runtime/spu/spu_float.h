/* SPU ISA v1.2, section 9.1: extended-range single precision.
 * Included by spu_helpers.h after spu_clz32. Exponent 255 is finite, exponent
 * zero is zero, results truncate toward zero, and overflow saturates.
 * Keep arithmetic in integers: host IEEE floats interpret extended operands
 * as NaNs/infinities, and even double intermediates lose tiny subtractands
 * that change a truncated result by one ulp. No host rounding mode is changed.
 */
#ifndef SPU_FLOAT_H
#define SPU_FLOAT_H

static inline uint32_t spu_float_mantissa(uint32_t bits) {
    return (bits & 0x7f800000u) ? (0x800000u | (bits & 0x7fffffu)) : 0;
}
static inline int spu_float_exponent(uint32_t bits) {
    return (int)((bits >> 23) & 255u) - 150;
}
static inline int spu_float_top(uint64_t value) {
    uint32_t high = (uint32_t)(value >> 32);
    return high ? 63 - spu_clz32(high) : 31 - spu_clz32((uint32_t)value);
}
/* value = magnitude * 2^exponent. Called only with an exact integer or a
 * normalized sum retaining more than 24 significant bits and a sticky bit. */
static inline uint32_t spu_float_pack(uint64_t magnitude, int exponent, uint32_t sign) {
    if (!magnitude) return 0;
    int top = spu_float_top(magnitude);
    int biased = exponent + top + 127;
    if (biased <= 0) return 0;
    if (biased > 255) return sign | 0x7fffffffu;
    uint32_t mantissa = (uint32_t)(top > 23 ? magnitude >> (top - 23)
                                                        : magnitude << (23 - top));
    return sign | ((uint32_t)biased << 23) | (mantissa & 0x7fffffu);
}
static inline uint64_t spu_float_shift_jam(uint64_t value, unsigned shift) {
    if (!shift) return value;
    if (shift >= 64) return value != 0;
    return (value >> shift) | ((value << (64 - shift)) != 0);
}
/* Each input has at most 48 significant bits (an exact 24x24 product).
 * Normalize at bit 62 to leave room for a carry. Alignment retains a sticky
 * bit far below the output precision. Cancellation capable of exposing that
 * bit requires close exponents, for which alignment is exact instead. */
static inline uint32_t spu_float_sum(uint64_t a, int ea, uint32_t sa,
                                     uint64_t b, int eb, uint32_t sb) {
    if (!a) return spu_float_pack(b, eb, sb);
    if (!b) return spu_float_pack(a, ea, sa);
    int shift = 62 - spu_float_top(a); a <<= shift; ea -= shift;
    shift = 62 - spu_float_top(b); b <<= shift; eb -= shift;
    if (ea > eb) b = spu_float_shift_jam(b, (unsigned)(ea - eb));
    else if (eb > ea) { a = spu_float_shift_jam(a, (unsigned)(eb - ea)); ea = eb; }
    if (sa == sb) return spu_float_pack(a + b, ea, sa);
    return a >= b ? spu_float_pack(a - b, ea, sa) : spu_float_pack(b - a, ea, sb);
}
static inline uint32_t spu_float_add(uint32_t a, uint32_t b) {
    return spu_float_sum(spu_float_mantissa(a), spu_float_exponent(a), a & 0x80000000u,
                         spu_float_mantissa(b), spu_float_exponent(b), b & 0x80000000u);
}
static inline uint32_t spu_float_mul(uint32_t a, uint32_t b) {
    return spu_float_pack((uint64_t)spu_float_mantissa(a) * spu_float_mantissa(b),
                          spu_float_exponent(a) + spu_float_exponent(b),
                          (a ^ b) & 0x80000000u);
}
static inline uint32_t spu_float_madd(uint32_t a, uint32_t b, uint32_t c) {
    return spu_float_sum((uint64_t)spu_float_mantissa(a) * spu_float_mantissa(b),
                         spu_float_exponent(a) + spu_float_exponent(b), (a ^ b) & 0x80000000u,
                         spu_float_mantissa(c), spu_float_exponent(c), c & 0x80000000u);
}
static inline uint32_t spu_float_canonical(uint32_t bits) {
    return (bits & 0x7f800000u) ? bits : 0;
}
static inline int spu_float_gt(uint32_t a, uint32_t b) {
    a = spu_float_canonical(a); b = spu_float_canonical(b);
    if ((a ^ b) & 0x80000000u) return (b >> 31) != 0;
    return (a >> 31) ? a < b : a > b;
}
static inline uint32_t spu_float_to_int(uint32_t bits, int scale, int is_signed) {
    uint32_t sign = bits & 0x80000000u;
    uint32_t mantissa = spu_float_mantissa(bits);
    if (!mantissa || (sign && !is_signed)) return 0;
    uint32_t limit = is_signed ? (sign ? 0x80000000u : 0x7fffffffu) : 0xffffffffu;
    int exponent = spu_float_exponent(bits) + scale;
    uint32_t magnitude;
    if (exponent >= 0) {
        magnitude = exponent >= 32 || mantissa > (limit >> exponent)
                    ? limit : mantissa << exponent;
    } else magnitude = exponent <= -24 ? 0 : mantissa >> -exponent;
    return sign ? 0u - magnitude : magnitude;
}
#endif
