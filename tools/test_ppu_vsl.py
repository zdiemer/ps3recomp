"""Execute decoded/lifted whole-vector shifts, including cross-byte carries.

The architectural input is a big-endian 128-bit bitstring. For defined VSL
inputs every byte of vB has the same low three count bits (AltiVec PEM).
"""
import os
from pathlib import Path
import random
import shutil
import subprocess
import tempfile
from types import SimpleNamespace

from ppu_disasm import decode
from ppu_lifter import PPULifter


def main():
    cc = shutil.which(os.environ.get("CC", "clang")) or shutil.which("cc")
    if not cc:
        raise SystemExit("A C compiler is required for VSL execution tests")
    rng = random.Random(452)
    cases = []
    for shift in range(8):
        for a in (bytes(range(16)), bytes([1]*16), bytes([255]*16),
                  bytes.fromhex("80000000000000000000000000000001"),
                  *(rng.randbytes(16) for _ in range(100))):
            b = bytes((rng.randrange(32)<<3)|shift for _ in range(16))
            expected = ((int.from_bytes(a,"big")<<shift)&((1<<128)-1)).to_bytes(16,"big")
            cases.append((a,b,expected))
    src = """#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct { uint8_t vr[32][16]; uint32_t vscr; } Context;
typedef struct { uint8_t a[16], b[16], expected[16]; } Case;
"""
    for dst in (1,2,3):
        insn = decode(0x100001C4 | (dst<<21) | (1<<16) | (2<<11),0x1000)
        assert insn.mnemonic=="vsl",insn
        fn=PPULifter().lift_function([insn,SimpleNamespace(addr=0x1004,mnemonic="blr",operands="")],0x1000,0x1008)
        body="\n".join(line for line in fn.body_lines if "return" not in line)
        src+=f"static void run{dst}(Context* ctx) {{\n{body}\n}}\n"
    rows=["{"+",".join("{"+",".join(map(str,v))+"}" for v in row)+"}" for row in cases]
    src+="static const Case cases[]={\n"+",\n".join(rows)+"};\n"
    src+="""int main(void) {
    void (*runs[])(Context*)={run1,run2,run3};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) for(int alias=0;alias<3;alias++) {
        Context c; memset(&c,0xA5,sizeof c);
        memcpy(c.vr[1],cases[i].a,16); memcpy(c.vr[2],cases[i].b,16);
        c.vscr=0x10001u; runs[alias](&c);
        if(memcmp(c.vr[alias+1],cases[i].expected,16) || c.vscr!=0x10001u) {
            fprintf(stderr,"case=%u destination=%d failed\\n",i,alias+1); return 1;
        }
        if(alias!=0 && memcmp(c.vr[1],cases[i].a,16)) return 2;
        if(alias!=1 && memcmp(c.vr[2],cases[i].b,16)) return 3;
    }
    /* Haze's invalidation idiom: splat(1) << splat(7) forms a high-bit
     * mask. Clearing it must preserve the distinct low cache-age bits. */
    Context c={0}; memset(c.vr[1],1,16); memset(c.vr[2],7,16); run3(&c);
    for(int i=0;i<16;i++) if(c.vr[3][i]!=0x80 ||
        ((uint8_t)(0x80|i)&(uint8_t)~c.vr[3][i])!=i) return 4;
    puts("VSL: 2496 executions passed, plus cache-age mask preservation");
    return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c=Path(tmp)/"vsl.c"
        exe=Path(tmp)/("vsl.exe" if os.name=="nt" else "vsl")
        c.write_text(src)
        subprocess.run([cc,"-std=c11","-O2",str(c),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True)


if __name__=="__main__":
    main()
