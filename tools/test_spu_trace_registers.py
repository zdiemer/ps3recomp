"""Trace the destination of four-operand SPU instructions, not their control."""
import unittest
from spu_disasm import spu_decode, RRR_TABLE
from spu_lifter import SPULifter


class TraceRegisterTests(unittest.TestCase):
    def test_four_operand_results(self):
        for opcode, mnemonic in RRR_TABLE.items():
            with self.subTest(mnemonic=mnemonic):
                # rt=24, ra=26, rb=25, rc=80: all fields deliberately differ.
                raw = opcode << 28 | 24 << 21 | 25 << 14 | 26 << 7 | 80
                insn = spu_decode(raw, 0x1000)
                function = SPULifter(trace=True).lift_function([insn], 0x1000, 0x1004)
                body = "\n".join(function.body_lines)
                self.assertIn("ctx->gpr[24] =", body)
                self.assertIn("spu_trace_rt(ctx, 24);", body)
                self.assertNotIn("spu_trace_rt(ctx, 80);", body)


if __name__ == "__main__":
    unittest.main()
