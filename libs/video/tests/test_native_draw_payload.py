"""Link the production owned payload arena into a deterministic C fixture.

Standalone from the SDK root in a VS developer environment:
  python -m unittest discover -s libs/video/tests -p test_native_draw_payload.py -v
Set CLANG_CL if clang-cl is not on PATH. No renderer, device, or game is used.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class NativeDrawPayload(unittest.TestCase):
    def test_actual_source_owned_bytes_and_transactional_failures(self):
        here = Path(__file__).resolve().parent
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a Visual Studio developer environment')
        with tempfile.TemporaryDirectory(prefix='ps3recomp-native-draw-payload-') as directory:
            root = Path(directory)
            executable = root/'test_native_draw_payload.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I'+str(here.parent), str(here/'test_native_draw_payload.c'),
                       str(here.parent/'rsx_native_draw_payload.c'),
                       '/Fo'+str(root)+'/', '/Fe'+str(executable)]
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
            print(f'[native-draw-payload] compile_exit={built.returncode}')
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=15)
            print(f'[native-draw-payload] fixture_exit={ran.returncode}')
            self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)
            self.assertIn('actual-source native draw payload tests passed', ran.stdout)
            print(ran.stdout.strip())


if __name__ == '__main__':
    # Direct invocation keeps successful unittest status off PowerShell's
    # native-stderr error path. Discovery remains supported by the build suite.
    program = unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2), exit=False)
    code = 0 if program.result.wasSuccessful() else 1
    print(f'[native-draw-payload] TEST_EXIT={code}')
    raise SystemExit(code)
