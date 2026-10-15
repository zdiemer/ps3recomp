"""Run the actual pipeline-recipe implementation on D3D12 WARP.

From a Visual Studio developer environment:
  python libs/video/tests/test_pipeline_recipes.py
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class PipelineRecipes(unittest.TestCase):
    def test_actual_source_roundtrip_corruption_and_render(self):
        here = Path(__file__).resolve().parent
        compiler = os.environ.get("CLANG_CL") or shutil.which("clang-cl")
        if not compiler:
            self.skipTest("requires clang-cl in a Visual Studio developer environment")
        with tempfile.TemporaryDirectory(prefix="ps3recomp-pipeline-recipes-") as directory:
            root = Path(directory)
            executable = root / "test_pipeline_recipes.exe"
            command = [compiler, "/nologo", "/TC", "/W4", "/WX", "/O2",
                       "/I" + str(here.parent), str(here / "test_pipeline_recipes.c"),
                       str(here.parent / "rsx_pipeline_recipes.c"),
                       "/Fo" + str(root) + "/", "/Fe" + str(executable), "/link",
                       "d3d12.lib", "dxgi.lib", "dxguid.lib", "d3dcompiler.lib"]
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120)
            print(f"[pipeline-recipes] compile_exit={built.returncode}")
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=120)
            print(f"[pipeline-recipes] fixture_exit={ran.returncode}")
            print(ran.stdout, end="")
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("recipe key mutations and unsupported stages rejected without driver creation", ran.stdout)
            self.assertIn("rejected archive truncated before driver create", ran.stdout)
            self.assertIn("rejected archive version before driver create", ran.stdout)
            self.assertIn("rejected archive second-record checksum before driver create", ran.stdout)
            self.assertTrue("rejected archive oversized sparse archive before driver create" in ran.stdout or
                            "oversized sparse-file test unavailable" in ran.stdout)
            self.assertIn("root signature mismatch rejected before driver creation", ran.stdout)
            self.assertIn("large aggregate archive roundtrip passed:", ran.stdout)
            self.assertIn("actual-source pipeline recipe WARP roundtrip passed", ran.stdout)


if __name__ == "__main__":
    program = unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2), exit=False)
    code = 0 if program.result.wasSuccessful() else 1
    print(f"[pipeline-recipes] TEST_EXIT={code}")
    raise SystemExit(code)
