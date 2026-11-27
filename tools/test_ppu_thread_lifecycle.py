#!/usr/bin/env python3
"""PPU thread-table lifetimes against the real sys_ppu_thread.c, on Windows and POSIX.

Builds runtime/syscalls/tests/test_ppu_thread_lifecycle.c (which compiles in
runtime/syscalls/sys_ppu_thread.c) and runs it twice: with the default
sys_ppu_thread_exit unwind, and with PS3_NO_THREAD_EXIT_UNWIND=1. Each run
creates thousands of detached and joinable lifetimes (well past the 64-slot
table), races concurrent creators against exiting threads, and checks join /
detach errors, exit status, stack reuse and the full-table CELL_EAGAIN.
"""
import os
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'runtime/syscalls/tests/test_ppu_thread_lifecycle.c'
windows = os.name == 'nt'
compiler = os.environ.get('CC', 'clang-cl' if windows else 'cc')
msvc_style = Path(compiler).stem.lower() in ('clang-cl', 'cl')
if not shutil.which(compiler):
    raise SystemExit(f'compiler {compiler!r} not found')
includes = [ROOT / 'include', ROOT / 'runtime', ROOT / 'runtime/syscalls']

with tempfile.TemporaryDirectory(prefix='ppu-thread-lifecycle-') as tmp:
    binary = Path(tmp) / ('test.exe' if windows else 'test')
    if msvc_style:
        command = [compiler, '/nologo', '/std:c17', '/O2', '/D_CRT_SECURE_NO_WARNINGS',
                   *('/I' + str(i) for i in includes), str(SOURCE), '/Fe' + str(binary)]
    else:
        command = [compiler, '-std=gnu17', '-O2', '-UNDEBUG', *('-I' + str(i) for i in includes),
                   str(SOURCE), *([] if windows else ['-pthread']), '-o', str(binary)]
    subprocess.run(command, check=True, timeout=180, cwd=tmp)
    for args, extra_env in (([], {}), (['nounwind'], {'PS3_NO_THREAD_EXIT_UNWIND': '1'})):
        env = {k: v for k, v in os.environ.items()
               if k not in ('PS3_NO_THREAD_EXIT_UNWIND', 'PS3_PPU_THREAD_FORCE_JOINABLE')}
        env.update(extra_env)
        run = subprocess.run([str(binary), *args], cwd=tmp, env=env, timeout=300,
                             capture_output=True, text=True)
        print(run.stdout.strip(), flush=True)
        if run.returncode != 0:
            failures = [line for line in run.stderr.splitlines() if line.startswith('FAIL')]
            print('\n'.join(failures) or run.stderr[-2000:])
            raise SystemExit(f'test_ppu_thread_lifecycle {" ".join(args) or "default"} '
                             f'failed (exit {run.returncode:#x})')
print('PASS')
