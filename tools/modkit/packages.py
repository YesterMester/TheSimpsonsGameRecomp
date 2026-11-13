"""The CLI and launcher intentionally share one package implementation."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher"))
from mod_packages import *  # noqa: E402,F403
