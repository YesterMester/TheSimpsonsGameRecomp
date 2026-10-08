#!/usr/bin/env python3
"""Write the Linux package's runtime config from the launcher's defaults."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from launcher.launcher import LINUX_RUNTIME_DEFAULTS, _fmt


def main():
    output = Path(sys.argv[1])
    defaults = dict(LINUX_RUNTIME_DEFAULTS)
    defaults["aot_shader_path"] = "launcher/ui/native_shaders"
    lines = ["# The Simpsons Game - runtime configuration (edit these via the launcher)"]
    for key, value in defaults.items():
        typ = "bool" if isinstance(value, bool) else "str" if isinstance(value, str) else "int"
        lines.append(f"{key} = {_fmt(value, typ)}")
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
