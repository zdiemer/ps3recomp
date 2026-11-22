#!/usr/bin/env python3
"""Compile the actual interpreter and verify ILH lane replication."""
from __future__ import annotations

import argparse
import subprocess
import sys
import os
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    # CI's generic tools/test_*.py loop runs this bare: default to the
    # platform compiler and a temporary directory.
    default_compiler = "clang-cl" if sys.platform == "win32" else os.environ.get("CC", "cc")
    parser.add_argument("--compiler", type=Path, default=Path(default_compiler))
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.output_dir is None:
        args.output_dir = Path(tempfile.mkdtemp(prefix="spu-interpreter-"))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    binary = args.output_dir / "spu-interpreter-ilh.exe"
    source = ROOT / "runtime" / "spu"
    if "cl" in args.compiler.stem.lower().split("-")[-1:]:
        command = [str(args.compiler), "/nologo", "/std:c11", "/O1", "/UNDEBUG", "/D_CRT_SECURE_NO_WARNINGS",
           f"/I{ROOT / 'include'}", f"/I{source}",
           str(source / "tests" / "test_spu_interpreter_ilh.c"),
           str(source / "spu_interp.c"), f"/Fo:{args.output_dir.resolve()}\\", f"/Fe:{binary}"]
    else:
        command = [str(args.compiler), "-std=gnu17", "-O1", "-UNDEBUG", f"-I{ROOT / 'include'}",
                   f"-I{source}", str(source / "tests" / "test_spu_interpreter_ilh.c"), str(source / "spu_interp.c"),
                   "-lm", "-o", str(binary)]
    subprocess.run(command, check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
