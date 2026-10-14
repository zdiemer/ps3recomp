#!/usr/bin/env python3
"""Compile the actual taskset shutdown/destroy bodies and guard guest bytes."""
from __future__ import annotations

import argparse
import hashlib
import re
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "libs" / "spurs" / "cellSpurs.c"
HEADER = ROOT / "libs" / "spurs" / "cellSpurs.h"


def extract_function(text: str, name: str) -> str:
    match = re.search(
        rf"(?m)^s32 {name}\(CellSpursTaskset\* taskset\)\s*\{{.*?^\}}",
        text,
        re.DOTALL,
    )
    if not match:
        raise SystemExit(f"could not extract {name} from {SOURCE}")
    return match.group(0)


def extract_taskset_layout(text: str) -> str:
    match = re.search(r"(?m)^struct CellSpursTaskset\s*\{.*?^\};", text, re.DOTALL)
    if not match:
        raise SystemExit(f"could not extract CellSpursTaskset from {HEADER}")
    return match.group(0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()

    source = SOURCE.read_text(encoding="utf-8")
    layout = extract_taskset_layout(HEADER.read_text(encoding="utf-8"))
    destroy = extract_function(source, "cellSpursDestroyTaskset")
    shutdown = extract_function(source, "cellSpursShutdownTaskset")
    print("cellSpursDestroyTaskset sha256=" + hashlib.sha256(destroy.encode()).hexdigest())
    print("cellSpursShutdownTaskset sha256=" + hashlib.sha256(shutdown.encode()).hexdigest())

    harness = r"""
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
typedef int32_t s32;
typedef uint8_t u8;
typedef uint32_t u32;
typedef struct CellSpurs CellSpurs;
typedef struct CellSpursTaskset CellSpursTaskset;
"""
    program = harness + "\n" + layout + r"""
#define CELL_SPURS_TASK_ERROR_NULL_POINTER ((s32)0x80410901)
#define CELL_OK ((s32)0)
#define GUEST_PTR(ptr, type) ((type)(ptr))
"""
    program += "\n" + destroy + "\n" + shutdown + r"""
int main(void) {
    _Static_assert(offsetof(CellSpursTaskset, initialized) == 0, "guest offset 0");
    _Static_assert(offsetof(CellSpursTaskset, taskCount) == 4, "guest offset 4");
    _Static_assert(offsetof(CellSpursTaskset, shutdownRequested) == 8, "guest offset 8");
    unsigned char memory[sizeof(CellSpursTaskset)];
    unsigned char expected[sizeof memory];
    for (size_t i = 0; i < sizeof memory; ++i) memory[i] = (unsigned char)(i * 37u + 11u);
    /* The first words stand in for a live BE running-bitset/header. These
       include nonzero values so the old native field stores are observable. */
    memory[0] = 0x91; memory[1] = 0x27; memory[2] = 0x44; memory[3] = 0x08;
    memory[4] = 0x80; memory[5] = 0x00; memory[6] = 0x00; memory[7] = 0x01;
    memory[8] = 0x12; memory[9] = 0x34; memory[10] = 0x56; memory[11] = 0x78;
    memcpy(expected, memory, sizeof memory);
    CellSpursTaskset* taskset = (CellSpursTaskset*)memory;
    assert(cellSpursShutdownTaskset(taskset) == CELL_OK);
    assert(memcmp(memory, expected, sizeof memory) == 0);
    assert(cellSpursDestroyTaskset(taskset) == CELL_OK);
    assert(memcmp(memory, expected, sizeof memory) == 0);
    assert(cellSpursShutdownTaskset(NULL) == CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert(cellSpursDestroyTaskset(NULL) == CELL_SPURS_TASK_ERROR_NULL_POINTER);
    assert(memcmp(memory, expected, sizeof memory) == 0);
    puts("actual taskset shutdown/destroy preserve all guest bytes");
    return 0;
}
"""
    args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="taskset-guest-fields-") as tmp:
        tmp_path = Path(tmp)
        cfile = tmp_path / "fixture.c"
        binary = args.output_dir / "taskset-guest-fields.exe"
        cfile.write_text(program, encoding="utf-8", newline="\n")
        subprocess.run(
            [str(args.compiler), "/nologo", "/std:c11", "/W4", "/WX", "/O2",
             str(cfile), f"/Fo:{args.output_dir.resolve()}\\", f"/Fe:{binary}"],
            check=True,
        )
        subprocess.run([str(binary)], check=True, timeout=5)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
