#!/usr/bin/env python3
"""Compile the actual interpreter and test RI10 opcodes the decoder missed."""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    binary = args.output_dir / "spu-interpreter-ri10.exe"
    source = ROOT / "runtime" / "spu"
    subprocess.run(
        [str(args.compiler), "/nologo", "/std:c11", "/O1", "/UNDEBUG",
         f"/I{ROOT / 'include'}", f"/I{source}",
         str(source / "tests" / "test_spu_interpreter_ri10.c"),
         str(source / "spu_interp.c"), f"/Fo:{args.output_dir.resolve()}\\", f"/Fe:{binary}"],
        check=True,
    )
    subprocess.run([str(binary)], check=True, timeout=10)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
