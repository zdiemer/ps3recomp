"""Run the real decrementer channel body with a controlled clock and wide oracle."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--source", type=Path, default=ROOT / "runtime/spu/spu_channels.c")
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    rate = re.search(r"^#define SPU_DECREMENTER_HZ\s+(\d+)ull", source, re.M)
    assert rate, "missing decrementer rate"
    hz = int(rate[1])
    begin = source.index("    case SPU_RdDec: {")
    end = source.index("    case SPU_RdEventMask:", begin)
    body = source[begin:end]
    # Python integers do not overflow: include both the old uint64 product
    # overflow and legitimate 32-bit decrementer wraps, with subsecond cases.
    boundary = (1 << 64) // hz
    elapsed = sorted({0, 1, 12, 13, 999_999_999, 1_000_000_000,
                      boundary - 1, boundary, boundary + 1,
                      232_000_000_000, 462_000_000_000, 693_123_456_789,
                      3600_000_000_000, 86400_000_000_001, (1 << 63) - 1})
    elapsed += [(1 << 32) * 1_000_000_000 // hz + n for n in (-1, 0, 1)]
    cases = []
    for ns in elapsed:
        for counter in (0, 0x7fffffff, 0xffffffff):
            expected = (counter - ns * hz // 1_000_000_000) & 0xffffffff
            cases.append(f"{{{ns}ull, {counter}u, {expected}u}}")
    harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#define SPU_RdDec 8
#define SPU_DECREMENTER_HZ @HZ@ull
typedef struct { uint64_t dec_base_ns; uint32_t decrementer; } context;
static uint64_t now_ns;
static uint64_t spu_host_ns(void) { return now_ns; }
static uint32_t read_dec(context* ctx) {
    uint32_t v = 0;
    switch (SPU_RdDec) {
@BODY@
    }
    return v;
}
int main(void) {
    static const struct { uint64_t elapsed; uint32_t counter, expected; } cases[] = {
@CASES@
    };
    context fresh = {0, 123};
    now_ns = 987654321000ull;
    assert(read_dec(&fresh) == 123 && fresh.dec_base_ns == now_ns);
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        context ctx = {1000, cases[i].counter};
        now_ns = ctx.dec_base_ns + cases[i].elapsed;
        uint32_t actual = read_dec(&ctx);
        if (actual != cases[i].expected) {
            fprintf(stderr, "elapsed=%llu: got %u, expected %u\n",
                    (unsigned long long)cases[i].elapsed, actual, cases[i].expected);
            return 1;
        }
        assert(ctx.dec_base_ns == 1000 && ctx.decrementer == cases[i].counter);
    }
    puts("PASS: decrementer startup, fractional ticks, long sessions and 32-bit wraps");
    return 0;
}
'''.replace("@HZ@", str(hz)).replace("@BODY@", body).replace("@CASES@", ",\n".join(cases))
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    c = output / "decrementer.c"
    c.write_text(harness, encoding="utf-8")
    exe = output / "decrementer.exe"
    subprocess.run([str(args.compiler), "/nologo", "/std:c11", "/W4", "/WX", "/O2",
                    "/UNDEBUG", str(c), f"/Fo{output / 'decrementer.obj'}", f"/Fe{exe}"],
                   check=True)
    subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
