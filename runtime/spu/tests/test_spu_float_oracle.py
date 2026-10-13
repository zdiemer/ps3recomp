"""Generate independent exact-integer SPU single-precision test vectors.

Usage: python test_spu_float_oracle.py output.bin [--cases 100000]
Run test_spu_float output.bin afterwards. All inputs are exact multiples of
2**-298, so Python's unbounded integers preserve every bit of a fused result.
This intentionally uses no floating-point arithmetic or runtime helper code.
"""
import argparse
import random
import struct
from pathlib import Path

UNIT = 298


def decode(bits):
    exp = (bits >> 23) & 255
    if not exp:
        return 0
    value = ((bits & 0x7fffff) | 0x800000) << (exp - 150 + UNIT)
    return -value if bits >> 31 else value


def pack(value, unit=UNIT):
    sign = 0x80000000 if value < 0 else 0
    value = abs(value)
    if not value:
        return 0
    top = value.bit_length() - 1
    biased = top - unit + 127
    if biased <= 0:
        return 0
    if biased > 255:
        return sign | 0x7fffffff
    mantissa = value >> (top - 23) if top >= 23 else value << (23 - top)
    return sign | (biased << 23) | (mantissa & 0x7fffff)


def expected(op, a, b, c, imm):
    x, y, z = map(decode, (a, b, c))
    if op == 0:
        return pack(x + y)
    if op == 1:
        return pack(x - y)
    if op == 2:
        return pack(x * y, UNIT * 2)
    if op in (3, 4, 5):
        value = x*y + (z << UNIT) if op == 3 else x*y - (z << UNIT) if op == 4 else (z << UNIT) - x*y
        return pack(value, UNIT * 2)
    if op in (6, 7, 8, 9):
        if op in (8, 9):
            x, y = abs(x), abs(y)
        return 0xffffffff if (x == y if op in (7, 9) else x > y) else 0
    if op in (10, 11):
        shift = UNIT - (173 - imm)
        magnitude = abs(x) >> shift if shift >= 0 else abs(x) << -shift
        value = -magnitude if x < 0 else magnitude
        lo, hi = (-2**31, 2**31-1) if op == 10 else (0, 2**32-1)
        return max(lo, min(hi, value)) & 0xffffffff
    if op in (12, 13):
        value = a - 2**32 if op == 12 and a >> 31 else a
        return pack(value, 155 - imm)
    # FI base and step are separately encoded fields, not two IEEE operands.
    base = decode(b & 0xfffffc00)
    exp = (b >> 23) & 255
    step_y = ((b & 0x3ff) * (a & 0x7ffff)) << (exp - 159 + UNIT)
    return pack(base + step_y if b >> 31 else base - step_y)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--cases', type=int, default=100000)
    args = parser.parse_args()
    rng = random.Random(0x535055)
    edges = [0, 0x80000000, 1, 0x007fffff, 0x00800000, 0x00800001,
             0x3f7fffff, 0x3f800000, 0x3f800001, 0x7effffff, 0x7f000000,
             0x7f7fffff, 0x7f800000, 0x7ffffffe, 0x7fffffff]
    edges += [x ^ 0x80000000 for x in edges]
    count = 0
    with args.output.open('wb') as f:
        for n in range(args.cases):
            a, b, c = [rng.choice(edges) if n % 2 else rng.getrandbits(32) for _ in range(3)]
            # Force close-exponent cancellation, including exact FMA low bits.
            if n % 4 == 0:
                b = (a & 0xff800000) | rng.getrandbits(23)
                c = a ^ 0x80000000
            for op in range(15):
                imm = rng.choice([0, 1, 127, 154, 155, 172, 173, 254, 255])
                f.write(struct.pack('<6I', op, a, b, c, imm, expected(op,a,b,c,imm)))
                count += 1
    print(f'{count:,} exact SPU oracle vectors written to {args.output}')


if __name__ == '__main__':
    main()
