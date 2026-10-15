"""Compile the actual ld_flush body against deterministic COM/Win32 substitutes.

Standalone from the SDK root:
  python -m unittest discover -s libs/video/tests -p test_live_flush.py -v
Set CLANG_CL to a clang-cl executable if it is not on PATH. Generated source,
objects and executables stay in a private temporary directory.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LiveFlush(unittest.TestCase):
    def test_actual_flush_completion_and_failure_paths(self):
        here = Path(__file__).resolve().parent
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a Visual Studio developer environment')
        source = (here.parent / 'rsx_live_draw.c').read_text(encoding='utf-8')
        begin = source.index('/* BEGIN RSX SLOW FLUSH WRAPPER */')
        end = source.index('/* END RSX SLOW FLUSH WRAPPER */', begin)
        body = source[begin:end]
        self.assertEqual(body.count('static void ld_flush('), 1)
        # The production reclaim hook must be exercised, not inserted by this
        # fixture into an older body that did not contain it.
        self.assertIn('ld_sampler_pages_completed();', body)
        def function(signature):
            start = source.index(signature)
            stop = source.index('\n}', start) + 2
            return source[start:stop] + '\n'
        pages = function('static void ld_sampler_pages_completed(void)') + function(
            'static int ld_sampler_pages_advance(void)')
        upload = function('static int ld_sampler_pages_before_upload(void)')
        with tempfile.TemporaryDirectory(prefix='ps3recomp-live-flush-') as directory:
            root = Path(directory)
            (root / 'live_flush_actual.inc').write_text(body, encoding='utf-8')
            (root / 'sampler_pages_actual.inc').write_text(pages, encoding='utf-8')
            (root / 'sampler_upload_actual.inc').write_text(upload, encoding='utf-8')
            executable = root / 'test_live_flush.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I' + str(root), str(here / 'test_live_flush.c'),
                       '/Fo' + str(root / 'test_live_flush.obj'), '/Fe' + str(executable)]
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(executable)], cwd=root, capture_output=True, text=True, timeout=15)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn('52 actual-source flush/upload cases passed', ran.stdout)
            self.assertIn('bounded sampler page advancement passed', ran.stdout)
            self.assertIn('terminal flush no-op passed', ran.stdout)
            print(ran.stdout.strip())


if __name__ == '__main__':
    unittest.main()
