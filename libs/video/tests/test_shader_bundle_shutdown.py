"""Actual terminal shader-bundle API with real worker/lock/atomic persistence.

Run in a VS developer environment with clang-cl available or CLANG_CL set.
No GPU, renderer, game assets, or display are used.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest


class ShaderBundleShutdown(unittest.TestCase):
    def test_actual_owned_shutdown(self):
        here = Path(__file__).resolve().parent
        sdk = here.parents[2]
        source = (here.parent / 'rsx_live_draw.c').read_text(encoding='utf-8')
        begin = source.index('/* BEGIN RSX SHADER BUNDLE SHUTDOWN')
        end = source.index('/* END RSX SHADER BUNDLE SHUTDOWN */', begin)
        block = source[begin:end]
        worker = block[block.index('static DWORD WINAPI shader_bundle_shutdown_worker('):
                       block.index('int rsx_live_draw_finish_shader_bundle(')]
        self.assertNotIn('g.', worker)
        self.assertNotIn('fprintf', worker)
        self.assertNotIn('TerminateThread', block)
        def function(signature):
            a = source.index(signature)
            b = source.index('\n}', a) + 2
            return source[a:b] + '\n'
        append = function('static void shader_bundle_append_cache(')
        release = function('static void shader_blob_cache_release(shader_blob_cache_t* cache)\n{')
        types = re.search(r'#define MAX_SHADER_BLOBS \d+', source).group() + '\n'
        for name in ('shader_blob_t', 'shader_blob_cache_t'):
            b = source.index('} ' + name + ';') + len('} ' + name + ';')
            a = source.rfind('typedef struct {', 0, b)
            types += source[a:b] + '\n'
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a VS developer environment')
        with tempfile.TemporaryDirectory(prefix='ps3recomp-shader-shutdown-') as directory:
            root = Path(directory)
            for name, body in (('shader_shutdown_actual.inc', block),
                               ('shader_cache_types_actual.inc', types),
                               ('shader_cache_append_actual.inc', append),
                               ('shader_cache_release_actual.inc', release)):
                (root / name).write_text(body, encoding='utf-8')
            executable = root / 'test_shader_bundle_shutdown.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                '/I' + str(root), '/I' + str(here.parent), '/I' + str(sdk / 'include'),
                str(here / 'test_shader_bundle_shutdown.c'),
                '/Fo' + str(root / 'test_shader_bundle_shutdown.obj'), '/Fe' + str(executable)]
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
            print(f'[shader-shutdown] compile_exit={built.returncode}')
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=30)
            print(f'[shader-shutdown] fixture_exit={ran.returncode}')
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('5 actual-source shader bundle shutdown cases passed', ran.stdout)
            print(ran.stdout.strip())


if __name__ == '__main__':
    program = unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2), exit=False)
    code = 0 if program.result.wasSuccessful() else 1
    print(f'[shader-shutdown] TEST_EXIT={code}')
    raise SystemExit(code)
