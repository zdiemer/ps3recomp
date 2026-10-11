"""Execute lifted VSUM4UBS against unsigned byte/word and aliasing cases.

Reference: IBM AltiVec PEM, Vector Sum Across Partial (1/4) Unsigned
Integer Byte Saturate. Register storage is in architectural byte order.
"""
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace

from ppu_lifter import PPULifter
from ppu_disasm import decode


def body(dst):
    insn = decode(0x10000608 | (dst << 21) | (1 << 16) | (2 << 11), 0x1000)
    assert insn.mnemonic == "vsum4ubs", insn
    insns = [insn,
             SimpleNamespace(addr=0x1004, mnemonic="blr", operands="")]
    lifted = PPULifter().lift_function(insns, 0x1000, 0x1008)
    return "\n".join(line for line in lifted.body_lines if "return" not in line)


def main():
    cc = shutil.which(os.environ.get("CC", "clang")) or shutil.which("cc")
    if not cc:
        raise SystemExit("A C compiler is required for VSUM4UBS execution tests")
    rng = random.Random(0x608)
    cases = [(bytes(range(16)), [0, 0x01020304, 0xFFFFFFFF, 0xFFFFFFFA]),
             (bytes([255] * 16), [0, 0xFFFFFC03, 0xFFFFFC04, 0xFFFFFFFF]),
             (bytes(16), [0, 1, 0x80000000, 0xFFFFFFFF])]
    for _ in range(1000):
        a = rng.randbytes(16)
        b = [rng.getrandbits(32) if rng.randrange(2) else 0xFFFFFFFF-rng.randrange(1024)
             for _ in range(4)]
        cases.append((a, b))
    rows = []
    for a, b in cases:
        sums = [sum(a[i*4:i*4+4]) + b[i] for i in range(4)]
        expected = struct.pack(">4I", *(min(v, 0xFFFFFFFF) for v in sums))
        sat = int(any(v > 0xFFFFFFFF for v in sums))
        arrays = [a, struct.pack(">4I", *b), expected]
        rows.append("{" + ",".join("{"+",".join(map(str,v))+"}" for v in arrays)
                    + f",{sat}" + "}")
    src = """#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct { uint8_t vr[32][16]; uint32_t vscr; } Context;
typedef struct { uint8_t a[16], b[16], expected[16]; uint32_t sat; } Case;
"""
    for dst in (1, 2, 3):
        src += f"static void run{dst}(Context* ctx) {{\n{body(dst)}\n}}\n"
    src += "static const Case cases[]={\n" + ",\n".join(rows) + "};\n"
    src += """int main(void) {
    void (*runs[])(Context*)={run1,run2,run3};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++)
    for(int alias=0;alias<3;alias++) for(int prior=0;prior<2;prior++) {
        Context c; memset(&c,0xA5,sizeof c);
        memcpy(c.vr[1],cases[i].a,16); memcpy(c.vr[2],cases[i].b,16);
        c.vscr=0x10000u | prior; runs[alias](&c);
        if(memcmp(c.vr[alias+1],cases[i].expected,16) ||
           c.vscr != (0x10000u | prior | cases[i].sat)) {
            fprintf(stderr,"case=%u destination=%d priorSAT=%d failed\\n",i,alias+1,prior);
            return 1;
        }
        if(alias!=0 && memcmp(c.vr[1],cases[i].a,16)) return 2;
        if(alias!=1 && memcmp(c.vr[2],cases[i].b,16)) return 3;
    }
    puts("VSUM4UBS: 6018 executions passed (lanes, saturation, sticky SAT, aliasing)");
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c = Path(tmp)/"sum.c"
        exe = Path(tmp)/("sum.exe" if os.name=="nt" else "sum")
        c.write_text(src)
        subprocess.run([cc,"-std=c11","-O2",str(c),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__ == "__main__":
    main()
