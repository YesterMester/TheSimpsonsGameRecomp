#!/usr/bin/env python3
"""Write a release package's runtime config from the launcher's defaults.

usage: write_runtime_defaults.py <simpsons.toml> [linux|windows]

Linux packages keep the shader set in launcher/ui/native_shaders (older
updaters already copy that folder); Windows packages keep it in
native_shaders next to simpsons.exe, where the game finds it by itself.
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from launcher.launcher import LINUX_RUNTIME_DEFAULTS, RUNTIME_DEFAULTS, _fmt


def main():
    output = Path(sys.argv[1])
    platform = sys.argv[2] if len(sys.argv) > 2 else "linux"
    if platform == "linux":
        defaults = dict(LINUX_RUNTIME_DEFAULTS)
        defaults["aot_shader_path"] = "launcher/ui/native_shaders"
    elif platform == "windows":
        defaults = dict(RUNTIME_DEFAULTS)
    else:
        sys.exit(f"unknown platform {platform}")
    lines = ["# The Simpsons Game - runtime configuration (edit these via the launcher)"]
    for key, value in defaults.items():
        typ = "bool" if isinstance(value, bool) else "str" if isinstance(value, str) else "int"
        lines.append(f"{key} = {_fmt(value, typ)}")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
