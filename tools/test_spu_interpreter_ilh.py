#!/usr/bin/env python3
"""Run the interpreter immediate-load regression and existing self-test."""
from __future__ import annotations

import argparse
import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def run(compiler: str, output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    source = ROOT / "runtime" / "spu"
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_", "GH3_"))}
    for name in ("test_spu_interpreter_ilh", "spu_interp_selftest"):
        binary = output_dir / (name + (".exe" if os.name == "nt" else ""))
        sources = [str(source / "tests" / f"{name}.c"), str(source / "spu_interp.c")]
        if os.name == "nt":
            command = [compiler, "/nologo", "/std:c11", "/O1", "/UNDEBUG", "/D_CRT_SECURE_NO_WARNINGS",
                       f"/I{ROOT / 'include'}", f"/I{source}", *sources, f"/Fe:{binary}"]
        else:
            command = [compiler, "-std=gnu17", "-O1", "-UNDEBUG",
                       f"-I{ROOT / 'include'}", f"-I{source}", *sources, "-lm", "-o", str(binary)]
        subprocess.run(command, check=True, cwd=output_dir)
        subprocess.run([str(binary)], check=True, timeout=10, env=env, cwd=output_dir)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", default=os.environ.get("CC", "clang-cl" if os.name == "nt" else "cc"))
    parser.add_argument("--output-dir", type=Path)
    args = parser.parse_args()
    if args.output_dir:
        run(str(args.compiler), args.output_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="spu-ilh-") as tmp:
            run(str(args.compiler), Path(tmp))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
