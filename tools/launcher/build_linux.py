#!/usr/bin/env python3
"""Bundle the Linux launcher with its Python interpreter and native Qt window."""
import argparse
from importlib import metadata
from pathlib import Path
import shutil
import subprocess
import sys
import sysconfig
import tempfile

ROOT = Path(__file__).resolve().parents[2]
from build_modkit_entry import build as build_modkit_entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Directory for simpsons-launcher")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Build the Linux launcher on Linux")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    licenses = output / "licenses"
    shutil.copytree(Path(__file__).parent / "licenses", licenses, dirs_exist_ok=True)
    shutil.copy2(Path(sysconfig.get_path("stdlib")) / "LICENSE.txt", licenses / "Python.txt")
    installer = metadata.distribution("PyInstaller")
    installer_license = next(p for p in installer.files if p.name == "COPYING.txt")
    shutil.copy2(installer.locate_file(installer_license), licenses / "PyInstaller.txt")
    versions = [f"Python {sys.version.split()[0]}"]
    versions += [f"{name} {metadata.version(name)}"
                 for name in ("PySide6", "PySide6-Essentials", "PySide6-Addons", "shiboken6", "PyInstaller")]
    (licenses / "versions.txt").write_text("\n".join(versions) + "\n")
    with tempfile.TemporaryDirectory(prefix="simpsons-launcher-build-") as temporary:
        subprocess.run([
            sys.executable, "-m", "PyInstaller", "--noconfirm", "--onefile", "--noupx",
            "--name", "simpsons-launcher", "--distpath", str(output),
            "--workpath", str(Path(temporary) / "work"), "--specpath", temporary,
            "--add-data", str(ROOT / "launcher/ui") + ":ui", str(ROOT / "launcher/launcher.py"),
        ], check=True, cwd=ROOT)
    build_modkit_entry(output)


if __name__ == "__main__":
    main()
