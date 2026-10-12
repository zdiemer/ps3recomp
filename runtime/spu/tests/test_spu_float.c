/* SPU ISA v1.2 section 9.1 and instruction pages 202-234.
 * Build as C17 or C++20. Optional argument: binary oracle vectors written by
 * test_spu_float_oracle.py. Expectations use SPU bits, never host NaN rules. */
#include "../spu_helpers.h"
#include <stdio.h>
#include <stdlib.h>

static unsigned checked;
static void check(const char* name, u128 actual, uint32_t expected) {
    for (int i = 0; i < 4; ++i) {
        ++checked;
        if (actual._u32[i] != expected) {
            fprintf(stderr, "%s lane %d: got %08x, expected %08x\n", name, i,
                    actual._u32[i], expected);
            exit(1);
        }
    }
}
#define V(x) spu_splat_u32(x)
int main(int argc, char** argv) {
    check("near-plane depth overflow", spu_fm(V(0x7f7fffff), V(0x43800000)), 0x7fffffff);
    check("negative overflow", spu_fm(V(0xff7fffff), V(0x43800000)), 0xffffffff);
    check("extended addition", spu_fa(V(0x7f7fffff), V(0x7f7fffff)), 0x7fffffff);
    check("extended operand", spu_fm(V(0x7f800000), V(0x3f000000)), 0x7f000000);
    check("overflowing product cancels", spu_fms(V(0x7f800000), V(0x40000000), V(0x7fffffff)), 0x74000000);
    check("no intermediate saturation", spu_fms(V(0x7fffffff), V(0x40000000), V(0x7fffffff)), 0x7fffffff);
    check("fused low product bits", spu_fms(V(0x3f800001), V(0x3f800001), V(0x3f800002)), 0x28800000);
    check("tiny subtraction affects truncation", spu_fs(V(0x3f800000), V(0x00800000)), 0x3f7fffff);
    check("negative tiny subtraction", spu_fa(V(0xbf800000), V(0x00800000)), 0xbf7fffff);
    check("truncate instead of nearest", spu_fa(V(0x3f800000), V(0x33c00000)), 0x3f800000);
    check("underflow", spu_fm(V(0x80800000), V(0x3f000000)), 0);
    check("denormal inputs", spu_fa(V(0x007fffff), V(0x807fffff)), 0);
    check("positive result zero", spu_fm(V(0x80000000), V(0x3f800000)), 0);
    check("extended comparison", spu_fcgt(V(0x7fffffff), V(0x7f800000)), 0xffffffff);
    check("negative extended comparison", spu_fcgt(V(0xff800000), V(0xffffffff)), 0xffffffff);
    check("extended equality", spu_fceq(V(0x7fffffff), V(0x7fffffff)), 0xffffffff);
    check("denormal equals zero", spu_fceq(V(0x807fffff), V(0)), 0xffffffff);
    check("magnitude comparison", spu_fcmgt(V(0xffffffff), V(0x7f800000)), 0xffffffff);
    check("magnitude equality", spu_fcmeq(V(0xffffffff), V(0x7fffffff)), 0xffffffff);
    check("signed extended conversion", spu_cflts(V(0x7fffffff), 173), 0x7fffffff);
    check("negative signed conversion", spu_cflts(V(0xffffffff), 173), 0x80000000);
    check("unsigned extended conversion", spu_cfltu(V(0x7fffffff), 173), 0xffffffff);
    check("negative unsigned conversion", spu_cfltu(V(0xffffffff), 173), 0);
    check("signed integer truncation", spu_csflt(V(0x7fffffff), 155), 0x4effffff);
    check("unsigned integer truncation", spu_cuflt(V(0xffffffff), 155), 0x4f7fffff);
    check("integer min", spu_csflt(V(0x80000000), 155), 0xcf000000);
    check("extended FI base", spu_fi(V(0), V(0x7fffffff)), 0x7ffffc00);
    /* Test different signs and exponents in the same SIMD register. */
    u128 lanes; lanes._u32[0]=0x7f7fffff; lanes._u32[1]=0xff7fffff;
    lanes._u32[2]=0x00800000; lanes._u32[3]=0x80000000;
    u128 result=spu_fm(lanes,V(0x43800000));
    const uint32_t expected[]={0x7fffffff,0xffffffff,0x04800000,0};
    for(int i=0;i<4;i++) check("mixed lane",V(result._u32[i]),expected[i]);
    if (argc > 1) {
        FILE* f=fopen(argv[1],"rb"); if(!f) return 2;
        uint32_t row[6];
        while (fread(row,sizeof row,1,f)==1) {
            u128 a=V(row[1]), b=V(row[2]), c=V(row[3]), out=spu_zero();
            int imm=(int)row[4];
            switch(row[0]) {
            case 0: out=spu_fa(a,b); break; case 1: out=spu_fs(a,b); break;
            case 2: out=spu_fm(a,b); break; case 3: out=spu_fma(a,b,c); break;
            case 4: out=spu_fms(a,b,c); break; case 5: out=spu_fnms(a,b,c); break;
            case 6: out=spu_fcgt(a,b); break; case 7: out=spu_fceq(a,b); break;
            case 8: out=spu_fcmgt(a,b); break; case 9: out=spu_fcmeq(a,b); break;
            case 10: out=spu_cflts(a,imm); break; case 11: out=spu_cfltu(a,imm); break;
            case 12: out=spu_csflt(a,imm); break; case 13: out=spu_cuflt(a,imm); break;
            case 14: out=spu_fi(a,b); break; default: return 3;
            }
            char label[96]; snprintf(label,sizeof label,"oracle op %u a=%08x b=%08x c=%08x imm=%d",row[0],row[1],row[2],row[3],imm);
            check(label,out,row[5]);
        }
        if(ferror(f)) return 4;
        fclose(f);
    }
    printf("SPU extended single precision: %u lane checks passed.\n",checked);
    return 0;
}
