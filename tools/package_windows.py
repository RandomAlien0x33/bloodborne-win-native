#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Release package for Windows: dist/bloodborne-win-native-<version>.zip.

Run in the MSYS2 CLANG64 environment after build.sh:
    python tools/package_windows.py --version v1.01 --python-embed python-3.14.8-embed-amd64.zip

The package runs without MSYS2: out\\ holds bb-probe.exe, the GPU check and every MSYS2 DLL they
load (found with ldd), python\\ the official embeddable Python (python.org) for the launcher
scripts, and run.bat picks that Python. NVIDIA's nvngx_dlss.dll goes in when out\\ has it (DLSS
SDK license: redistributable with an application; its license file goes along). No game files.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parent.parent
EXECUTABLES = ('bb-probe.exe', 'bb-gpu-capabilities.exe')

LAUNCHER = r'''@echo off
setlocal
cd /d "%~dp0"
set "BB_GPU_USER_DIR=%~dp0user\gpu"
if not exist "%BB_GPU_USER_DIR%" mkdir "%BB_GPU_USER_DIR%"
set "BB_PREBUILT=1"
set "BB_PRODUCER_CHECK=1"
if not defined BB_DLSS_MODEL set "BB_DLSS_MODEL=cnn"
if exist "%~dp0out\last-run.log" move /y "%~dp0out\last-run.log" "%~dp0out\prev-run.log" >nul
call "%~dp0run.bat" %* 2>&1 | "%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -Command "$w = New-Object IO.StreamWriter('%~dp0out\last-run.log', $false, (New-Object Text.UTF8Encoding($false))); $w.AutoFlush = $true; foreach ($line in $input) { $line; $w.WriteLine($line) }; $w.Close()"
findstr /c:"Host fault" /c:"STOP:" "%~dp0out\last-run.log" >nul && (
    echo.
    echo The game crashed. The log is in %~dp0out\last-run.log
    pause
)
'''

THIRD_PARTY = '''Bloodborne Win Native - third-party components in this package

Source code of the port: https://github.com/RandomAlien0x33/bloodborne-win-native
(GPL-2.0-or-later, see LICENSE).

out\\*.dll (except nvngx_dlss.dll): MSYS2 CLANG64 packages (SDL3, FFmpeg and its codecs, libc++,
  and their dependencies), each under its own license (zlib, LGPL/GPL, Apache-2.0 with LLVM
  exception, MIT, BSD, ...). Sources and license texts: https://packages.msys2.org and
  https://github.com/msys2/MINGW-packages
out\\nvngx_dlss.dll: NVIDIA DLSS, NVIDIA DLSS SDK license (out\\LICENSE-DLSS-SDK.txt).
python\\: the official Python embeddable package (python.org), PSF license (python\\LICENSE.txt).
'''


def dll_closure(executables):
    """MSYS2 DLLs the executables load, recursively (ldd lists the whole closure)."""
    found = set()
    for exe in executables:
        result = subprocess.run(['ldd', str(exe)], capture_output=True, text=True, check=True)
        for line in result.stdout.splitlines():
            parts = line.split('=>')
            if len(parts) == 2 and '/clang64/' in parts[1].lower():
                found.add(parts[1].split('(')[0].strip())
    paths = []
    for unix in sorted(found):
        win = subprocess.run(['cygpath', '-w', unix], capture_output=True, text=True, check=True).stdout.strip()
        paths.append(Path(win))
    return paths


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--version', required=True)
    p.add_argument('--python-embed', required=True, type=Path, help='python-3.x-embed-amd64.zip from python.org')
    a = p.parse_args()
    name = f'bloodborne-win-native-{a.version}'
    dist = ROOT / 'dist'
    stage = dist / name
    if stage.exists():
        shutil.rmtree(stage)
    (stage / 'out').mkdir(parents=True)
    out = ROOT / 'out'
    executables = [out / e for e in EXECUTABLES]
    for exe in executables:
        shutil.copy2(exe, stage / 'out')
    dlls = dll_closure(executables)
    for dll in dlls:
        shutil.copy2(dll, stage / 'out')
    for extra in ('nvngx_dlss.dll', 'LICENSE-DLSS-SDK.txt'):
        if (out / extra).is_file():
            shutil.copy2(out / extra, stage / 'out')
    for folder in ('scripts', 'patches'):
        tracked = subprocess.run(['git', 'ls-files', folder], cwd=ROOT, capture_output=True, text=True,
                                 check=True).stdout.split()
        for f in tracked:
            target = stage / f
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / f, target)
    for f in ('LICENSE', 'README.md', 'README.ru.md'):
        shutil.copy2(ROOT / f, stage / f)
    (stage / 'run.bat').write_bytes(crlf((ROOT / 'run.bat').read_text(encoding='utf-8')).encode())
    (stage / 'Bloodborne.cmd').write_bytes(crlf(LAUNCHER).encode())
    # The settings the port is played with (bbport.default.ini): the defaults in the code render
    # the scene at full output size (Native AA), much slower.
    (stage / 'bbport.ini').write_bytes(crlf((ROOT / 'bbport.default.ini').read_text(encoding='utf-8')).encode())
    (stage / 'THIRD_PARTY.txt').write_bytes(crlf(THIRD_PARTY).encode())
    with zipfile.ZipFile(a.python_embed) as z:
        z.extractall(stage / 'python')
    # The embeddable Python's path is its ._pth file only: the launcher scripts import each other.
    pth = next((stage / 'python').glob('python*._pth'))
    pth.write_text(pth.read_text().rstrip('\n') + '\n..\\scripts\n')
    archive = dist / f'{name}.zip'
    if archive.exists():
        archive.unlink()
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for f in sorted(stage.rglob('*')):
            if f.is_file():
                z.write(f, Path(name) / f.relative_to(stage))
    print(f'{archive}: {len(dlls)} DLLs, {archive.stat().st_size >> 20} MiB')


if __name__ == '__main__':
    main()
