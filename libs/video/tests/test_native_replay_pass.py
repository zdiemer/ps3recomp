"""Execute production replay/payload modules and packet recorders on WARP.

From the SDK root in a VS developer environment:
  python libs/video/tests/test_native_replay_pass.py
Uses one clang-cl invocation, real Windows D3D12 WARP and synthetic shaders.
No hardware adapter, display/window, game, guest data or audio is used.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class NativeReplayPass(unittest.TestCase):
    def test_actual_source_warp_color_parity_and_cpu_ownership(self):
        here = Path(__file__).resolve().parent
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a Visual Studio developer environment')
        source = (here.parent/'rsx_live_draw.c').read_text(encoding='utf-8')
        begin = '/* BEGIN RSX NATIVE DRAW PACKET RECORDERS */'
        end = '/* END RSX NATIVE DRAW PACKET RECORDERS */'
        self.assertEqual(source.count(begin), 1)
        self.assertEqual(source.count(end), 1)
        body = source[source.index(begin):source.index(end)]
        self.assertEqual(hashlib.sha256(body.encode('utf-8')).hexdigest(),
                         'f4892f3952912cc0781271e6cd0e73bcc7b6769b4073c8105912127579f8c6ad',
                         'Production packet recorder change requires call-order re-audit')
        with tempfile.TemporaryDirectory(prefix='ps3recomp-native-replay-warp-') as directory:
            root = Path(directory)
            (root/'native_draw_packet_actual.inc').write_text(body, encoding='utf-8')
            executable = root/'test_native_replay_pass.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I'+str(here.parent), '/I'+str(root),
                       str(here/'test_native_replay_pass.c'),
                       str(here.parent/'rsx_native_replay_pass.c'),
                       str(here.parent/'rsx_native_draw_payload.c'),
                       '/Fo'+str(root)+'/', '/Fe'+str(executable), '/link',
                       'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'd3dcompiler.lib']
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=90)
            print(f'[native-replay-pass] compile_exit={built.returncode}')
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=90)
            print(f'[native-replay-pass] fixture_exit={ran.returncode}')
            print(ran.stdout, end='')
            self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)
            for depth in (0, 1):
                for draws in (1, 2):
                    self.assertIn(f'WARP replay depth={depth} draws={draws} compared=4096 mismatches=0 CPU-source-mutation passed', ran.stdout)
            self.assertIn('WARP replay rejection=1 passed after completion', ran.stdout)
            self.assertIn('WARP replay rejection=2 passed after completion', ran.stdout)
            self.assertIn('WARP replay rejection=3 passed after completion', ran.stdout)
            for mode in range(1, 9):
                for corruption in ((0, 1, 2) if (mode - 1) % 4 < 2 else (0, 1)):
                    self.assertIn(f'WARP depth-output format={mode} corruption={corruption} ', ran.stdout)
            for draws in (1, 2):
                self.assertIn(f'WARP pull-only draws={draws} owned-SRV-VA passed after original-GPU-buffer overwrite', ran.stdout)
            for format_name in ('RGBA8', 'R32_RRR1'):
                self.assertIn(f'WARP sampled versions format={format_name} draws=3 snapshots=2 reuses=1 compared=4096 mismatches=0 original-overwrite and scrubbed-transitions passed', ran.stdout)
            for rejection in (1, 2, 3, 4):
                self.assertIn(f'WARP snapshot rejection={rejection} passed; zero snapshots published', ran.stdout)
            self.assertIn('actual-source native replay WARP integration passed', ran.stdout)


if __name__ == '__main__':
    program = unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2), exit=False)
    code = 0 if program.result.wasSuccessful() else 1
    print(f'[native-replay-pass] TEST_EXIT={code}')
    raise SystemExit(code)
