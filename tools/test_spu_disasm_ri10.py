#!/usr/bin/env python3
"""Check immediate opcode decoding and generated interpreter tables."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))
import spu_disasm


def word(op8: int, imm: int, ra: int = 24, rt: int = 17) -> int:
    return (op8 << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt


def main() -> int:
    forms = {0x06: "orbi", 0x45: "xorhi", 0x46: "xorbi"}
    for opcode, mnemonic in forms.items():
        got = spu_disasm.spu_decode(word(opcode, 0x25), 0).mnemonic
        if got != mnemonic:
            raise SystemExit(f"op8 0x{opcode:02X} decoded as {got!r}, expected {mnemonic!r}")

    generated = subprocess.run(
        [sys.executable, str(TOOLS / "gen_spu_interp.py")],
        check=True, capture_output=True, text=True,
    ).stdout
    committed = (ROOT / "runtime" / "spu" / "spu_interp_tables.inc").read_text(encoding="utf-8")
    if generated != committed:
        raise SystemExit("runtime/spu/spu_interp_tables.inc is stale; regenerate it from tools/gen_spu_interp.py")
    print("ORBI, XORHI, XORBI decode and interpreter tables match the source decoder")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
