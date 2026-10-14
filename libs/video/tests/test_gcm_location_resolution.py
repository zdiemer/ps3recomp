"""Compile the actual GCM resolver functions and exercise overlapping spaces."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def fixture():
    source = (ROOT / 'libs/video/cellGcmSys.c').read_text(encoding='utf-8')
    functions = []
    for name in ('cellGcmResolveOffset', 'cellGcmResolveIO', 'cellGcmResolveLocated'):
        begin = source.index('\nu32 ' + name + '(') + 1
        end = source.index('\n}', begin) + 2
        functions.append(source[begin:end])
    return r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint16_t u16;
static struct { u32 localAddress; } s_config;
static unsigned char s_local_offset_page[1024];
static u16 s_ea_address_table[65536];
''' + '\n'.join(functions) + r'''
int main(void) {
    s_config.localAddress = 0xC0000000;
    const u32 offset = 0x02A1F380, io = 0x1201F380;
    s_ea_address_table[offset >> 20] = 0x120;
    assert(cellGcmResolveLocated(0, offset) == io);
    assert(cellGcmResolveLocated(1, offset) == 0xC2A1F380);
    s_local_offset_page[offset >> 20] = 1;
    /* Known MAIN remains IO even after the guest derives a local alias. */
    assert(cellGcmResolveLocated(0, offset) == io);
    assert(cellGcmResolveLocated(1, offset) == 0xC2A1F380);
    assert(cellGcmResolveOffset(offset) == 0xC2A1F380);
    assert(cellGcmResolveIO(offset) == io);
    /* Mapping changes, page boundaries, and unmapped sentinels. */
    s_ea_address_table[offset >> 20] = 0x234;
    assert(cellGcmResolveLocated(0, offset) == 0x2341F380);
    assert(cellGcmResolveLocated(0, 0x02AFFFFF) == 0x234FFFFF);
    assert(cellGcmResolveLocated(0, 0x02B00000) == 0xC2B00000);
    s_ea_address_table[offset >> 20] = 0xFFFF;
    assert(cellGcmResolveIO(offset) == 0);
    assert(cellGcmResolveLocated(0, offset) == 0xC2A1F380);
    s_ea_address_table[offset >> 20] = 0;
    assert(cellGcmResolveLocated(0, offset) == 0xC2A1F380);
    s_ea_address_table[0] = 0x100;
    s_local_offset_page[0] = 1;
    assert(cellGcmResolveLocated(0, 0) == 0x10000000);
    assert(cellGcmResolveLocated(1, 0) == 0xC0000000);
    puts("GCM location resolution actual-source contract passed");
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default=os.environ.get('CC', 'clang-cl'))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='gcm-location-') as directory:
        directory = Path(directory)
        source, exe, obj = (directory / name for name in ('fixture.c', 'fixture.exe', 'fixture.obj'))
        source.write_text(fixture(), encoding='utf-8')
        subprocess.run([args.compiler, '/nologo', '/TC', '/std:c11', '/W4',
                        str(source), f'/Fe:{exe}', f'/Fo:{obj}'], check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    main()
