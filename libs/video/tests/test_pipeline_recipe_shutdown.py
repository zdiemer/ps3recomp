"""Compile the terminal renderer operation with real Windows worker/wait APIs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PipelineRecipeShutdown(unittest.TestCase):
    def test_actual_owned_writer(self):
        here = Path(__file__).resolve().parent
        source = (here.parent / 'rsx_live_draw.c').read_text(encoding='utf-8')
        begin = source.index('/* BEGIN RSX PIPELINE RECIPE SHUTDOWN */')
        end = source.index('/* END RSX PIPELINE RECIPE SHUTDOWN */', begin)
        block = source[begin:end]
        worker = block[block.index('static DWORD WINAPI ld_pipeline_recipe_write_worker('):
                       block.index('int rsx_live_draw_finish_pipeline_recipes(')]
        self.assertNotIn('g.', worker)
        self.assertNotIn('fprintf', worker)
        self.assertNotIn('TerminateThread', block)
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a VS developer environment')
        with tempfile.TemporaryDirectory(prefix='ps3recomp-recipe-shutdown-') as directory:
            root = Path(directory)
            (root / 'pipeline_recipe_shutdown_actual.inc').write_text(block, encoding='utf-8')
            exe = root / 'test_recipe_shutdown.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I' + str(root), str(here / 'test_pipeline_recipe_shutdown.c'),
                       '/Fo' + str(root / 'test_recipe_shutdown.obj'), '/Fe' + str(exe)]
            built = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            print(ran.stdout)
            self.assertIn('5 actual-source pipeline recipe shutdown cases passed', ran.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
