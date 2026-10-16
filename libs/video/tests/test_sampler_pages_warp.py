"""Real GPU descriptor lifetime/output across four actual sampler pages."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


class SamplerPagesWarp(unittest.TestCase):
    def test_actual_helpers_render_before_single_submission(self):
        here = Path(__file__).resolve().parent
        compiler = os.environ.get('CLANG_CL') or shutil.which('clang-cl')
        if not compiler:
            self.skipTest('requires clang-cl in a VS developer environment')
        source = (here.parent / 'rsx_live_draw.c').read_text(encoding='utf-8')
        def function(signature):
            a = source.index(signature)
            b = source.index('\n}', a) + 2
            return source[a:b] + '\n'
        helpers = ''.join(function(signature) for signature in [
            'static D3D12_CPU_DESCRIPTOR_HANDLE smp_cpu(u32 slot)',
            'static void ld_sampler_pages_reset(void)',
            'static void ld_sampler_pages_completed(void)',
            'static int ld_sampler_pages_advance(void)',
            'static D3D12_GPU_DESCRIPTOR_HANDLE sampler_table(',
            'static int sampler_table_needs_flush('])
        constants = '\n'.join(re.search(r'^#define ' + name + r' .+$', source, re.M).group()
                              for name in ['SMP_TABLE_SIZE', 'SMP_RING_TABLES'])
        with tempfile.TemporaryDirectory(prefix='ps3recomp-sampler-warp-') as directory:
            root = Path(directory)
            (root / 'sampler_pages_helpers_actual.inc').write_text(helpers, encoding='utf-8')
            (root / 'sampler_pages_constants_actual.inc').write_text(constants + '\n', encoding='utf-8')
            exe = root / 'test_sampler_pages.exe'
            command = [compiler, '/nologo', '/TC', '/W4', '/WX', '/O2',
                       '/I' + str(root), '/I' + str(here.parent),
                       str(here / 'test_sampler_pages_warp.c'), str(here.parent / 'rsx_pipeline_recipes.c'),
                       '/Fo' + str(root) + '/', '/Fe' + str(exe), '/link',
                       'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'd3dcompiler.lib']
            built = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(exe)], cwd=root, capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            print(ran.stdout)
            self.assertIn('actual-source sampler page WARP lifetime/rendering passed', ran.stdout)


if __name__ == '__main__':
    unittest.main(verbosity=2)
