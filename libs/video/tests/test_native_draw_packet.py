"""Exercise the actual native draw packet recorders with real C COM ABI types.

Standalone from the SDK root, in a Visual Studio developer environment:
  python -m unittest discover -s libs/video/tests -p test_native_draw_packet.py -v
Set CLANG_CL when clang-cl is not on PATH. Only one standalone fixture is
compiled. The executable records mock COM calls; it creates no D3D device.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class NativeDrawPacket(unittest.TestCase):
    def test_actual_recorders_order_arguments_and_immutable_packet(self):
        here = Path(__file__).resolve().parent
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a Visual Studio developer environment')
        source = (here.parent / 'rsx_live_draw.c').read_text(encoding='utf-8')
        begin = '/* BEGIN RSX NATIVE DRAW PACKET RECORDERS */'
        end = '/* END RSX NATIVE DRAW PACKET RECORDERS */'
        self.assertEqual(source.count(begin), 1)
        self.assertEqual(source.count(end), 1)
        body = source[source.index(begin):source.index(end)]
        self.assertEqual(hashlib.sha256(body.encode('utf-8')).hexdigest(),
                         'f4892f3952912cc0781271e6cd0e73bcc7b6769b4073c8105912127579f8c6ad',
                         'Production packet recorders changed: review call order and re-audit fixture guard')
        for function in ('begin', 'targets', 'bind', 'draw', 'end'):
            self.assertEqual(body.count('void rsx_native_draw_packet_'+function+'('), 1)
        with tempfile.TemporaryDirectory(prefix='ps3recomp-native-draw-packet-') as directory:
            root = Path(directory)
            (root / 'native_draw_packet_actual.inc').write_text(body, encoding='utf-8')
            executable = root / 'test_native_draw_packet.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I'+str(here.parent), '/I'+str(root),
                       str(here/'test_native_draw_packet.c'),
                       '/Fo'+str(root/'test_native_draw_packet.obj'), '/Fe'+str(executable)]
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout+ran.stderr)
            self.assertIn('208896 actual-source native draw packet cases passed', ran.stdout)
            self.assertIn('phase ordering, arguments, immutability and null guards passed', ran.stdout)
            print(ran.stdout.strip())


if __name__ == '__main__':
    unittest.main()
