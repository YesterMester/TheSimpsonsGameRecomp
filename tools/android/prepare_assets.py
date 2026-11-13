#!/usr/bin/env python3
"""Stage the Android runtime config and shader set, without game data."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))


def stage_launcher(output):
    # Android imports and runs packages. Desktop authoring data is not shipped
    # in its launcher; remove stale copies when reusing a staging directory.
    output = Path(output)
    for name in ("modkit-templates.json", "mission-editor.js"):
        (output / name).unlink(missing_ok=True)
    # The desktop launcher's example mods and module copies are not used here.
    for name in ("bundled-mods", "modules"):
        shutil.rmtree(output / name, ignore_errors=True)
    shutil.copytree(ROOT / "launcher/ui", output, dirs_exist_ok=True,
                   ignore=shutil.ignore_patterns("native_shaders", "__pycache__", "modkit-templates.json",
                                                 "mission-editor.js", "bundled-mods", "modules"))


def main():
    from launcher.launcher import (LINUX_RUNTIME_DEFAULTS, SETTINGS_SCHEMA, DERIVED_KEYS,
                                  LOGO_MOVIES, EPISODE_NAMES, _fmt)
    subprocess.run([sys.executable, str(ROOT / "tools/modkit/generate_templates.py"), "--check"], check=True)
    output = Path(sys.argv[1]).resolve()
    # Private probe builds may have added a qualification label here. It is
    # not part of the product and must not survive a later normal APK build.
    (output / "native-test-build.json").unlink(missing_ok=True)
    shaders = output / "native_shaders"
    # Only reproducible staging output lives here. Retire desktop translations
    # left by an earlier APK build: the phone bakes its own from the current
    # runtime and the owner's executable, using the shared variant recipes.
    if shaders.exists():
        shutil.rmtree(shaders)
    shaders.mkdir(parents=True, exist_ok=True)
    # The shared builder stages warmup identities and recipes for every
    # platform after adding its replacement shader variants.
    subprocess.run([sys.executable,
                    str(ROOT / "tools/native-renderer/native_shaders/build_native_shaders.py"),
                    str(shaders), "0000400000000001", "--warmup-archive",
                    str(ROOT / "tools/native-renderer/translated_shaders.tar.xz")], check=True)
    # Keep a content index so UI-only APK updates do not recopy thousands of shaders.
    entries = {}
    for file in sorted(shaders.rglob("*")):
        if file.is_file():
            entries[file.relative_to(shaders).as_posix()] = {
                "size": file.stat().st_size, "sha256": hashlib.sha256(file.read_bytes()).hexdigest(),
            }
    fingerprint = hashlib.sha256(json.dumps(entries, sort_keys=True).encode()).hexdigest()
    (output / "native-shaders-index.json").write_text(json.dumps({
        "fingerprint": fingerprint, "bytes": sum(item["size"] for item in entries.values()), "files": entries,
    }), encoding="utf-8")
    defaults = dict(LINUX_RUNTIME_DEFAULTS)
    defaults.update(gpu="vulkan", aot_shader_path="native_shaders", resolution_scale=1,
                    audio_maxqframes=32, fullscreen=True, input_backend="sdl",
                    audio_log_underruns=True, native_telemetry=120)
    lines = ["# The Simpsons Game - Android runtime configuration"]
    for key, value in defaults.items():
        typ = "bool" if isinstance(value, bool) else "str" if isinstance(value, str) else "int"
        lines.append(f"{key} = {_fmt(value, typ)}")
    (output / "simpsons.toml").write_text("\n".join(lines) + "\n", encoding="utf-8")
    values = {key: spec[1] for key, spec in SETTINGS_SCHEMA.items()}
    values.update(gpu="vulkan", fullscreen=True, resolution_scale=1)
    values.update(touch_size=1.0, touch_opacity=0.5, floating_stick=True,
                  touch_haptics=True, hide_with_controller=True, touch_deadzone=0.12,
                  touch_camera_sensitivity=1.0, touch_camera_invert_y=False, touch_show_editor=True,
                  touch_scheme="simple")
    (output / "launcher-settings.json").write_text(json.dumps({
        "values": values, "defaults": values, "runtime_defaults": defaults,
        "types": {key: spec[0] for key, spec in SETTINGS_SCHEMA.items()},
        "derived_keys": list(DERIVED_KEYS),
    }), encoding="utf-8")
    stage_launcher(output / "launcher")
    shutil.copy2(ROOT / "android/touch-controls.json", output / "touch-controls.json")
    (output / "game-patches.json").write_text(json.dumps({
        "logo_movies": LOGO_MOVIES, "episode_names": EPISODE_NAMES,
    }), encoding="utf-8")


if __name__ == "__main__":
    main()
