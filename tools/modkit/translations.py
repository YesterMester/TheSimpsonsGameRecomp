"""The CLI, desktop editor and browser editor share one translation implementation."""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "launcher"))
from mod_translations import *  # noqa: E402,F403
