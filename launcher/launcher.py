#!/usr/bin/env python3
"""
The Simpsons Game — Recompiled : Launcher  (v3)

- Native desktop app (PySide6/Qt WebEngine) with browser fallback.
- Install from the user's own legally-owned ISO (extract-xiso).
- Launcher art generated from the user's own game files.
- Deep settings: display, FPS, anti-aliasing, input (keyboard/mouse!),
  language, audio — written to a launcher-owned block in simpsons.toml.
- Patches tab (skip intro videos; more to come).
- Save-data backup/restore, Add to Steam, GitHub update checks.
- Diagnostics: one-click session logging (crashes, black screens,
  performance samples) + support bundles for bug reports.
"""

import hashlib
import http.server
import json
import os
import platform
import re
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib
from pathlib import Path

# Dev-tree fallback only: release.yml stamps the real release tag over this
# at build time, so packaged builds always know exactly which release they
# are (otherwise every launcher shipped inside vX.Y.Z.W would compare itself
# against its own release and nag "update available" forever).
VERSION = "0.0.6.4"

FROZEN = getattr(sys, "frozen", False)
if FROZEN:
    # Packaged .exe: the launcher lives in a flat release directory next to
    # simpsons.exe / extract-xiso.exe / gamedata/. The web UI is bundled and
    # unpacked to PyInstaller's temp dir (sys._MEIPASS).
    LAUNCHER_DIR = Path(sys.executable).resolve().parent
    ROOT = LAUNCHER_DIR
    UI_DIR = Path(getattr(sys, "_MEIPASS", LAUNCHER_DIR)) / "ui"
else:
    LAUNCHER_DIR = Path(__file__).resolve().parent
    ROOT = LAUNCHER_DIR.parent
    UI_DIR = LAUNCHER_DIR / "ui"
ART_DIR = LAUNCHER_DIR / "art"
MODS_DIR = LAUNCHER_DIR / "mods"
BACKUPS_DIR = LAUNCHER_DIR / "backups"
DIAG_DIR = LAUNCHER_DIR / "diagnostics"
CONFIG_JSON = LAUNCHER_DIR / "launcher.json"

DEFAULT_CONFIG = {
    "github_repo": "YesterMester/TheSimpsonsGameRecomp",
    "diagnostics_enabled": False,
    # Launcher colour theme: "system", "light" or "dark".
    "ui_theme": "system",
    "engine": {
        "Linux": "simpsons/out/build/linux-amd64-relwithdebinfo/simpsons",
        "Windows": "simpsons/out/build/win-amd64-relwithdebinfo/simpsons.exe",
    },
    "extract_xiso": {
        "Linux": "tools/extract-xiso/build/extract-xiso",
        "Windows": "tools/extract-xiso/build/extract-xiso.exe",
    },
    "lib_dirs": {
        "Linux": ["tools/rexglue-bin/linux-amd64/lib",
                  "/home/.steamos/offload/nix/store/ah4525ca553drv47jhvgpl9sl87i7a1d-libxml2-2.13.8/lib"],
        "Windows": [],
    },
}


def load_config():
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))
    if CONFIG_JSON.exists():
        try:
            user = json.loads(CONFIG_JSON.read_text(encoding="utf-8"))
            for k, v in user.items():
                if isinstance(v, dict) and k in cfg:
                    cfg[k].update(v)
                else:
                    cfg[k] = v
        except Exception:
            pass
    else:
        CONFIG_JSON.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    return cfg


def save_config():
    try:
        CONFIG_JSON.write_text(json.dumps(CONFIG, indent=2) + "\n", encoding="utf-8")
    except Exception:
        pass


CONFIG = load_config()
PLAT = platform.system()

# The packaged launcher is a windowed exe on Windows, so every subprocess it
# spawns pops a visible console unless told not to. Artwork generation alone
# can spawn hundreds (one ffmpeg per candidate frame) - players saw a storm
# of cmd windows at every boot. Pass this to every subprocess call whose
# output we capture.
POPEN_NO_WINDOW = ({"creationflags": subprocess.CREATE_NO_WINDOW}
                   if PLAT == "Windows" else {})


def _resolve(p):
    p = Path(p)
    return p if p.is_absolute() else ROOT / p


if FROZEN:
    # Flat release layout: game binary and tools sit beside the launcher.
    GAME_BIN = ROOT / ("simpsons.exe" if PLAT == "Windows" else "simpsons")
    EXTRACT_XISO = ROOT / ("extract-xiso.exe" if PLAT == "Windows" else "extract-xiso")
else:
    GAME_BIN = _resolve(CONFIG["engine"].get(PLAT, CONFIG["engine"]["Linux"]))
    EXTRACT_XISO = _resolve(CONFIG["extract_xiso"].get(PLAT, CONFIG["extract_xiso"]["Linux"]))
BUILD_DIR = GAME_BIN.parent
GAME_TOML = BUILD_DIR / "simpsons.toml"
GAMEDATA = ROOT / "gamedata"


def _user_data_dir():
    """Where the engine keeps saves and its cache — must match ReXApp::SetupEnvironment.

    Windows uses local app data (the engine moved off Documents, which is often
    OneDrive-redirected); everything else uses the XDG data home. This was
    hardcoded to the Linux path, so every save feature here silently did
    nothing on Windows.
    """
    if PLAT == "Windows":
        base = os.environ.get("LOCALAPPDATA")
        if base:
            return Path(base) / "simpsons"
        return Path.home() / "AppData" / "Local" / "simpsons"
    xdg = os.environ.get("XDG_DATA_HOME")
    # Ignore XDG_DATA_HOME when it points inside a Flatpak sandbox. Launching
    # from a terminal owned by a Flatpak app (VS Code, for instance) exports
    # that app's private data dir to children, which silently sends saves to
    # ~/.var/app/<app>/data/simpsons instead of the real location -- so the
    # same install shows a different set of saves depending on what happened
    # to spawn it, which reads to the player as "my save vanished".
    if xdg and "/.var/app/" not in str(xdg):
        base = Path(xdg)
    else:
        base = Path.home() / ".local" / "share"
    return base / "simpsons"


USER_DATA = _user_data_dir()

TOKEN = secrets.token_hex(16)
PORT = 8712

SETTINGS_BEGIN = "# >>> LAUNCHER SETTINGS (managed block - do not edit by hand) >>>"
SETTINGS_END = "# <<< LAUNCHER SETTINGS <<<"

# Keys the launcher derives from the schema values rather than exposing
# directly. The Vulkan presenter picks its present mode independently of the
# vsync flag (which only paces the guest's vblank), so without these a driver
# with no mailbox support silently falls back to immediate (tearing) - the
# original main-menu flicker - no matter what vsync says. With vsync on, the
# tearing-permitted modes are disallowed so the fallback chain lands on fifo.
DERIVED_KEYS = ("vulkan_allow_present_mode_immediate",
                "vulkan_allow_present_mode_fifo_relaxed")

# Bumped whenever a shipped default changes; written as a TOML comment so the
# engine's config parser ignores it. Each migration entry drops a stored value
# once, but only if it still equals the default it is superseding - a player
# who picked that value on purpose keeps it.
# Stage-1 measurement capture. These engine cvars are diagnostic-only, so they
# are not part of SETTINGS_SCHEMA (they must not persist as user settings) --
# they are written into the managed block only while a capture is armed, and
# removed again afterwards. The engine reads them at startup, so arming takes
# effect on the next launch.
CAPTURE_MARKER = LAUNCHER_DIR / "capture-armed"
CAPTURE_DIR = DIAG_DIR / "capture"


def capture_armed():
    return CAPTURE_MARKER.exists()


def capture_keys():
    """Engine cvars for a capture run, or {} when not armed."""
    if not capture_armed():
        return {}
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    return {
        "draw_telemetry": "true",
        "perf_log_csv": f'"{(CAPTURE_DIR / "perf.csv").as_posix()}"',
        "shader_inventory_csv": f'"{(CAPTURE_DIR / "shader_inventory.csv").as_posix()}"',
        "pipeline_inventory_json": f'"{(CAPTURE_DIR / "pipeline_inventory.jsonl").as_posix()}"',
        # Frame traces (F10 in-game) land in the capture folder too.
        "trace_gpu_prefix": f'"{(CAPTURE_DIR / "trace").as_posix()}"',
    }


def set_capture(enable):
    if enable:
        CAPTURE_MARKER.touch()
    else:
        CAPTURE_MARKER.unlink(missing_ok=True)
    write_settings({})  # rewrite the managed block with/without the capture keys
    if enable:
        return True, ("Capture armed. Launch the game, play the scene you care about, then "
                      "quit normally - do not force-kill it, the CSV is finalised on exit. "
                      "Then use 'Create support bundle' to collect the results.")
    return True, "Capture disarmed."


SETTINGS_VERSION = 3
SETTINGS_VERSION_MARKER = "# settings_version ="
SETTINGS_DEFAULT_MIGRATIONS = (
    # The v1 migration moved everyone from "none" to the then-new FXAA default.
    # FXAA turned out to render a black screen on the Steam Deck (RADV), so v2
    # walks anyone still on the stamped default back to "none". A player who
    # picked fxaa_extreme on purpose keeps it.
    (2, "swap_post_effect", "fxaa"),
    # v3 made the native renderer the default. Configs from before it pin the
    # old "fsi" hand patch; move them over once, and the Renderer setting can
    # still pick "fsi" deliberately afterwards.
    (3, "render_target_path_vulkan", "fsi"),
)

# key -> (type, default, needs_restart)
SETTINGS_SCHEMA = {
    # display
    "fullscreen": ("bool", False, True),
    "resolution": ("str", "", True),               # "", 720p, 1080p, 1440p, 4k
    "window_width": ("int", 0, True),
    "window_height": ("int", 0, True),
    # vsync also picks the present-mode fallback (see DERIVED_KEYS), which is
    # chosen once at swapchain creation - hence the restart flag.
    "vsync": ("bool", True, True),
    "present_letterbox": ("bool", True, False),
    # quality
    "resolution_scale": ("int", 1, True),
    # Shadow maps at the original resolution while the rest renders at 2x / 3x
    # ("" = match the render resolution, "832:13" = the game's shadow map
    # render target). Almost the same look, about 0.7 ms faster per frame at
    # 2x on the Steam Deck.
    "native_rt_original_resolution_targets": ("str", "", True),
    "anisotropic_override": ("int", 3, False),
    # How the guest image is scaled to the window: bilinear, AMD FidelityFX
    # FSR 1 (spatial upscaling with sharpening) or CAS (sharpening only).
    "present_effect": ("str", "bilinear", True),
    "present_fsr_sharpness_reduction": ("float", 0.2, True),   # stops, 0 = sharpest
    # FXAA works on every backend since 0.0.6.0 (it used to show a black
    # screen with Vulkan on the Steam Deck); new installs start with it.
    "swap_post_effect": ("str", "fxaa", True),     # none, fxaa, fxaa_extreme
    # The black ink outlines (simpsons/src/ink_outlines.cpp): original, soft or
    # off; the soft lines' darkness; and their colour as RRGGBB (000000 =
    # black, the original). Patched when the game loads, hence the restart.
    "ink_outlines": ("str", "original", True),
    "ink_outline_strength": ("float", 0.6, True),
    "ink_outline_color": ("str", "000000", True),
    # The characters' eyes (simpsons/src/eye_shading.cpp): "clean" reads the
    # artists' no-rim-shadow flag with a tolerance, "original" keeps the
    # speckled shadow of the Xbox 360 game. Patched when the game loads.
    "eye_shading": ("str", "clean", True),
    # fps
    "video_mode_refresh_rate": ("float", 60.0, True),
    # input
    "mnk_mode": ("bool", False, True),
    "mnk_sensitivity": ("float", 1.0, False),
    "mnk_invert_y": ("bool", False, False),
    # Keyboard / mouse bindings: key names separated by commas (the runtime's
    # names - see KEYBIND_DEFAULTS). Written only when changed, so new defaults
    # still reach everyone else.
    **{k: ("str", d, False) for k, d in {
        "keybind_lstick_up": "W",
        "keybind_lstick_down": "S",
        "keybind_lstick_left": "A",
        "keybind_lstick_right": "D",
        "keybind_walk": "Control",
        "keybind_a": "Space,Enter",
        "keybind_x": "LMB",
        "keybind_b": "RMB,Backspace",
        "keybind_y": "E",
        "keybind_left_trigger": "Shift",
        "keybind_right_trigger": "F",
        "keybind_left_shoulder": "Q",
        "keybind_right_shoulder": "R",
        "keybind_dpad_up": "1,Up",
        "keybind_dpad_right": "2,Right",
        "keybind_dpad_down": "3,Down",
        "keybind_dpad_left": "4,Left",
        "keybind_back": "Tab",
        "keybind_start": "Escape",
        "keybind_lstick_press": "C",
        "keybind_rstick_press": "MMB",
        # Camera on keys (#33), unbound unless chosen.
        "keybind_rstick_up": "",
        "keybind_rstick_down": "",
        "keybind_rstick_left": "",
        "keybind_rstick_right": "",
    }.items()},
    # Keys that open the in-game overlays (one key each). The runtime keeps
    # these until the overlays register, so a saved key survives a restart.
    **{k: ("str", d, False) for k, d in {
        "bind_controls": "F1",
        "bind_settings": "F4",
        "bind_debug_overlay": "F3",
        "bind_console": "Backtick",
        "bind_freecam": "F6",
        "bind_photo_mode": "F8",
    }.items()},
    # game
    "user_language": ("int", 1, True),
    # Always show subtitles, even in a new game's first cutscene (the engine
    # then overrides the in-game option, so it starts off).
    "subtitles": ("bool", False, True),
    # graphics backend: "" = automatic (Vulkan first, D3D12 fallback on
    # Windows), "vulkan", "d3d12" or the experimental "d3d11" (Windows) to
    # force one. Chosen at startup.
    "gpu": ("str", "", True),
    "vulkan_device": ("str", "", True),
    # renderer: "native" = GPU render targets; "fsi" = Xbox EDRAM emulated in
    # the pixel shader (accurate fallback). Vulkan only; chosen at startup.
    "render_target_path_vulkan": ("str", "native", True),
    # audio
    "audio_mute": ("bool", False, False),
    "audio_maxqframes": ("int", 32, True),
}

# Kept in the launcher so updates from older launchers, which preserve
# simpsons.toml and do not copy newly added files, receive these defaults too.
LINUX_RUNTIME_DEFAULTS = {
    "gpu_allow_invalid_fetch_constants": False,
    "gpu_shader_max_cf_iterations": 0,
    "native_index_buffers": True,
    "native_vertex_buffers": True,
    "native_texture_uploads": True,
    "native_vertex_buffer_cache": True,
    "native_vertex_cache_refresh": True,
    "native_index_buffer_cache": True,
    "native_index_bounds_cache": True,
    "pm4_bulk_state_registers": True,
    "native_rt_image_copies": True,
    "native_resolve_image_copies": True,
    "native_resolve_buffers": True,
    "native_resolve_buffer_reads": True,
    "native_resolve_buffer_lazy_memory": True,
    "native_resolve_buffer_reuse": True,
    "native_resolve_buffer_texture_first": True,
    "timer_queue_sleep": True,
    "native_buffer_write_watches": False,
    "native_vertex_cache_check_order": False,
    "native_resolve_copy_free": False,
    "frame_pacing_vblank_lock": False,
}

# Settings left out of the config while at their default, so the runtime's own
# default applies (and a later change of it reaches the player).
OMIT_WHEN_DEFAULT = ({k for k in SETTINGS_SCHEMA if k.startswith(("keybind_", "bind_"))}
                     | {"mnk_invert_y"})

LOGO_MOVIES = ("ealogo", "ealogo_sd", "foxlogo", "foxlogo_sd",
               "gracielogo", "gracielogo_sd")

game_proc_lock = threading.Lock()
game_proc = None
install_state = {"running": False, "log": [], "ok": None}
update_state = {"checked": False, "msg": "", "update_available": False, "download_url": None,
                "latest_tag": "", "applying": False, "apply_msg": ""}


# ----------------------------------------------------------------- settings

def _fmt(v, typ):
    if typ == "bool":
        return "true" if v else "false"
    if typ == "str":
        return f'"{v}"'
    return str(v)


def _parse(raw, typ):
    raw = raw.strip()
    if typ == "bool":
        return raw.lower() == "true"
    if typ == "str":
        return raw.strip('"')
    if typ == "float":
        return float(raw)
    return int(raw)


def read_settings():
    values = {k: v[1] for k, v in SETTINGS_SCHEMA.items()}
    if not GAME_TOML.exists():
        return values
    stored = {}
    file_version = 0
    for line in GAME_TOML.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        if s.startswith(SETTINGS_VERSION_MARKER):
            try:
                file_version = int(s[len(SETTINGS_VERSION_MARKER):].strip())
            except ValueError:
                pass
            continue
        if "=" in s and not s.startswith("#"):
            key, _, raw = s.partition("=")
            key = key.strip()
            if key in SETTINGS_SCHEMA:
                try:
                    stored[key] = _parse(raw, SETTINGS_SCHEMA[key][0])
                except ValueError:
                    pass
    # Settings written before a default changed are pinned to the old value,
    # so a new default would never reach anyone who has already run the
    # launcher. Drop only the specific stale keys, once, rather than resetting
    # anything the player deliberately chose.
    for version, key, superseded_default in SETTINGS_DEFAULT_MIGRATIONS:
        if file_version < version and stored.get(key) == superseded_default:
            stored.pop(key, None)
    values.update(stored)
    return values


def write_settings(new_values):
    values = read_settings()
    for k, v in new_values.items():
        if k not in SETTINGS_SCHEMA:
            continue
        typ = SETTINGS_SCHEMA[k][0]
        values[k] = (bool(v) if typ == "bool" else str(v) if typ == "str"
                     else float(v) if typ == "float" else int(v))
    lines = []
    if GAME_TOML.exists():
        in_block = False
        for line in GAME_TOML.read_text(encoding="utf-8").splitlines():
            s = line.strip()
            if s == SETTINGS_BEGIN:
                in_block = True
                continue
            if s == SETTINGS_END:
                in_block = False
                continue
            if in_block:
                continue
            key = s.partition("=")[0].strip()
            if "=" in s and not s.startswith("#") and (
                    key in SETTINGS_SCHEMA or key in DERIVED_KEYS
                    or key in ("draw_telemetry", "perf_log_csv", "shader_inventory_csv",
                               "pipeline_inventory_json", "trace_gpu_prefix")):
                continue
            lines.append(line)
        while lines and not lines[-1].strip():
            lines.pop()
    # Updates keep the player's config. Add the Linux runtime defaults only
    # where no value was saved, so the new resource paths reach existing
    # installs too, without replacing deliberate overrides.
    if PLAT == "Linux":
        present = {line.partition("=")[0].strip() for line in lines
                   if "=" in line and not line.lstrip().startswith("#")}
        defaults = dict(LINUX_RUNTIME_DEFAULTS)
        # Older Linux updaters already replace launcher/ui. Put the packaged
        # shader assets there so their first update also receives the new set.
        shaders = LAUNCHER_DIR / "ui" / "native_shaders"
        if shaders.is_dir():
            defaults["aot_shader_path"] = str(shaders)
        missing = [f"{k} = {_fmt(v, 'bool' if isinstance(v, bool) else 'str' if isinstance(v, str) else 'int')}"
                   for k, v in defaults.items() if k not in present]
        if missing:
            lines.extend(["", "# Linux runtime defaults", *missing])
    block = [SETTINGS_BEGIN]
    for k, (typ, default, _r) in SETTINGS_SCHEMA.items():
        if k in OMIT_WHEN_DEFAULT and values[k] == default:
            continue
        block.append(f"{k} = {_fmt(values[k], typ)}")
    allow_tearing = not values["vsync"]
    for k in DERIVED_KEYS:
        block.append(f"{k} = {_fmt(allow_tearing, 'bool')}")
    for k, v in capture_keys().items():
        block.append(f"{k} = {v}")
    block.append(f"{SETTINGS_VERSION_MARKER} {SETTINGS_VERSION}")
    block.append(SETTINGS_END)
    GAME_TOML.write_text("\n".join(lines + ["", *block]) + "\n", encoding="utf-8")
    return values


# ------------------------------------------------------------------ patches

patch_lock = threading.Lock()

def _logo_movie_files(enabled):
    """The logo movies on disk, in every language folder under movies/ (en on
    English discs, de, fr, ... on others), matched case-insensitively like
    the guest filesystem: their .vp6 files, or the .vp6.disabled ones."""
    movies = _find_ci(GAMEDATA, "movies", folder=True)
    if movies is None or not movies.is_dir():
        return []
    suffix = ".vp6" if enabled else ".vp6.disabled"
    names = {m + suffix for m in LOGO_MOVIES}
    return [f for d in sorted(movies.iterdir()) if d.is_dir()
            for f in sorted(d.iterdir()) if f.is_file() and f.name.lower() in names]


def patch_skip_intro_state():
    present = _logo_movie_files(True)
    disabled = _logo_movie_files(False)
    if not present and not disabled:
        return "unavailable"
    return "on" if disabled and not present else "off"


def patch_skip_intro(enable):
    with patch_lock:
        files = _logo_movie_files(enable)
        if not files and not _logo_movie_files(not enable):
            return False, "logo movies not installed"
        moves = [(f, f.with_name(f.name + ".disabled") if enable
                     else f.with_name(f.name[:-len(".disabled")])) for f in files]
        # Never replace another copy when both forms exist, including on
        # case-sensitive Linux filesystems with case-insensitive game paths.
        for src, dst in moves:
            if _find_ci(dst.parent, dst.name) is not None:
                return False, f"both enabled and disabled copies of {src.name} exist"
        completed = []
        try:
            for src, dst in moves:
                src.rename(dst)
                completed.append((src, dst))
        except OSError as e:
            for src, dst in reversed(completed):
                dst.rename(src)
            return False, str(e)
        return True, f"{'skipped' if enable else 'restored'} {len(files)} intro videos"


# Start episode (level select): the game starts a new game in the episode whose
# block in gamedata/simpsons_gameflow.lua calls episode:SetDefault() (Land of
# Chocolate). Moving that call makes new games start elsewhere; games in
# progress keep their own progress. The untouched file is kept beside it as
# simpsons_gameflow.lua.original while another episode is chosen.
GAMEFLOW_LUA = "simpsons_gameflow.lua"
EPISODE_NAMES = {
    "SPR_HUB": "Springfield (hub)",
    "LAND_OF_CHOCOLATE": "Land of Chocolate (original start)",
    "BARTMAN_BEGINS": "Bartman Begins",
    "EIGHTY_BITES": "Around the World in 80 Bites",
    "TREEHUGGER": "Lisa the Tree Hugger",
    "MOB_RULES": "Mob Rules",
    "CHEATER": "Enter the Cheatrix",
    "DOLPHINS": "Day of the Dolphins",
    "COLOSSAL_DONUT": "The Colossal Donut",
    "SPRINGFIELD_STOOD_STILL": "The Day the Earth Stood Stupid",
    "BARGAIN_BIN": "Bargain Bin",
    "GAME_HUB": "Video game world (hub)",
    "NEVERQUEST": "NeverQuest",
    "GRAND_THEFT_SCRATCHY": "Grand Theft Scratchy",
    "MEDAL_OF_HOMER": "Medal of Homer",
    "BIG_SUPER": "Big Super Happy Fun Fun Game",
    "RHYMES_WITH_COMPLAINING": "Rhymes with Complaining",
    "MEET_THY_PLAYER": "Meet Thy Player",
}
_EPISODE_RE = re.compile(r'^([ \t]*)episode\s*=\s*NewEpisode\(\s*game\s*,\s*"([A-Za-z0-9_]+)"', re.M)
_SET_DEFAULT_RE = re.compile(r'^[ \t]*episode:SetDefault\(\)[^\r\n]*', re.M)


def _gameflow_files():
    """(the gameflow file, its .original backup) or (None, None)."""
    lua = _find_ci(GAMEDATA, GAMEFLOW_LUA)
    if lua is None:
        return None, None
    return lua, lua.with_name(lua.name + ".original")


def _read_lua(path):
    with open(path, "r", encoding="utf-8", errors="surrogateescape", newline="") as f:
        return f.read()


def _episodes(text):
    """[(episode id, offset of its block, end of its block)] in file order."""
    found = list(_EPISODE_RE.finditer(text))
    return [(m.group(2), m.start(), found[i + 1].start() if i + 1 < len(found) else len(text))
            for i, m in enumerate(found)]


def _start_episode_of(text):
    """The episode whose block calls episode:SetDefault(), or None."""
    for episode_id, start, end in _episodes(text):
        if _SET_DEFAULT_RE.search(text, start, end):
            return episode_id
    return None


def start_episode_state():
    lua, _ = _gameflow_files()
    if lua is None:
        return "unavailable"
    return _start_episode_of(_read_lua(lua)) or "unavailable"


def start_episode_options():
    lua, backup = _gameflow_files()
    if lua is None:
        return []
    text = _read_lua(backup if backup.is_file() else lua)
    return [{"id": e, "name": EPISODE_NAMES.get(e, e.replace("_", " ").title())}
            for e, _, _ in _episodes(text)]


def _episode_text(original, episode_id):
    """Move the default call, preserving the untouched file's bytes and newlines."""
    if episode_id == _start_episode_of(original):
        return original
    newline = "\r\n" if "\r\n" in original else "\n"
    text = _SET_DEFAULT_RE.sub(
        lambda m: m.group(0).replace("episode:SetDefault()", "-- episode:SetDefault()", 1)
        + "  -- start episode moved by the launcher", original, count=1)
    match = next(m for m in _EPISODE_RE.finditer(text) if m.group(2) == episode_id)
    line_end = text.find("\n", match.end())
    if line_end < 0:
        text += newline
        line_end = len(text)
    else:
        line_end += 1
    text = (text[:line_end] + f"{match.group(1)}episode:SetDefault()  -- start episode chosen in the launcher"
            + newline + text[line_end:])
    if _start_episode_of(text) != episode_id or len(_SET_DEFAULT_RE.findall(text)) != 1:
        raise ValueError("could not move the start episode")
    return text


def _write_lua(path, text):
    # A unique temporary file also keeps a failed write from truncating game data.
    fd, tmp = tempfile.mkstemp(prefix=path.name + ".", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(text.encode("utf-8", errors="surrogateescape"))
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    finally:
        Path(tmp).unlink(missing_ok=True)


def set_start_episode(episode_id):
    with patch_lock:
        lua, backup = _gameflow_files()
        if lua is None:
            return False, "game data not installed"
        try:
            original = _read_lua(backup if backup.is_file() else lua)
            found = _episodes(original)
            episodes = {e for e, _, _ in found}
            original_start = _start_episode_of(original)
            if episode_id not in episodes or original_start is None:
                return False, f"unknown episode {episode_id!r}"
            if len(episodes) != len(found) or len(_SET_DEFAULT_RE.findall(original)) != 1:
                return False, "unexpected simpsons_gameflow.lua (expected unique episodes and one default)"
            if backup.is_file():
                current = _read_lua(lua)
                selected = _start_episode_of(current)
                if selected not in episodes or current != _episode_text(original, selected):
                    return False, "gameflow was edited outside the launcher; original backup kept"
            text = _episode_text(original, episode_id)
            if episode_id == original_start:
                if backup.is_file():
                    _write_lua(lua, original)
                    backup.unlink()
            else:
                if not backup.is_file():
                    # Do not overwrite another tool's backup.
                    with open(backup, "xb") as f:
                        f.write(original.encode("utf-8", errors="surrogateescape"))
                        f.flush()
                        os.fsync(f.fileno())
                _write_lua(lua, text)
        except (OSError, ValueError) as e:
            return False, str(e)
        name = EPISODE_NAMES.get(episode_id, episode_id)
        return True, f"new games start in {name}"


def _toml_flag(key, default="false"):
    if not GAME_TOML.exists():
        return default
    m = re.search(rf"^{key}\s*=\s*(\S+)", GAME_TOML.read_text(encoding="utf-8"), re.M)
    return m.group(1) if m else default


def retire_instant_popin_patch():
    """The old 'Instant character pop-in' patch (the stale streaming 'priming'
    draws, which could hang the GPU on Steam Deck) is gone: the engine draws
    characters with absent optional streams itself now. Turn its settings off
    for anyone who still had it on."""
    if _toml_flag("gpu_allow_invalid_fetch_constants") != "true":
        return
    text = GAME_TOML.read_text(encoding="utf-8")
    text = re.sub(r"^gpu_allow_invalid_fetch_constants\s*=.*$",
                  "gpu_allow_invalid_fetch_constants = false", text, flags=re.M)
    text = re.sub(r"^gpu_shader_max_cf_iterations\s*=.*$",
                  "gpu_shader_max_cf_iterations = 0", text, flags=re.M)
    GAME_TOML.write_text(text, encoding="utf-8")
    # the shader runaway cap it enabled is baked into translated shaders
    shutil.rmtree(USER_DATA / "cache", ignore_errors=True)


def patches_list():
    return [
        {"id": "skip_intro", "name": "Skip intro logo videos",
         "desc": "Boots straight past the EA / Fox / Gracie logo movies.",
         "state": patch_skip_intro_state(), "available": patch_skip_intro_state() != "unavailable"},
        {"id": "start_episode", "name": "Start episode (level select)",
         "desc": "New games start in the chosen episode, skipping the ones before it. Games "
                 "already in progress keep their progress. Takes effect the next time the game "
                 "starts; choose Land of Chocolate to go back to the original game.",
         "state": start_episode_state(), "available": start_episode_state() != "unavailable",
         "options": start_episode_options()},
        {"id": "fps_unlock", "name": "60 FPS mode",
         "desc": "Runs the game at 60 Hz instead of the original 30. Set it in "
                 "Settings → FRAMERATE. Experimental: cutscenes/physics may misbehave.",
         "state": "see settings", "available": False},
    ]


# ------------------------------------------------------------------ status

def game_running_pids():
    try:
        out = subprocess.run(["pgrep", "-f", str(GAME_BIN)], capture_output=True, text=True,
                             **POPEN_NO_WINDOW)
        return [int(p) for p in out.stdout.split()]
    except Exception:
        return []


def _find_ci(directory, name, *, folder=False):
    """Locate a file or folder case-insensitively, like the guest filesystem.

    extract-xiso preserves whatever case the disc used, so a Linux install can
    end up with DEFAULT.XEX while everything looks for default.xex.
    """
    if not directory.is_dir():
        return None
    matches_kind = Path.is_dir if folder else Path.is_file
    exact = directory / name
    if matches_kind(exact):
        return exact
    lowered = name.lower()
    for entry in directory.iterdir():
        if matches_kind(entry) and entry.name.lower() == lowered:
            return entry
    return None


def _looks_like_game_dir(directory):
    return (directory / "movies").is_dir() and _find_ci(directory, "default.xex") is not None


def gamedata_ok():
    # The engine needs default.xex as well as the movies folder; checking only
    # for movies let a half-extracted install look ready and fail at boot.
    return _looks_like_game_dir(GAMEDATA)


def art_files():
    return sorted(p.name for p in ART_DIR.glob("hero*.jpg")) if ART_DIR.is_dir() else []


def art_version():
    """Newest art file mtime — the UI uses this to know when to re-fetch
    images, since regeneration reuses the same hero*.jpg names."""
    try:
        return max(int(p.stat().st_mtime) for p in ART_DIR.glob("hero*.jpg"))
    except (ValueError, OSError):
        return 0


art_state = {"running": False}


def list_mods():
    MODS_DIR.mkdir(exist_ok=True)
    return sorted(p.name for p in MODS_DIR.iterdir() if not p.name.startswith("."))


def list_backups():
    BACKUPS_DIR.mkdir(exist_ok=True)
    return [{"name": p.name, "size_kb": p.stat().st_size // 1024,
             "date": time.strftime("%Y-%m-%d %H:%M", time.localtime(p.stat().st_mtime))}
            for p in sorted(BACKUPS_DIR.glob("saves-*.zip"), reverse=True)]


def status():
    return {
        "version": VERSION,
        "platform": PLAT,
        "ui_theme": CONFIG.get("ui_theme", "system"),
        "engine_ready": GAME_BIN.exists(),
        "engine_date": time.strftime("%Y-%m-%d %H:%M", time.localtime(GAME_BIN.stat().st_mtime)) if GAME_BIN.exists() else None,
        "gamedata_ready": gamedata_ok(),
        "art": art_files(),
        "art_version": art_version(),
        "art_running": art_state["running"],
        "running": bool(game_running_pids()),
        "mods": list_mods(),
        "backups": list_backups(),
        "patches": patches_list(),
        "github_repo": CONFIG.get("github_repo", ""),
        "diagnostics_enabled": diagnostics_enabled(),
        "capture_armed": capture_armed(),
        "update": update_state,
        "steam_available": bool(shutil.which("steamos-add-to-steam") or shutil.which("steam")),
        "install": {"running": install_state["running"], "ok": install_state["ok"],
                    "log": install_state["log"][-40:]},
        "platforms": [
            {"name": "Linux / Steam Deck", "state": "ready"},
            {"name": "Windows", "state": "experimental"},
            {"name": "Android", "state": "planned"},
        ],
    }


# ------------------------------------------------------------ file browser

def browse_roots():
    if PLAT == "Windows":
        # Drive letters, probed per call so USB drives plugged in while the
        # launcher is open still show up.
        drives = [Path(f"{d}:\\") for d in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"]
        return [Path.home()] + [d for d in drives if d.is_dir()]
    return [Path.home(), Path("/run/media"), Path("/media"), Path("/mnt")]


def browse(path_str):
    roots = browse_roots()
    if not path_str:
        entries = [{"name": str(r), "path": str(r), "dir": True} for r in roots if r.is_dir()]
        return {"path": "", "up": None, "entries": entries}
    # Only folders inside one of the starting locations can be listed. realpath
    # resolves ".." and symlinks before the check, and the trailing separator
    # on both sides keeps /home/deck from also matching /home/deck2.
    prefixes = tuple(os.path.join(os.path.realpath(r), "") for r in roots)
    folder = os.path.join(os.path.realpath(path_str), "")
    if folder.startswith(prefixes):
        return _browse_folder(folder, prefixes)
    return _browse_folder(os.path.join(os.path.realpath(Path.home()), ""), prefixes)


def _browse_folder(folder, prefixes):
    p = Path(folder)
    entries = []
    try:
        for child in sorted(p.iterdir(), key=lambda c: (not c.is_dir(), c.name.lower())):
            if child.name.startswith("."):
                continue
            if child.is_dir():
                entries.append({"name": child.name, "path": str(child), "dir": True})
            elif child.suffix.lower() in (".iso", ".xiso", ".360", ".img"):
                entries.append({"name": child.name, "path": str(child), "dir": False,
                                "size_mb": child.stat().st_size // (1 << 20)})
    except OSError:
        pass
    # ".." goes back to the starting locations once it would leave them.
    parent = p.parent
    inside = parent != p and os.path.join(str(parent), "").startswith(prefixes)
    return {"path": str(p), "up": str(parent) if inside else "", "entries": entries[:400]}


# -------------------------------------------------------------------- art

def ffmpeg_path():
    """ffmpeg from PATH, or bundled beside the launcher (Windows release)."""
    local = LAUNCHER_DIR / ("ffmpeg.exe" if PLAT == "Windows" else "ffmpeg")
    if local.exists():
        return str(local)
    return shutil.which("ffmpeg")


# The marker is written after an artwork sweep so a machine where every frame
# extraction fails (unreadable movies) doesn't re-run the whole sweep on every
# boot - that is what produced the endless window storm and the minute-long
# startup for players whose art folder stayed empty. The UI's refresh action
# passes force=True to retry. Sweeps by launchers before 0.0.6.2 seeked the
# input, which gave repeated frames and, with current ffmpeg builds (the one
# bundled on Windows), nothing at all (#7); the marker's new name makes every
# install redo its art once.
ART_MARKER = ".attempted-v2"


def generate_art(force=False):
    if not gamedata_ok() or not ffmpeg_path():
        return
    if art_state["running"]:
        return
    art_state["running"] = True
    try:
        _generate_art_locked(force)
    finally:
        art_state["running"] = False


def _extract_frame(movie, ts, out):
    """Writes the frame of a movie at ts seconds to out; True if a frame was
    written. The seek is on the output side: VP6 can't start decoding in the
    middle of a stream, so seeking the input decodes garbage frames, which
    older ffmpeg turned into repeated pictures and current builds refuse with
    "decode error rate exceeds maximum" (exit code 69). Decoding from the start
    takes well under a second for these movies."""
    try:
        subprocess.run(
            [ffmpeg_path(), "-loglevel", "error", "-y", "-i", str(movie), "-ss", ts,
             "-frames:v", "1", "-q:v", "3", str(out)],
            capture_output=True, timeout=60, **POPEN_NO_WINDOW)
    except (subprocess.TimeoutExpired, OSError):
        return False
    # The exit code isn't trusted either way: a frame that was written is good
    # even when ffmpeg also reported decode errors elsewhere in the stream.
    return out.exists() and out.stat().st_size > 0


def _generate_art_locked(force):
    ART_DIR.mkdir(parents=True, exist_ok=True)
    marker = ART_DIR / ART_MARKER
    if not force and marker.exists():
        return
    movie_dir = GAMEDATA / "movies" / "en"
    if not movie_dir.is_dir():
        # Non-English dumps keep their movies under a different language code.
        language_dirs = sorted(d for d in (GAMEDATA / "movies").glob("*") if d.is_dir())
        if not language_dirs:
            return
        movie_dir = language_dirs[0]
    # prefer HD in-game cutscenes, then any other movies (skip logo bumpers)
    igc = sorted(p for p in movie_dir.glob("*_igc*.vp6") if "_sd" not in p.name)
    rest = sorted(p for p in movie_dir.glob("*.vp6")
                  if "_sd" not in p.name and "logo" not in p.name and p not in igc)
    candidates = igc + rest
    # Frames are made under temporary names and only replace the old set at the
    # end, so the launcher keeps showing art while a sweep runs.
    for stale in list(ART_DIR.glob(".new*.jpg")) + list(ART_DIR.glob(".fb*.jpg")):
        stale.unlink(missing_ok=True)
    made = []
    seen = set()
    attempts = 0
    for mv in candidates:
        # A hard ceiling on ffmpeg invocations, not just on successes: when
        # every extraction fails, the success counter never advances and the
        # loop would otherwise walk 3 timestamps x every movie on the disc.
        if len(made) >= 24 or attempts >= 48:
            break
        for ts in ("3.0", "8.0", "14.0"):
            if len(made) >= 24 or attempts >= 48:
                break
            attempts += 1
            out = ART_DIR / f".new{attempts}.jpg"
            if not _extract_frame(mv, ts, out):
                out.unlink(missing_ok=True)
                continue
            data = out.read_bytes()
            digest = hashlib.sha1(data).hexdigest()
            if digest in seen:
                out.unlink(missing_ok=True)
                continue
            seen.add(digest)
            # size threshold filters black/flat frames
            if len(data) > 45000:
                made.append(out)
            elif len(data) > 12000:
                # Decoded fine but too small for the main cut - keep as a
                # fallback so a dark-ish set of movies still yields art
                # instead of a permanently empty folder.
                out.rename(ART_DIR / f".fb{attempts}.jpg")
            else:
                out.unlink(missing_ok=True)
    if not made:
        # Promote the largest decodable frames rather than leaving nothing.
        made = sorted(ART_DIR.glob(".fb*.jpg"), key=lambda p: p.stat().st_size,
                      reverse=True)[:8]
    if made:
        for old_art in ART_DIR.glob("hero*.jpg"):
            old_art.unlink(missing_ok=True)
        for i, frame in enumerate(made):
            frame.replace(ART_DIR / f"hero{i}.jpg")
    for leftover in list(ART_DIR.glob(".new*.jpg")) + list(ART_DIR.glob(".fb*.jpg")):
        leftover.unlink(missing_ok=True)
    try:
        (ART_DIR / ".attempted").unlink(missing_ok=True)
        marker.write_text(f"made={len(made)} attempts={attempts}\n", encoding="utf-8")
    except OSError:
        pass


# ------------------------------------------------------------------- icon

def write_donut_icon(path, size=128):
    import math
    w = h = size
    cx = cy = size / 2
    R, r_hole = size * 0.42, size * 0.16
    spr = []
    for i in range(26):
        a = i * 2.399963
        rad = r_hole + (R - r_hole) * (0.35 + 0.45 * ((i * 37) % 10) / 10)
        spr.append((cx + math.cos(a) * rad, cy + math.sin(a) * rad, a,
                    [(255, 255, 255), (87, 185, 232), (255, 217, 15),
                     (124, 179, 66), (142, 36, 170)][i % 5]))
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            d = math.hypot(x - cx, y - cy)
            px = (0, 0, 0, 0)
            if r_hole <= d <= R:
                px = (232, 163, 61, 255)
                wave = math.sin(x * 0.35) * 2.5
                if d <= R * 0.94 and d >= r_hole * 1.08 and (y < cy + R * 0.28 + wave):
                    px = (240, 98, 146, 255)
                    for sx, sy, sa, col in spr:
                        dx, dy = x - sx, y - sy
                        u = dx * math.cos(sa) + dy * math.sin(sa)
                        v = -dx * math.sin(sa) + dy * math.cos(sa)
                        if abs(u) < 4 and abs(v) < 1.6:
                            px = (*col, 255)
                            break
                if d > R - 2 or d < r_hole + 2:
                    px = (35, 38, 41, 255)
            row += bytes(px)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n"
                           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(raw, 9))
                           + chunk(b"IEND", b""))


def install_desktop_entry():
    icon = LAUNCHER_DIR / "icon.png"
    if not icon.exists():
        try:
            write_donut_icon(icon)
        except Exception:
            pass
    apps = Path.home() / ".local/share/applications"
    apps.mkdir(parents=True, exist_ok=True)
    (apps / "simpsons-recompiled.desktop").write_text(f"""[Desktop Entry]
Type=Application
Name=The Simpsons Game (Recompiled)
Comment=Launcher for the Simpsons Game native recompilation
Exec={LAUNCHER_DIR}/simpsons-launcher.sh
Icon={icon if icon.exists() else 'applications-games'}
Terminal=false
Categories=Game;
""")


# ---------------------------------------------------------------- install

def run_install(iso_path):
    install_state["running"] = True
    install_state["ok"] = None
    install_state["log"] = []
    log = install_state["log"]

    def fail(msg):
        log.append("ERROR: " + msg)
        install_state["ok"] = False
        install_state["running"] = False

    try:
        iso = Path(iso_path).expanduser()
        if not iso.is_file():
            return fail(f"ISO not found: {iso}")
        if not EXTRACT_XISO.exists():
            # The most common way this "goes missing" on Windows: the player
            # double-clicked the exe inside the downloaded ZIP, so Explorer
            # unpacked just the exe into a temp folder and ran it there - away
            # from every other file in the archive. Detect that and say so,
            # instead of leaving them staring at files that "seem to be there".
            exe = str(Path(sys.executable))
            if FROZEN and PLAT == "Windows" and ("\\Temp\\" in exe or "/Temp/" in exe):
                return fail("The launcher is running from a temporary folder - this "
                            "happens when it is started from inside the ZIP. Extract the "
                            "whole archive first (right-click the ZIP -> Extract All), "
                            "then run simpsons-launcher.exe from the extracted folder.")
            return fail(f"extract-xiso tool missing - expected it at {EXTRACT_XISO}, next to "
                        "the launcher. If an antivirus quarantined it, restore it or "
                        "re-extract the release archive.")
        target = ROOT / "gamedata_extracting"
        if target.exists():
            shutil.rmtree(target)
        target.mkdir()
        log.append(f"Extracting {iso.name} ... (this can take a few minutes)")
        # Options must precede the positional ISO: the Windows getopt does not
        # permute argv, so "-x <iso> -d <target>" would leave -d unparsed.
        p = subprocess.Popen([str(EXTRACT_XISO), "-x", "-d", str(target), str(iso)],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                             **POPEN_NO_WINDOW)
        for line in p.stdout:
            if line.strip():
                log.append(line.rstrip())
                del log[:-400]
        p.wait()
        if p.returncode != 0:
            return fail(f"extract-xiso exited with {p.returncode}")
        if not _looks_like_game_dir(target):
            nested = [d for d in target.iterdir() if d.is_dir() and _looks_like_game_dir(d)]
            if not nested:
                # Say what was actually extracted - a silent "DONE!" here used to
                # hand the engine a folder with no default.xex in it, which only
                # showed up later as a cryptic "Entrypoint XEX not found".
                found = sorted(p.name for p in target.iterdir())[:12] if target.is_dir() else []
                return fail("Extraction finished but this doesn't look like The Simpsons "
                            "Game (need both default.xex and a movies folder). "
                            + (f"Extracted instead: {', '.join(found)}" if found
                               else "Nothing was extracted."))
            target = nested[0]
        log.append("Extraction complete. Installing game data ...")
        if GAMEDATA.exists():
            backup = ROOT / "gamedata_previous"
            if backup.exists():
                shutil.rmtree(backup)
            GAMEDATA.rename(backup)
            log.append("(previous game data kept as gamedata_previous)")
        target.rename(GAMEDATA)
        log.append("Generating launcher artwork from your game files ...")
        generate_art(force=True)
        log.append("DONE! The game is installed and ready to play.")
        install_state["ok"] = True
    except Exception as e:  # noqa: BLE001
        fail(str(e))
    finally:
        install_state["running"] = False


# ------------------------------------------------------------- save backup

def backup_saves():
    if not USER_DATA.exists():
        return False, "No save data found yet — play the game first!"
    BACKUPS_DIR.mkdir(exist_ok=True)
    name = time.strftime("saves-%Y%m%d-%H%M%S.zip")
    with zipfile.ZipFile(BACKUPS_DIR / name, "w", zipfile.ZIP_DEFLATED) as z:
        for f in USER_DATA.rglob("*"):
            rel = f.relative_to(USER_DATA)
            if f.is_file() and rel.parts and rel.parts[0] != "cache":
                z.write(f, rel)
    return True, name


def restore_saves(name):
    # Only a backup from the list the launcher shows can be restored: the name
    # must match one of those files exactly, so it can't lead anywhere else.
    src = next((p for p in BACKUPS_DIR.glob("saves-*.zip") if p.name == name), None)
    if src is None or not src.is_file():
        return False, "backup not found"
    if game_running_pids():
        return False, "Stop the game before restoring saves"
    with zipfile.ZipFile(src) as z:
        z.extractall(USER_DATA)
    return True, "restored"


# ---------------------------------------------------------------- updates

# Paths inside a release download that are safe to overwrite wholesale on
# update (engine binaries + launcher code), relative to the extracted
# archive's own root -- release.yml ships a FLAT layout (simpsons[.exe] and
# friends sit right next to launcher/, not nested under simpsons/out/build/
# like a dev tree). Anything NOT listed here is preserved untouched even if
# the release also contains a copy of it -- in particular user data:
# launcher.json (settings), backups/ (save backups), mods/, art/
# (regenerated locally from the player's own files), gamedata/, and
# simpsons.toml (the player's own runtime settings).
if PLAT == "Windows":
    UPDATE_MANAGED_PATHS = ["simpsons.exe", "extract-xiso.exe", "ffmpeg.exe", "README.md",
                            "build_variant.txt"]
    UPDATE_GLOB_PATHS = ["*.dll"]
else:
    UPDATE_MANAGED_PATHS = ["simpsons", "extract-xiso", "launcher/ui",
                            "launcher/launcher.py", "launcher/simpsons-launcher.sh", "README.md",
                            "build_variant.txt"]
    UPDATE_GLOB_PATHS = ["*.so*"]
# The launcher's own executable (Windows only, PyInstaller-frozen release)
# needs special handling: it can't overwrite its own running file's content,
# but Windows does allow renaming a running exe out of the way first.
UPDATE_SELF_EXE = "simpsons-launcher.exe"


# Releases come in two builds: the default one for x86-64-v3 CPUs (AVX2, BMI2
# and FMA: Intel Haswell, AMD Excavator or Zen, and newer), and a NoAVX2 one
# for older CPUs. The default build fails to start on those with error
# 0xc0000142 on Windows (#28). Packages name their build in build_variant.txt.
BUILD_VARIANT_FILE = ROOT / "build_variant.txt"
VARIANT_DEFAULT = "x86-64-v3"
VARIANT_NO_AVX2 = "x86-64-v2"


def cpu_runs_default_build():
    """Whether this CPU has the x86-64-v3 features the default build uses."""
    try:
        if PLAT == "Windows":
            import ctypes
            # PF_AVX2_INSTRUCTIONS_AVAILABLE: also false when the OS does not
            # enable AVX. Every CPU with AVX2 also has the rest of x86-64-v3.
            return bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))
        with open("/proc/cpuinfo", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("flags"):
                    flags = set(line.split(":", 1)[1].split())
                    return {"avx2", "bmi1", "bmi2", "fma", "movbe"} <= flags
    except Exception:  # noqa: BLE001
        pass
    # Unknown: assume the default build, which fits nearly every PC today.
    return True


def installed_build_variant():
    """The build of the installed package, or None for a source tree or a
    package from before 0.0.6.0 (all of which were default builds)."""
    try:
        return BUILD_VARIANT_FILE.read_text(encoding="utf-8").strip() or None
    except OSError:
        return None


def wanted_build_variant():
    return VARIANT_DEFAULT if cpu_runs_default_build() else VARIANT_NO_AVX2


def _platform_asset_name(variant=VARIANT_DEFAULT):
    suffix = "-NoAVX2" if variant == VARIANT_NO_AVX2 else ""
    return (f"TheSimpsonsGame-Recompiled-Windows-x64{suffix}.zip" if PLAT == "Windows"
            else f"TheSimpsonsGame-Recompiled-Linux-x64{suffix}.tar.gz")


def check_updates():
    repo = CONFIG.get("github_repo", "")
    if not repo:
        update_state.update(checked=True, update_available=False, download_url=None,
                            msg="No GitHub repository configured.")
        return
    try:
        # /releases/latest is exactly what we want: the release GitHub marks
        # "latest" — published, non-draft, non-prerelease. release.yml always
        # publishes with make_latest on and prerelease off, so this is always
        # the build players should be offered. (Historic note: this used to
        # walk the full /releases list because early releases were published
        # as prereleases, which /releases/latest excludes.)
        req = urllib.request.Request(
            f"https://api.github.com/repos/{repo}/releases/latest",
            headers={"User-Agent": f"simpsons-launcher/{VERSION}"})
        try:
            with urllib.request.urlopen(req, timeout=8) as r:
                data = json.loads(r.read())
        except urllib.error.HTTPError as e:
            if e.code == 404:
                update_state.update(checked=True, update_available=False, download_url=None,
                                    msg="No published releases yet.")
                return
            raise
        tag = data.get("tag_name", "")
        wanted_variant = wanted_build_variant()
        asset_name = _platform_asset_name(wanted_variant)
        asset = next((a for a in data.get("assets", []) if a.get("name") == asset_name), None)
        # An installed package this CPU cannot run (the default build on a CPU
        # without AVX2) is replaced even without a newer version.
        installed_variant = installed_build_variant()
        wrong_build = (FROZEN or installed_variant is not None) and \
            (installed_variant or VARIANT_DEFAULT) != wanted_variant

        def _ver_tuple(s):
            # "v0.3.1" / "0.3.1" -> (0, 3, 1); malformed parts count as 0 so a
            # weird tag can never brick the comparison. Trailing zeros are
            # stripped so hotfix-style tags of different lengths compare
            # sanely: v0.0.4.0 == 0.0.4, while v0.0.4.1 > 0.0.4.
            parts = []
            for p in s.lstrip("vV").split("."):
                digits = "".join(ch for ch in p if ch.isdigit())
                parts.append(int(digits) if digits else 0)
            while parts and parts[-1] == 0:
                parts.pop()
            return tuple(parts)

        # Strictly newer only: a mismatched-but-older tag must never nag
        # every user with a bogus "update available".
        is_newer = bool(tag) and _ver_tuple(tag) > _ver_tuple(VERSION)
        if wrong_build and asset:
            update_state.update(checked=True, update_available=True,
                                download_url=asset["browser_download_url"], latest_tag=tag,
                                msg=f"This CPU needs the build for older CPUs (no AVX2): "
                                    f"install {tag} for it now.")
        elif is_newer and asset:
            update_state.update(checked=True, update_available=True,
                                download_url=asset["browser_download_url"], latest_tag=tag,
                                msg=f"Update available: {tag} (you're on {VERSION})")
        elif is_newer:
            update_state.update(checked=True, update_available=False, download_url=None,
                                msg=f"A newer release ({tag}) exists but has no {asset_name} asset for "
                                    f"this platform yet — see {data.get('html_url', '')}")
        else:
            update_state.update(checked=True, update_available=False, download_url=None,
                                msg=f"You're up to date (latest release: {tag or 'none'}).")
    except Exception as e:  # noqa: BLE001
        update_state.update(checked=True, update_available=False, download_url=None,
                            msg=f"Update check failed: {e}")


def apply_update():
    if game_running_pids():
        return False, "Close the game before updating — its files may be in use."
    url = update_state.get("download_url")
    if not url:
        return False, "No update ready to apply — check for updates first."
    import tarfile
    import tempfile
    try:
        update_state.update(applying=True, apply_msg="Downloading...")
        with tempfile.TemporaryDirectory(prefix="simpsons-update-") as tmp:
            tmp = Path(tmp)
            archive_path = tmp / url.split("/")[-1]
            req = urllib.request.Request(url, headers={"User-Agent": f"simpsons-launcher/{VERSION}"})
            with urllib.request.urlopen(req, timeout=120) as r, open(archive_path, "wb") as f:
                shutil.copyfileobj(r, f)

            update_state.update(apply_msg="Extracting...")
            extract_dir = tmp / "extracted"
            if archive_path.name.endswith(".zip"):
                with zipfile.ZipFile(archive_path) as z:
                    z.extractall(extract_dir)
            else:
                with tarfile.open(archive_path) as t:
                    t.extractall(extract_dir)

            # The archive's own top-level layout is flat (matches ROOT
            # directly) -- but a .zip made from a single top-level folder
            # sometimes nests everything one level deeper; detect that.
            entries = list(extract_dir.iterdir())
            if len(entries) == 1 and entries[0].is_dir():
                extract_dir = entries[0]

            update_state.update(apply_msg="Installing...")
            installed = []

            def install_one(rel_src, rel_dst=None):
                src = extract_dir / rel_src
                if not src.exists():
                    return
                dst = ROOT / (rel_dst or rel_src)
                if src.is_dir():
                    if dst.exists():
                        shutil.rmtree(dst)
                    shutil.copytree(src, dst)
                else:
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src, dst)
                # Normalise to a string: UPDATE_MANAGED_PATHS supplies plain
                # strings, but the glob pass below hands us a Path. Mixing the
                # two blew up the "Installed: ..." join at the end -- after the
                # files had already been copied -- so a successful update was
                # reported as a failure.
                installed.append(Path(rel_dst or rel_src).as_posix())

            for rel in UPDATE_MANAGED_PATHS:
                install_one(rel)
            for pattern in UPDATE_GLOB_PATHS:
                for src in extract_dir.glob(pattern):
                    install_one(src.relative_to(extract_dir))

            # The running launcher exe can't have its own file content
            # overwritten, but Windows does allow renaming an open exe out
            # of the way first -- the currently-running process keeps
            # executing fine from the renamed file.
            #
            # This must never fail silently: if everything else updates but
            # the launcher exe stays old, its (stamped) version keeps
            # comparing below the release it just installed, and the player
            # gets the "update available" nag again on every start with no
            # hint anything went wrong.
            if FROZEN and PLAT == "Windows":
                new_exe = extract_dir / UPDATE_SELF_EXE
                if not new_exe.exists():
                    update_state.update(applying=False,
                                        apply_msg="Launcher exe missing from archive.")
                    return False, (f"Update installed, but {UPDATE_SELF_EXE} was missing from "
                                   "the downloaded archive, so the launcher itself is still "
                                   "the old version. Download the release from GitHub and "
                                   "extract it over this folder to finish.")
                current_exe = Path(sys.executable)
                try:
                    old_exe = current_exe.with_suffix(".exe.old")
                    old_exe.unlink(missing_ok=True)
                    current_exe.rename(old_exe)
                    shutil.copy2(new_exe, current_exe)
                    installed.append(UPDATE_SELF_EXE)
                except OSError as e:
                    # Leftover .old locked by a previous instance, antivirus
                    # holding the new file, exe on read-only media... Fall
                    # back to dropping the new exe alongside with clear
                    # instructions rather than pretending the update finished.
                    try:
                        side = current_exe.with_name("simpsons-launcher-new.exe")
                        shutil.copy2(new_exe, side)
                        update_state.update(applying=False,
                                            apply_msg="Launcher exe could not be replaced.")
                        return False, ("Update installed, but the launcher couldn't replace "
                                       f"its own exe ({e}). The new version was saved as "
                                       f"{side.name} - close this launcher, delete the old "
                                       "simpsons-launcher.exe and rename the new one in its "
                                       "place.")
                    except OSError:
                        update_state.update(applying=False,
                                            apply_msg="Launcher exe could not be replaced.")
                        return False, ("Update installed, but the launcher couldn't replace "
                                       f"its own exe ({e}). Download the release from GitHub "
                                       "and extract it over this folder to finish.")

        update_state.update(applying=False, apply_msg="Updated — restart the launcher to finish.",
                            update_available=False, checked=True,
                            msg=f"Updated to {update_state.get('latest_tag', 'latest')}. Restart the launcher.")
        return True, f"Installed: {', '.join(installed)}. Restart the launcher to finish."
    except Exception as e:  # noqa: BLE001
        update_state.update(applying=False, apply_msg=f"Update failed: {e}")
        return False, f"Update failed: {e}"


# ------------------------------------------------------------ diagnostics
#
# One switch for players who hit crashes / black screens / stutter: when
# enabled, every PLAY press writes a session log (system info + full game
# output + a plain-English diagnosis of how the game ended) and samples
# CPU / RAM / GPU into a .csv while the game runs. A support bundle zips
# the lot for attaching to a GitHub issue. Everything stays local.

def diagnostics_enabled():
    return bool(CONFIG.get("diagnostics_enabled", False))


def set_diagnostics(enable):
    CONFIG["diagnostics_enabled"] = bool(enable)
    save_config()
    return diagnostics_enabled()


def _run_quiet(cmd, timeout=6):
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, **POPEN_NO_WINDOW).stdout.strip()
    except Exception:
        return ""


def _read_first(path, default=""):
    try:
        return Path(path).read_text(errors="replace").strip()
    except Exception:
        return default


def _tail_file(path, n, max_bytes=131072):
    """Last n lines of a possibly-huge file without reading all of it."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            f.seek(max(0, f.tell() - max_bytes))
            return f.read().decode("utf-8", "replace").splitlines()[-n:]
    except Exception:
        return []


def _gpu_perf_device():
    """sysfs perf node of the first GPU exposing amdgpu's busy% (Steam Deck
    and most AMD cards). None elsewhere -- sampling degrades gracefully."""
    if PLAT != "Linux":
        return None
    for card in sorted(Path("/sys/class/drm").glob("card[0-9]")):
        dev = card / "device"
        if (dev / "gpu_busy_percent").exists():
            return dev
    return None


def _win_mem_status():
    """(total_mb, avail_mb) of physical RAM on Windows, or (None, None)."""
    try:
        import ctypes

        class MEMORYSTATUSEX(ctypes.Structure):
            _fields_ = [("dwLength", ctypes.c_uint32), ("dwMemoryLoad", ctypes.c_uint32),
                        ("ullTotalPhys", ctypes.c_uint64), ("ullAvailPhys", ctypes.c_uint64),
                        ("ullTotalPageFile", ctypes.c_uint64), ("ullAvailPageFile", ctypes.c_uint64),
                        ("ullTotalVirtual", ctypes.c_uint64), ("ullAvailVirtual", ctypes.c_uint64),
                        ("ullAvailExtendedVirtual", ctypes.c_uint64)]

        st = MEMORYSTATUSEX()
        st.dwLength = ctypes.sizeof(st)
        if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st)):
            return int(st.ullTotalPhys) // (1 << 20), int(st.ullAvailPhys) // (1 << 20)
    except Exception:
        pass
    return None, None


def _win_proc_rss_mb(pid):
    try:
        import ctypes
        from ctypes import wintypes

        class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

        h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # QUERY_LIMITED_INFORMATION
        if not h:
            return None
        try:
            pmc = PROCESS_MEMORY_COUNTERS()
            pmc.cb = ctypes.sizeof(pmc)
            if ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
                return int(pmc.WorkingSetSize) // (1 << 20)
        finally:
            ctypes.windll.kernel32.CloseHandle(h)
    except Exception:
        pass
    return None


def system_info_text():
    lines = [f"launcher: v{VERSION} ({'packaged' if FROZEN else 'dev tree'})",
             f"platform: {platform.platform()}",
             f"machine: {platform.machine()}",
             f"python: {platform.python_version()}"]
    if PLAT == "Linux":
        m = re.search(r'^PRETTY_NAME="?([^"\n]+)', _read_first("/etc/os-release"), re.M)
        if m:
            lines.append(f"os: {m.group(1)}")
        m = re.search(r"^model name\s*:\s*(.+)$", _read_first("/proc/cpuinfo"), re.M)
        if m:
            lines.append(f"cpu: {m.group(1).strip()}")
        m = re.search(r"^MemTotal:\s*(\d+)", _read_first("/proc/meminfo"), re.M)
        if m:
            lines.append(f"ram: {int(m.group(1)) // 1024} MB")
        for lspci_line in _run_quiet(["lspci"]).splitlines():
            if re.search(r"VGA|3D controller|Display controller", lspci_line):
                lines.append(f"gpu: {lspci_line.split(':', 2)[-1].strip()}")
        dev = _gpu_perf_device()
        if dev:
            vram = _read_first(dev / "mem_info_vram_total")
            if vram.isdigit():
                lines.append(f"vram: {int(vram) // (1 << 20)} MB")
    else:
        cpu = os.environ.get("PROCESSOR_IDENTIFIER", "")
        if cpu:
            lines.append(f"cpu: {cpu}")
        total, _avail = _win_mem_status()
        if total:
            lines.append(f"ram: {total} MB")
        gpus = _run_quiet(["wmic", "path", "win32_VideoController", "get", "name"]).splitlines()
        for g in gpus[1:]:
            if g.strip():
                lines.append(f"gpu: {g.strip()}")
    lines += ["", "[settings]"]
    lines += [f"{k} = {v}" for k, v in read_settings().items()]
    lines += ["", "[patches]"]
    lines += [f"{p['id']}: {p['state']}" for p in patches_list()]
    return "\n".join(lines)


def describe_exit_code(code):
    if code is None:
        return "unknown"
    if code == 0:
        return "0 (clean exit)"
    if code < 0:
        try:
            name = signal.Signals(-code).name
        except (ValueError, AttributeError):
            name = f"signal {-code}"
        return f"{code} (killed by {name})"
    return f"{code} (error)"


def diagnose_exit(code, duration, log_text):
    """Turn an exit code + log tail into plain English a player can act on."""
    low = (log_text or "").lower()
    hints = []
    if code == 0:
        verdict = "The game exited normally."
        if duration < 60:
            hints.append("The session was very short — if the window went black or closed "
                         "on its own, create a support bundle and attach it to a GitHub issue.")
    elif code is not None and code < 0:
        sig = -code
        if sig == getattr(signal, "SIGKILL", 9):
            verdict = ("The game was force-killed by the system (SIGKILL) — on Linux this is "
                       "almost always the out-of-memory killer striking during a level-load "
                       "memory spike.")
            hints.append("Close other applications to free RAM, or lower the render "
                         "resolution scale in Settings.")
        elif sig == getattr(signal, "SIGTERM", 15):
            verdict = "The game was stopped (SIGTERM) — usually the STOP button or a system shutdown."
        else:
            try:
                name = signal.Signals(sig).name
            except (ValueError, AttributeError):
                name = f"signal {sig}"
            verdict = f"The game crashed ({name})."
    else:
        verdict = f"The game exited with error code {code}."
    if any(s in low for s in ("device lost", "vk_error_device_lost", "gpu hang", "gpu hung",
                              "device_lost")):
        hints.append("A GPU hang / 'device lost' shows in the log. The launcher purges the "
                     "shader cache automatically after a bad exit, so the next launch may "
                     "stutter briefly while it rebuilds.")
    if any(s in low for s in ("out of memory", "bad_alloc", "not enough memory")):
        hints.append("An out-of-memory error shows in the log — close other applications or "
                     "lower quality settings.")
    if "vulkan" in low and ("failed" in low or "error" in low):
        hints.append("Vulkan errors show in the log — make sure your graphics drivers are up "
                     "to date.")
    if code not in (0, None) and duration < 15:
        hints.append("The game died during startup — check that game data is installed "
                     "(Install tab) and try setting Settings → Resolution preset back to "
                     "Automatic.")
    return " ".join([verdict] + hints)


class DiagSession:
    """One diagnostic recording of one game run: a .log with system info +
    game output + final summary, and a .perf.csv sampled every 2 seconds."""

    SAMPLE_SECS = 2.0
    KEEP_SESSIONS = 20

    def __init__(self):
        DIAG_DIR.mkdir(exist_ok=True)
        self.name = time.strftime("session-%Y%m%d-%H%M%S")
        self.log_path = DIAG_DIR / f"{self.name}.log"
        self.perf_path = DIAG_DIR / f"{self.name}.perf.csv"
        self.summary_path = DIAG_DIR / f"{self.name}.summary.json"
        self.t0 = time.time()
        self.peak_rss = 0
        self.cpu_samples = []
        self.gpu_samples = []
        self._prune_old()

    def _prune_old(self):
        try:
            for old in sorted(DIAG_DIR.glob("session-*.log"))[:-self.KEEP_SESSIONS]:
                stem = old.name[:-len(".log")]
                for suffix in (".log", ".perf.csv", ".summary.json"):
                    (DIAG_DIR / (stem + suffix)).unlink(missing_ok=True)
        except Exception:
            pass

    def write_header(self, cmd, env_notes):
        try:
            with open(self.log_path, "w", encoding="utf-8") as f:
                f.write("The Simpsons Game — Recompiled : diagnostic session log\n")
                f.write(f"started: {time.strftime('%Y-%m-%d %H:%M:%S')}\n\n")
                f.write(system_info_text() + "\n\n")
                if env_notes:
                    f.write("[launch environment]\n" + "\n".join(env_notes) + "\n\n")
                f.write("command: " + " ".join(cmd) + "\n")
                f.write("==== GAME OUTPUT ====\n")
        except Exception:
            pass

    def game_output_handle(self):
        try:
            return open(self.log_path, "a", encoding="utf-8", errors="replace")
        except Exception:
            return subprocess.DEVNULL

    def monitor(self, proc):
        """Sample the game process until it exits. Cheap: a few tiny sysfs /
        procfs reads every 2 s on Linux, one WinAPI call on Windows."""
        try:
            clk = os.sysconf("SC_CLK_TCK")
        except (AttributeError, ValueError, OSError):
            clk = 100
        gpu_dev = _gpu_perf_device()
        last = None  # (wall time, cpu ticks)
        try:
            pf = open(self.perf_path, "w", encoding="utf-8")
        except Exception:
            return
        with pf:
            pf.write("t_seconds,cpu_percent,rss_mb,threads,sys_avail_mb,"
                     "gpu_busy_percent,vram_used_mb\n")
            while proc.poll() is None:
                time.sleep(self.SAMPLE_SECS)
                if proc.poll() is not None:
                    break
                t = time.time()
                cpu = rss = threads_n = sys_avail = gpu = vram = ""
                if PLAT == "Linux":
                    stat = _read_first(f"/proc/{proc.pid}/stat")
                    if stat and ")" in stat:
                        try:
                            fields = stat.rsplit(")", 1)[1].split()
                            ticks = int(fields[11]) + int(fields[12])  # utime+stime
                            threads_n = fields[17]
                            if last:
                                cpu = f"{(ticks - last[1]) / clk / (t - last[0]) * 100:.0f}"
                            last = (t, ticks)
                        except (IndexError, ValueError):
                            pass
                    m = re.search(r"^VmRSS:\s*(\d+)",
                                  _read_first(f"/proc/{proc.pid}/status"), re.M)
                    if m:
                        rss = str(int(m.group(1)) // 1024)
                    m = re.search(r"^MemAvailable:\s*(\d+)", _read_first("/proc/meminfo"), re.M)
                    if m:
                        sys_avail = str(int(m.group(1)) // 1024)
                    if gpu_dev:
                        gpu = _read_first(gpu_dev / "gpu_busy_percent")
                        v = _read_first(gpu_dev / "mem_info_vram_used")
                        if v.isdigit():
                            vram = str(int(v) // (1 << 20))
                else:
                    r = _win_proc_rss_mb(proc.pid)
                    if r is not None:
                        rss = str(r)
                    _total, avail = _win_mem_status()
                    if avail is not None:
                        sys_avail = str(avail)
                try:
                    self.peak_rss = max(self.peak_rss, int(rss or 0))
                    if cpu:
                        self.cpu_samples.append(float(cpu))
                    if gpu:
                        self.gpu_samples.append(float(gpu))
                except ValueError:
                    pass
                try:
                    pf.write(f"{t - self.t0:.0f},{cpu},{rss},{threads_n},"
                             f"{sys_avail},{gpu},{vram}\n")
                    pf.flush()
                except Exception:
                    break

    def finalize(self, code, notes=()):
        duration = time.time() - self.t0
        log_text = "\n".join(tail_game_log(200) + _tail_file(self.log_path, 200))
        diagnosis = diagnose_exit(code, duration, log_text)
        avg_cpu = round(sum(self.cpu_samples) / len(self.cpu_samples)) if self.cpu_samples else None
        max_gpu = round(max(self.gpu_samples)) if self.gpu_samples else None
        summary = {"name": self.name, "exit_code": code,
                   "exit_desc": describe_exit_code(code),
                   "duration_s": round(duration),
                   "peak_rss_mb": self.peak_rss or None,
                   "avg_cpu_percent": avg_cpu, "max_gpu_percent": max_gpu,
                   "diagnosis": diagnosis, "notes": list(notes),
                   "ended": time.strftime("%Y-%m-%d %H:%M:%S")}
        try:
            self.summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
        except Exception:
            pass
        try:
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write("\n==== SESSION SUMMARY ====\n")
                f.write(f"exit: {describe_exit_code(code)}\n")
                f.write(f"duration: {round(duration)} s\n")
                if self.peak_rss:
                    f.write(f"peak game memory: {self.peak_rss} MB\n")
                if avg_cpu is not None:
                    f.write(f"average cpu: {avg_cpu}%\n")
                if max_gpu is not None:
                    f.write(f"peak gpu busy: {max_gpu}%\n")
                for n in notes:
                    f.write(n + "\n")
                f.write("diagnosis: " + diagnosis + "\n")
        except Exception:
            pass


def diagnostics_report():
    sessions = sorted(DIAG_DIR.glob("session-*.log"), reverse=True) if DIAG_DIR.is_dir() else []
    last_summary = None
    summaries = sorted(DIAG_DIR.glob("session-*.summary.json"), reverse=True)
    if summaries:
        try:
            last_summary = json.loads(summaries[0].read_text(encoding="utf-8"))
        except Exception:
            pass
    return {
        "enabled": diagnostics_enabled(),
        "running": bool(game_running_pids()),
        "dir": str(DIAG_DIR),
        "sessions": [{"name": p.name, "size_kb": p.stat().st_size // 1024,
                      "date": time.strftime("%Y-%m-%d %H:%M", time.localtime(p.stat().st_mtime))}
                     for p in sessions[:8]],
        "tail": _tail_file(sessions[0], 120) if sessions else [],
        "last_summary": last_summary,
    }


def create_support_bundle():
    """Zip recent diagnostics + config + engine log tails for a bug report.
    Contents: system/hardware info, launcher settings, game settings
    (simpsons.toml), session logs & perf samples, engine log tails. No save
    data, no game content, nothing leaves the machine."""
    try:
        DIAG_DIR.mkdir(exist_ok=True)
        path = DIAG_DIR / time.strftime("support-bundle-%Y%m%d-%H%M%S.zip")
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("system-info.txt", system_info_text() + "\n")
            for log in sorted(DIAG_DIR.glob("session-*.log"), reverse=True)[:3]:
                stem = log.name[:-len(".log")]
                z.writestr(f"sessions/{log.name}", "\n".join(_tail_file(log, 2000, 1 << 20)) + "\n")
                for extra in (f"{stem}.perf.csv", f"{stem}.summary.json"):
                    if (DIAG_DIR / extra).is_file():
                        z.write(DIAG_DIR / extra, f"sessions/{extra}")
            engine_logs = sorted((BUILD_DIR / "logs").glob("simpsons_*.log"),
                                 key=lambda p: p.stat().st_mtime, reverse=True)[:2]
            for elog in engine_logs:
                z.writestr(f"engine-logs/{elog.name}",
                           "\n".join(_tail_file(elog, 1000, 1 << 20)) + "\n")
            if CAPTURE_DIR.is_dir():
                for cap in CAPTURE_DIR.iterdir():
                    if cap.is_file():
                        z.write(cap, f"capture/{cap.name}")
            for f in (GAME_TOML, CONFIG_JSON, LAUNCHER_DIR / "last_exit.txt",
                      LAUNCHER_DIR / "last_run_debug.txt",
                      LAUNCHER_DIR / "launcher_native_error.log"):
                if f.is_file():
                    z.write(f, f.name)
        return True, str(path)
    except Exception as e:  # noqa: BLE001
        return False, f"bundle failed: {e}"


# ----------------------------------------------------------------- launch

def repair_saves():
    """Self-heal 'damaged' save slots: the game truncates a slot to 0 bytes when
    it is killed mid-write (or mid-failed-load). Restore any zero-byte slot from
    the newest backup that holds a good copy; if none exists, remove the husk so
    the game sees a clean empty slot instead of a damaged one."""
    repaired = []
    try:
        profiles = list(USER_DATA.glob("*/45410809"))
        backups = sorted(BACKUPS_DIR.glob("*.zip"), key=lambda p: p.stat().st_mtime,
                         reverse=True) if BACKUPS_DIR.is_dir() else []
        # also treat the flatpak-originals folder backups as a source
        folder_backups = sorted(BACKUPS_DIR.glob("flatpak-originals-*"),
                                key=lambda p: p.stat().st_mtime, reverse=True)
        for prof in profiles:
            for slot_file in prof.glob("00000001/*/*"):
                if not slot_file.is_file() or slot_file.stat().st_size >= 4096:
                    continue
                slot_name = slot_file.name
                rel_data = f"{prof.parent.name}/45410809/00000001/{slot_name}/{slot_name}"
                rel_head = f"{prof.parent.name}/45410809/Headers/00000001/{slot_name}.header"
                fixed = False

                # Prefer a live snapshot: those are taken while playing, so
                # they cost at most one save-write, where the launch-time zips
                # cost the whole session.
                guard = restore_slot_from_guard(slot_name)
                if guard is not None and guard.stat().st_size >= 4096:
                    slot_file.write_bytes(guard.read_bytes())
                    guard_head = guard.with_suffix(".header")
                    if guard_head.is_file():
                        head = prof / "Headers/00000001" / f"{slot_name}.header"
                        head.parent.mkdir(parents=True, exist_ok=True)
                        head.write_bytes(guard_head.read_bytes())
                    repaired.append(f"{slot_name} (from live snapshot)")
                    continue
                for zf in backups:
                    try:
                        with zipfile.ZipFile(zf) as z:
                            names = set(z.namelist())
                            if rel_data in names and z.getinfo(rel_data).file_size >= 4096:
                                slot_file.write_bytes(z.read(rel_data))
                                if rel_head in names:
                                    head = prof / "Headers/00000001" / f"{slot_name}.header"
                                    head.parent.mkdir(parents=True, exist_ok=True)
                                    head.write_bytes(z.read(rel_head))
                                repaired.append(f"{slot_name} (from {zf.name})")
                                fixed = True
                                break
                    except Exception:
                        continue
                if not fixed:
                    for fb in folder_backups:
                        src = fb / rel_data
                        srch = fb / rel_head
                        if src.is_file() and src.stat().st_size >= 4096:
                            slot_file.write_bytes(src.read_bytes())
                            if srch.is_file():
                                head = prof / "Headers/00000001" / f"{slot_name}.header"
                                head.write_bytes(srch.read_bytes())
                            repaired.append(f"{slot_name} (from {fb.name})")
                            fixed = True
                            break
                if not fixed:
                    # no good copy anywhere: remove the husk + header entirely
                    shutil.rmtree(slot_file.parent, ignore_errors=True)
                    (prof / "Headers/00000001" / f"{slot_name}.header").unlink(missing_ok=True)
                    repaired.append(f"{slot_name} (husk removed)")
    except Exception:
        pass
    return repaired


def auto_backup_saves():
    try:
        if not USER_DATA.exists():
            return
        BACKUPS_DIR.mkdir(exist_ok=True)
        name = time.strftime("auto-%Y%m%d-%H%M%S.zip")
        with zipfile.ZipFile(BACKUPS_DIR / name, "w", zipfile.ZIP_DEFLATED) as z:
            for f in USER_DATA.rglob("*"):
                rel = f.relative_to(USER_DATA)
                if f.is_file() and rel.parts and rel.parts[0] != "cache":
                    z.write(f, rel)
        autos = sorted(BACKUPS_DIR.glob("auto-*.zip"))
        for old_zip in autos[:-10]:
            old_zip.unlink()
    except Exception:
        pass


# ---------------------------------------------------- live save protection

# The engine truncates a save slot to zero bytes the moment it opens it for
# writing (HostPathEntry::Truncate uses "wb"), and only then writes the new
# contents. A crash anywhere in that window - a GPU device-lost, a hang, a
# kill - leaves a 0-byte husk and the save is simply gone. auto_backup_saves()
# only runs at launch, so everything since launch was lost with it.
#
# This watches the slots while the game runs and keeps a copy of every healthy
# version it sees, so the worst a crash can cost is the single write that was
# in flight. repair_saves() already knows how to restore from these.
SAVE_GUARD_DIR = BACKUPS_DIR / "live"
SAVE_GUARD_KEEP = 12
SAVE_GUARD_POLL_SEC = 4
# Below this a slot is a husk, not a save; matches repair_saves' threshold.
SAVE_MIN_BYTES = 4096

save_guard_stop = threading.Event()


def _healthy_slot_files():
    if not USER_DATA.exists():
        return []
    out = []
    for prof in USER_DATA.glob("*/45410809"):
        for slot in prof.glob("00000001/*/*"):
            try:
                if slot.is_file() and slot.stat().st_size >= SAVE_MIN_BYTES:
                    out.append(slot)
            except OSError:
                pass
    return out


def _save_guard_loop():
    """Snapshot each slot whenever its contents change to a healthy value."""
    seen = {}
    while not save_guard_stop.is_set():
        try:
            for slot in _healthy_slot_files():
                data = slot.read_bytes()
                digest = hashlib.sha256(data).hexdigest()
                if seen.get(slot.name) == digest:
                    continue
                seen[slot.name] = digest
                SAVE_GUARD_DIR.mkdir(parents=True, exist_ok=True)
                stamp = time.strftime("%Y%m%d-%H%M%S")
                (SAVE_GUARD_DIR / f"{slot.name}-{stamp}.bin").write_bytes(data)
                header = (slot.parents[2] / "Headers" / "00000001" /
                          f"{slot.name}.header")
                if header.is_file():
                    (SAVE_GUARD_DIR / f"{slot.name}-{stamp}.header").write_bytes(
                        header.read_bytes())
                # Keep the newest few per slot; these are ~115 KB each.
                snaps = sorted(SAVE_GUARD_DIR.glob(f"{slot.name}-*.bin"))
                for old in snaps[:-SAVE_GUARD_KEEP]:
                    old.unlink(missing_ok=True)
                    old.with_suffix(".header").unlink(missing_ok=True)
        except Exception:
            pass
        save_guard_stop.wait(SAVE_GUARD_POLL_SEC)


def start_save_guard():
    save_guard_stop.clear()
    t = threading.Thread(target=_save_guard_loop, daemon=True)
    t.start()
    return t


def stop_save_guard():
    save_guard_stop.set()


def restore_slot_from_guard(slot_name):
    """Newest healthy snapshot for a slot, or None."""
    snaps = sorted(SAVE_GUARD_DIR.glob(f"{slot_name}-*.bin")) if SAVE_GUARD_DIR.is_dir() else []
    return snaps[-1] if snaps else None


def _watch_game_exit(proc, diag=None):
    """Record how the game ended; a GPU hang / kill mid-load can leave a corrupt
    shader cache behind, which then hangs the GPU on every later load. Purge the
    cache automatically after any abnormal exit."""
    code = proc.wait()
    stop_save_guard()  # game is gone; nothing left to snapshot
    notes = []
    try:
        (LAUNCHER_DIR / "last_exit.txt").write_text(str(code))
        if code != 0:
            good = BACKUPS_DIR / "goodcache"
            if good.is_dir():
                shutil.rmtree(USER_DATA / "cache", ignore_errors=True)
                shutil.copytree(good, USER_DATA / "cache")
                install_state["log"].append(
                    f"Game exited abnormally (code {code}) - known-good shader cache restored.")
                notes.append("known-good shader cache restored after the abnormal exit")
    except Exception:
        pass
    if diag:
        try:
            diag.finalize(code, notes)
        except Exception:
            pass


def launch_game():
    global game_proc
    with game_proc_lock:
        if game_running_pids():
            return False, "Game is already running"
        retire_instant_popin_patch()
        if not GAMEDATA.is_dir():
            return False, ("No game data installed yet — use the Install tab to install "
                           "from your Xbox 360 ISO.")
        if _find_ci(GAMEDATA, "default.xex") is None:
            return False, ("Game data is incomplete: default.xex is missing from "
                           f"{GAMEDATA}. Re-run the install from your ISO in the "
                           "Install tab.")
        if not (GAMEDATA / "movies").is_dir():
            return False, ("Game data is incomplete: the movies folder is missing from "
                           f"{GAMEDATA}. Re-run the install from your ISO in the "
                           "Install tab.")
        if (installed_build_variant() or VARIANT_DEFAULT) == VARIANT_DEFAULT and \
                (FROZEN or installed_build_variant()) and not cpu_runs_default_build():
            return False, ("This build needs a CPU with AVX2 (Intel from 2013 or AMD from "
                           "2015 on). Open the About tab and check for updates to install "
                           "the build for older CPUs, or download the NoAVX2 package from "
                           "the Releases page.")
        repaired = repair_saves()
        if repaired:
            install_state["log"].append("Save self-heal: " + ", ".join(repaired))
        auto_backup_saves()
        # Re-sync the managed settings block so default migrations apply on
        # launch, not only when the player next touches a setting.
        write_settings({})
        if PLAT == "Windows":
            # The game exe links the Microsoft C++ runtime. Without it, Windows
            # kills the process at startup with the famously unhelpful
            # 0xc0000142 dialog - catch that here and say what to install.
            import ctypes
            missing = []
            for dll in ("vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"):
                try:
                    ctypes.WinDLL(dll)
                except OSError:
                    missing.append(dll)
            if missing:
                return False, ("Windows is missing the Microsoft Visual C++ runtime "
                               f"({', '.join(missing)}), so the game would fail to start "
                               "with error 0xc0000142. Install the x64 redistributable from "
                               "https://aka.ms/vs/17/release/vc_redist.x64.exe and press "
                               "Play again.")
            runtime_dll = BUILD_DIR / "rexruntimerd.dll"
            if FROZEN and not runtime_dll.exists():
                return False, (f"rexruntimerd.dll is missing from {BUILD_DIR} - the game "
                               "cannot start without it. If an antivirus quarantined it, "
                               "restore it or re-extract the release archive.")
        env = os.environ.copy()
        env_notes = []
        # Lossless Scaling frame-gen (lsfg-vk) hooks Vulkan via a RenderDoc-style
        # capture layer that deadlocks this game's FSI render path (GPU hang,
        # "device lost" on level load -- the 2026-07-05 crash saga). Hard-disable
        # it for the game regardless of the user's global LS settings.
        env["DISABLE_LSFG"] = "1"
        env_notes.append("DISABLE_LSFG=1 (Lossless Scaling frame-gen disabled for stability)")
        # Experiments survive stale launcher backends: extra env is read from
        # launcher-env.json at every PLAY press, not baked into this process.
        env_file = LAUNCHER_DIR / "launcher-env.json"
        if env_file.exists():
            try:
                overrides = json.loads(env_file.read_text())
                for k, v in overrides.items():
                    env[str(k)] = str(v)
                if overrides:
                    env_notes.append("launcher-env.json overrides: " + ", ".join(map(str, overrides)))
            except Exception:
                pass
        libs = [str(_resolve(d)) for d in CONFIG["lib_dirs"].get(PLAT, [])]
        if libs:
            env["LD_LIBRARY_PATH"] = ":".join(libs) + ":" + env.get("LD_LIBRARY_PATH", "")
        # Pass the user data root explicitly: the engine derives it from
        # XDG_DATA_HOME independently, so without this the launcher and the
        # game can disagree about where saves live.
        cmd = [str(GAME_BIN), "--game_data_root", str(GAMEDATA),
               "--user_data_root", str(USER_DATA)]
        debug_gdb = LAUNCHER_DIR / "debug_run.gdb"
        crash_log = LAUNCHER_DIR / "last_run_debug.txt"
        if debug_gdb.exists() and shutil.which("gdb"):
            # temporary diagnostics mode: capture a backtrace if the game crashes
            cmd = ["gdb", "-batch", "-x", str(debug_gdb), "--args"] + cmd
        diag = DiagSession() if diagnostics_enabled() else None
        if diag:
            diag.write_header(cmd, env_notes)
        if debug_gdb.exists():
            out = open(crash_log, "w")
            if diag:
                with open(diag.log_path, "a", encoding="utf-8") as f:
                    f.write("(gdb diagnostics mode: game output captured in last_run_debug.txt)\n")
        elif diag:
            out = diag.game_output_handle()
        else:
            out = subprocess.DEVNULL
        try:
            game_proc = subprocess.Popen(cmd, cwd=str(BUILD_DIR), env=env,
                                         stdout=out, stderr=subprocess.STDOUT
                                         if out is not subprocess.DEVNULL else subprocess.DEVNULL,
                                         **POPEN_NO_WINDOW)
        except OSError as exc:
            message = f"Could not start the game: {exc}"
            install_state["log"].append(message)
            if diag:
                diag.finalize(None, [message])
            return False, message
        finally:
            if out is not subprocess.DEVNULL:
                out.close()  # the child holds its own duplicate of the fd
        start_save_guard()
        # HAND PATCH: level-load memory spikes were getting the game SIGKILLed
        # by systemd-oomd. This used to be handled by wrapping the launch in
        # `systemd-run --user --scope`, which hands the process off into a
        # separate transient systemd unit -- that reparents it away from
        # this process, which broke Steam/gamescope's tracking of the game
        # window in Gaming Mode (window would open but never get focused/
        # composited -- "black screen" when switching to it). Writing
        # oom_score_adj directly protects against the kernel OOM killer AND
        # systemd-oomd equally well, with no extra process/cgroup hop and no
        # reparenting -- the game stays a normal direct child.
        try:
            (Path(f"/proc/{game_proc.pid}/oom_score_adj")).write_text("-1000")
        except OSError:
            pass
        if diag:
            threading.Thread(target=diag.monitor, args=(game_proc,), daemon=True).start()
        threading.Thread(target=_watch_game_exit, args=(game_proc, diag), daemon=True).start()
        return True, "launched"


def stop_game():
    global game_proc
    with game_proc_lock:
        if game_proc and game_proc.poll() is None:
            game_proc.kill()
            game_proc = None
            return True, "stopped"
        if not game_running_pids():
            return True, "stopped"
    return False, "not started by launcher"


def add_to_steam():
    script = LAUNCHER_DIR / "simpsons-launcher.sh"
    tool = shutil.which("steamos-add-to-steam")
    if tool:
        subprocess.Popen([tool, str(script)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True, "Sent to Steam — check your library (may need a Steam restart)."
    if shutil.which("steam"):
        subprocess.Popen(["steam", f"steam://addnonsteamgame/{urllib.parse.quote(str(script))}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        return True, "Asked Steam to add the launcher — confirm the dialog in Steam."
    return False, ("Steam tools not found. Add manually: Steam → Games → "
                   f"Add a Non-Steam Game → browse to {script}")


def tail_game_log(n=60):
    logs = sorted((BUILD_DIR / "logs").glob("simpsons_*.log"),
                  key=lambda p: p.stat().st_mtime, reverse=True)
    if not logs:
        return []
    try:
        return logs[0].read_text(errors="replace").splitlines()[-n:]
    except Exception:
        return []


# ------------------------------------------------------------------ http

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        data = body if isinstance(body, bytes) else json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _addressed_here(self):
        # Other web pages can't read this server's answers, except one whose
        # domain name has been pointed at 127.0.0.1 (DNS rebinding). Its
        # requests still carry that domain in the Host header, so they're refused.
        port = self.server.server_address[1]
        return self.headers.get("Host", "").lower() in (f"127.0.0.1:{port}", f"localhost:{port}")

    def _token_ok(self):
        return secrets.compare_digest(self.headers.get("X-Token", "").encode(), TOKEN.encode())

    def do_GET(self):
        if not self._addressed_here():
            return self._send(403, {"error": "forbidden"})
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        if path in ("/", "/index.html"):
            html = (UI_DIR / "index.html").read_text(encoding="utf-8").replace("__TOKEN__", TOKEN)
            html = html.replace("__THEME__", CONFIG.get("ui_theme", "system"))
            return self._send(200, html.encode(), "text/html; charset=utf-8")
        if path == "/api/ping":
            return self._send(200, {"app": "simpsons-launcher", "version": VERSION})
        # The rest of the API answers only the launcher's own page, which has
        # the token. /api/browse lists folders on this computer.
        if path.startswith("/api/") and not self._token_ok():
            return self._send(403, {"error": "bad token"})
        if path == "/api/status":
            return self._send(200, status())
        if path == "/api/settings":
            return self._send(200, {"values": read_settings(),
                                    "defaults": {k: v[1] for k, v in SETTINGS_SCHEMA.items()},
                                    "restart_needed": [k for k, v in SETTINGS_SCHEMA.items() if v[2]]})
        if path == "/api/log":
            return self._send(200, {"lines": tail_game_log()})
        if path == "/api/diagnostics":
            return self._send(200, diagnostics_report())
        if path == "/api/browse":
            q = urllib.parse.parse_qs(parsed.query)
            return self._send(200, browse((q.get("path") or [""])[0]))
        # Art and fonts are served by exact name from their folder's own
        # listing, so a request can't name a file anywhere else.
        if path.startswith("/art/"):
            name = path[len("/art/"):]
            for f in ART_DIR.glob("hero*.jpg"):
                if f.name == name:
                    return self._send(200, f.read_bytes(), "image/jpeg")
        if path == "/icon.png":
            f = LAUNCHER_DIR / "icon.png"
            if f.is_file():
                return self._send(200, f.read_bytes(), "image/png")
        if path.startswith("/fonts/"):
            name = path[len("/fonts/"):]
            for f in (UI_DIR / "fonts").glob("*.ttf"):
                if f.name == name:
                    return self._send(200, f.read_bytes(), "font/ttf")
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        if not self._addressed_here() or not self._token_ok():
            return self._send(403, {"error": "bad token"})
        path = urllib.parse.urlparse(self.path).path
        length = int(self.headers.get("Content-Length") or 0)
        body = json.loads(self.rfile.read(length) or b"{}")
        if path == "/api/launch":
            ok, msg = launch_game()
            return self._send(200 if ok else 409, {"ok": ok, "msg": msg})
        if path == "/api/stop":
            ok, msg = stop_game()
            return self._send(200, {"ok": ok, "msg": msg})
        if path == "/api/settings":
            return self._send(200, {"values": write_settings(body.get("values", {}))})
        if path == "/api/install":
            if install_state["running"]:
                return self._send(409, {"ok": False, "msg": "install already running"})
            threading.Thread(target=run_install, args=(body.get("iso_path", ""),),
                             daemon=True).start()
            return self._send(200, {"ok": True})
        if path == "/api/ui-theme":
            theme = body.get("theme")
            if theme in ("system", "light", "dark"):
                CONFIG["ui_theme"] = theme
                save_config()
            return self._send(200, {"ok": True, "theme": CONFIG.get("ui_theme", "system")})
        if path == "/api/regen-art":
            threading.Thread(target=generate_art, kwargs={"force": True}, daemon=True).start()
            return self._send(200, {"ok": True})
        if path == "/api/backup-saves":
            ok, msg = backup_saves()
            return self._send(200, {"ok": ok, "msg": msg})
        if path == "/api/restore-saves":
            ok, msg = restore_saves(body.get("name", ""))
            return self._send(200, {"ok": ok, "msg": msg})
        if path == "/api/add-to-steam":
            ok, msg = add_to_steam()
            return self._send(200, {"ok": ok, "msg": msg})
        if path == "/api/patch":
            if body.get("id") == "skip_intro":
                ok, msg = patch_skip_intro(bool(body.get("enable")))
                return self._send(200, {"ok": ok, "msg": msg})
            if body.get("id") == "start_episode":
                ok, msg = set_start_episode(str(body.get("value", "")))
                return self._send(200, {"ok": ok, "msg": msg})
            return self._send(404, {"ok": False, "msg": "unknown patch"})
        if path == "/api/diagnostics":
            return self._send(200, {"ok": True,
                                    "enabled": set_diagnostics(body.get("enable"))})
        if path == "/api/capture":
            ok, msg = set_capture(bool(body.get("enable")))
            return self._send(200, {"ok": ok, "msg": msg, "armed": capture_armed()})
        if path == "/api/support-bundle":
            ok, msg = create_support_bundle()
            return self._send(200, {"ok": ok, "msg": msg})
        if path == "/api/check-updates":
            threading.Thread(target=check_updates, daemon=True).start()
            return self._send(200, {"ok": True})
        if path == "/api/apply-update":
            if update_state.get("applying"):
                return self._send(409, {"ok": False, "msg": "Update already in progress."})
            threading.Thread(target=apply_update, daemon=True).start()
            return self._send(200, {"ok": True, "msg": "Update started."})
        return self._send(404, {"error": "not found"})


# --------------------------------------------------------------- frontend

class LauncherServer(http.server.ThreadingHTTPServer):
    # The window's browser engine opens several connections at once, and
    # Windows refuses connections beyond the listen backlog outright (the
    # default here is 5) instead of letting them wait.
    request_queue_size = 64
    # SO_REUSEADDR lets a Windows socket bind a port another process is still
    # listening on, so a stuck earlier launcher could end up receiving this
    # one's connections (#37). There, take the port exclusively or move on.
    allow_reuse_address = PLAT != "Windows"

    def server_bind(self):
        if PLAT == "Windows" and hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


LOG_FILE = LAUNCHER_DIR / "launcher.log"


def startup_log(message):
    """Appends a line to launcher.log (started fresh on every launch), which
    shows where a launcher that won't open got stuck."""
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n")
    except OSError:
        pass


def wait_for_server(url, seconds=10.0):
    """True once the launcher's own server answers at url."""
    deadline = time.time() + seconds
    last_error = None
    while time.time() < deadline:
        try:
            # No proxy: a system proxy must not get between the window and
            # the launcher on this computer.
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(url + "api/ping", timeout=2) as r:
                if json.loads(r.read()).get("app") == "simpsons-launcher":
                    return True
        except Exception as e:  # noqa: BLE001
            last_error = e
        time.sleep(0.2)
    startup_log(f"the launcher's server at {url} did not answer: {last_error!r}")
    return False


def error_page(url):
    return f"""<html><body style="font-family:sans-serif;background:#fff6d5;color:#222;padding:40px">
<h2>The launcher couldn't show its window</h2>
<p>Its built-in server at <code>{url}</code> isn't answering. Security software that blocks
programs from talking to this computer (127.0.0.1) can cause this.</p>
<p>Try closing every copy of the launcher and starting it again, or open
<a href="{url}">{url}</a> in your web browser. <code>launcher.log</code> next to the launcher
says where it got stuck; please attach it to a report on GitHub.</p></body></html>"""


def run_native(url):
    """Native desktop window via PySide6 QWebEngineView."""
    # HAND PATCH: a Steam shortcut that needs a compatibility tool (e.g.
    # Steam Linux Runtime) to launch runs this inside a sandboxed container
    # that can't see the host's ~/.local Python packages, so a normal
    # `pip install --user PySide6` isn't visible and this silently fell back
    # to a browser tab. Vendoring a trimmed PySide6+QtWebEngine directly
    # under launcher/vendor/ (not gitignored intentionally -- see .gitignore
    # comment) means it travels with the code regardless of sandbox.
    vendor_dir = LAUNCHER_DIR / "vendor"
    if vendor_dir.is_dir() and str(vendor_dir) not in sys.path:
        sys.path.insert(0, str(vendor_dir))

    from PySide6.QtCore import QTimer, QUrl
    from PySide6.QtGui import QIcon
    from PySide6.QtWebEngineWidgets import QWebEngineView
    from PySide6.QtWidgets import QApplication, QMainWindow

    app = QApplication(sys.argv)
    win = QMainWindow()
    win.setWindowTitle("The Simpsons Game — Recompiled")
    icon = LAUNCHER_DIR / "icon.png"
    if icon.exists():
        win.setWindowIcon(QIcon(str(icon)))
    view = QWebEngineView()
    # A page that fails to load (the server still starting, a connection
    # refused) is retried before giving up with an explanation instead of the
    # browser engine's bare "site can't be reached" page (#37).
    retries = {"left": 20}

    def loaded(ok):
        if ok:
            return
        if retries["left"] > 0:
            retries["left"] -= 1
            QTimer.singleShot(500, lambda: view.setUrl(QUrl(url)))
        else:
            startup_log(f"the window could not load {url}")
            view.setHtml(error_page(url))

    view.loadFinished.connect(loaded)
    view.setUrl(QUrl(url))
    win.setCentralWidget(view)
    win.resize(1180, 800)
    win.show()
    app.exec()


def open_browser(url):
    # App-mode chromium first (nicest window), then any regular browser,
    # then flatpak browsers (the Steam Deck ships Firefox as a flatpak and
    # nothing else), then the OS default handler. Report success so the
    # caller can tell the player to open the URL by hand as a last resort.
    for browser in ("chromium", "chromium-browser", "google-chrome-stable", "brave"):
        exe = shutil.which(browser)
        if exe:
            subprocess.Popen([exe, f"--app={url}", "--window-size=1180,800"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return True
    for browser in ("firefox", "brave-browser", "vivaldi", "epiphany"):
        exe = shutil.which(browser)
        if exe:
            subprocess.Popen([exe, url],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            return True
    if shutil.which("flatpak"):
        for app_id in ("org.mozilla.firefox", "org.chromium.Chromium",
                       "com.brave.Browser", "com.google.Chrome"):
            r = subprocess.run(["flatpak", "info", app_id],
                               capture_output=True, **POPEN_NO_WINDOW)
            if r.returncode == 0:
                subprocess.Popen(["flatpak", "run", app_id, url],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                return True
    import webbrowser
    return webbrowser.open(url)


def main():
    if "--play" in sys.argv:
        # Start the game straight away without the launcher window, e.g. from
        # a Steam shortcut in Game Mode (#18, #20). Settings, save protection
        # and the environment are handled exactly as by the Play button.
        ok, message = launch_game()
        if not ok:
            print(message)
            sys.exit(1)
        if game_proc:
            game_proc.wait()
        return
    try:
        LOG_FILE.write_text("", encoding="utf-8")
    except OSError:
        pass
    startup_log(f"launcher {VERSION} on {PLAT} {platform.release()}, frozen={FROZEN}")
    # In the background: a full artwork sweep can take a minute on a slow
    # disk, and players were staring at nothing until it finished.
    threading.Thread(target=generate_art, daemon=True).start()
    try:
        install_desktop_entry()
    except Exception as e:  # noqa: BLE001
        startup_log(f"desktop entry: {e!r}")
    server = None
    for cand in range(PORT, PORT + 20):
        try:
            server = LauncherServer(("127.0.0.1", cand), Handler)
            break
        except OSError as e:
            startup_log(f"port {cand} unavailable: {e}")
            continue
    if server is None:
        server = LauncherServer(("127.0.0.1", 0), Handler)
    port = server.server_address[1]
    url = f"http://127.0.0.1:{port}/"
    print(f"Simpsons Launcher v{VERSION}: {url}")
    startup_log(f"serving {url}")

    def serve():
        try:
            server.serve_forever()
        except Exception as e:  # noqa: BLE001
            startup_log(f"server stopped: {e!r}")

    threading.Thread(target=serve, daemon=True).start()
    threading.Thread(target=check_updates, daemon=True).start()
    if wait_for_server(url):
        startup_log("server answers")

    if "--no-browser" in sys.argv:
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            return
    try:
        run_native(url)          # real app window
    except Exception:       # noqa: BLE001
        import traceback
        tb = traceback.format_exc()
        # The full traceback goes to a log file only: printing it to the
        # terminal made the graceful fallback look like a crash (issue #9).
        try:
            (LAUNCHER_DIR / "launcher_native_error.log").write_text(tb)
        except Exception:
            pass
        startup_log("native window unavailable, opening a browser (launcher_native_error.log)")
        print("(native window unavailable; opening in your browser instead — "
              "details in launcher_native_error.log)")
        opened = False
        try:
            opened = open_browser(url)
        except Exception:
            opened = False
        if not opened:
            print(f"Could not open a browser automatically.\n"
                  f"Open this address in any browser to use the launcher: {url}")
        try:
            while True:
                time.sleep(3600)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
