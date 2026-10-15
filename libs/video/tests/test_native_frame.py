"""Run an asset-free ordered native-frame dependency chain on D3D12 WARP.

In a VS developer environment: python libs/video/tests/test_native_frame.py
One compiler invocation links actual frame/payload modules. The prior WARP
fixture supplies only setup/recording helpers; its main is not executed.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class NativeFrame(unittest.TestCase):
    def test_actual_source_warp_ordered_dependency_chain(self):
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
                         'f4892f3952912cc0781271e6cd0e73bcc7b6769b4073c8105912127579f8c6ad')
        with tempfile.TemporaryDirectory(prefix='ps3recomp-native-frame-warp-') as directory:
            root = Path(directory)
            (root/'native_draw_packet_actual.inc').write_text(body, encoding='utf-8')
            executable = root/'test_native_frame.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I'+str(here.parent), '/I'+str(root), str(here/'test_native_frame.c'),
                       str(here.parent/'rsx_native_frame.c'),
                       str(here.parent/'rsx_native_draw_payload.c'),
                       str(here.parent/'rsx_native_replay_pass.c'),
                       '/Fo'+str(root)+'/', '/Fe'+str(executable), '/link',
                       'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'd3dcompiler.lib']
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=90)
            print(f'[native-frame] compile_exit={built.returncode}')
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=90)
            print(f'[native-frame] fixture_exit={ran.returncode}')
            print(ran.stdout, end='')
            self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)
            self.assertIn('actual-source ordered native frame WARP dependency chain passed', ran.stdout)
            self.assertIn('native frame unsupported/incomplete rejected', ran.stdout)


if __name__ == '__main__':
    program = unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2), exit=False)
    code = 0 if program.result.wasSuccessful() else 1
    print(f'[native-frame] TEST_EXIT={code}')
    raise SystemExit(code)
