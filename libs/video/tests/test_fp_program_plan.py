"""Compile the actual plan and legacy decoder; compare mutable input behavior."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class ProgramPlanTest(unittest.TestCase):
    def test_actual_c_sources(self):
        here = Path(__file__).resolve().parent
        video = here.parent
        root = video.parents[1]
        compiler = os.environ.get("CLANG_CL") or shutil.which("clang-cl")
        self.assertIsNotNone(compiler, "requires a Visual Studio developer environment")
        with tempfile.TemporaryDirectory(prefix="rsx-fp-plan-") as directory:
            out = Path(directory)
            exe = out / "test.exe"
            cmd = [compiler, "/nologo", "/TC", "/std:c11", "/W4", "/WX", "/O2",
                   # Legacy decompiler prose contains a nested-comment token.
                   "/clang:-Wno-comment",
                   "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(root / "include"),
                   "/I" + str(video), str(here / "test_fp_program_plan.c"),
                   str(video / "rsx_fp_program_plan.c"),
                   str(video / "rsx_fp_decompiler.c"), "/Fo" + str(out) + "/",
                   "/Fe" + str(exe)]
            build = subprocess.run(cmd, cwd=out, text=True, capture_output=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(exe)], cwd=out, text=True, capture_output=True, timeout=120)
            print(run.stdout, end="")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("native FP program plan: PASS", run.stdout)


if __name__ == "__main__":
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
