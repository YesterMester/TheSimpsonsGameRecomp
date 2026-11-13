#!/usr/bin/env python3
"""Build the small separate Modkit entry beside the shared desktop runtime."""
import argparse
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]


def windows_resources(output, compiler):
    from PySide6.QtCore import QByteArray, QBuffer, QIODevice
    from PySide6.QtGui import QImage, QPainter
    from PySide6.QtSvg import QSvgRenderer
    image = QImage(128, 128, QImage.Format_ARGB32); image.fill(0)
    painter = QPainter(image)
    QSvgRenderer(str(ROOT / 'launcher/ui/modkit-icon.svg')).render(painter); painter.end()
    data = QByteArray(); buffer = QBuffer(data); buffer.open(QIODevice.WriteOnly)
    if not image.save(buffer, 'PNG'):
        raise RuntimeError('Could not render the Modkit icon')
    png = bytes(data)
    (output / 'modkit.ico').write_bytes(struct.pack('<HHHBBBBHHII', 0, 1, 1, 128, 128, 0, 0, 1, 32, len(png), 22) + png)
    version = '0.0.0.0'
    for line in (ROOT / 'launcher/launcher.py').read_text().splitlines():
        if line.startswith('VERSION = '):
            version = line.split('"')[1]; break
    import re
    numbers = (re.findall(r'\d+', version) + ['0'] * 4)[:4]
    numeric = ','.join(str(min(65535, int(value))) for value in numbers)
    text = f'''1 ICON "modkit.ico"
1 VERSIONINFO
 FILEVERSION {numeric}
 PRODUCTVERSION {numeric}
 FILEFLAGSMASK 0x3f
 FILEFLAGS 0
 FILEOS 0x40004
 FILETYPE 1
 FILESUBTYPE 0
BEGIN
 BLOCK "StringFileInfo"
 BEGIN
  BLOCK "040904b0"
  BEGIN
   VALUE "CompanyName", "YesterMester\\0"
   VALUE "FileDescription", "The Simpsons Game Modkit\\0"
   VALUE "FileVersion", "{version}\\0"
   VALUE "InternalName", "simpsons-modkit\\0"
   VALUE "OriginalFilename", "simpsons-modkit.exe\\0"
   VALUE "ProductName", "The Simpsons Game Modkit\\0"
  END
 END
 BLOCK "VarFileInfo"
 BEGIN
  VALUE "Translation", 0x409, 1200
 END
END
'''
    script = output / 'modkit-entry.rc'; script.write_text(text, encoding='utf-8')
    sibling = Path(compiler).parent
    rc = shutil.which('llvm-rc') or shutil.which('rc')
    if not rc:
        candidate = sibling / ('llvm-rc.exe' if sys.platform == 'win32' else 'llvm-rc')
        rc = str(candidate) if candidate.is_file() else None
    if not rc:
        raise RuntimeError('The Windows entry needs llvm-rc or the Windows SDK resource compiler')
    result = output / 'modkit-entry.res'
    subprocess.run([rc, '/FO', result.name, script.name], cwd=output, check=True)
    return result


def build(output, compiler=None, windows=None):
    windows = sys.platform == 'win32' if windows is None else windows
    output = Path(output).resolve(); output.mkdir(parents=True, exist_ok=True)
    source = ROOT / 'tools/launcher/modkit_entry.c'
    destination = output / ('simpsons-modkit.exe' if windows else 'simpsons-modkit')
    cc = shlex.split(compiler or os.environ.get('CC', '')) or [
        (shutil.which('cl') or shutil.which('clang-cl') or 'cl') if windows else
        (shutil.which('cc') or shutil.which('clang') or 'cc')]
    resource = windows_resources(output, cc[0]) if windows else None
    if Path(cc[0]).name.lower() in ('cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'):
        args = cc + ['/nologo', '/O2', '/W4', str(source), str(resource), '/Fe:' + str(destination),
                     '/Fo:' + str(output / 'modkit-entry.obj'), '/link', '/SUBSYSTEM:WINDOWS', 'user32.lib']
    else:
        args = cc + ['-std=c11', '-O2', '-Wall', '-Wextra', str(source), '-o', str(destination)]
        if windows:
            args += [str(resource), '-municode', '-mwindows', '-luser32']
    subprocess.run(args, check=True)
    if not windows:
        destination.chmod(0o755)
    return destination


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path); parser.add_argument('--cc'); parser.add_argument('--windows', action='store_true')
    args = parser.parse_args(); build(args.output, args.cc, args.windows or sys.platform == 'win32')
