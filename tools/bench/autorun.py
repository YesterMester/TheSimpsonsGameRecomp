#!/usr/bin/env python3
"""Unattended game runs for renderer testing and profiling.

Boots the game from an isolated directory (its own copy of the config, logs and
save data, so a test run can never touch the player's settings or saves),
drives it with a timed script of keyboard input (the engine's keyboard
controller emulation), and collects screenshots, the engine log, the per-frame
perf CSV and optionally a `perf` profile.

Run it on the host, not inside a Flatpak sandbox:

    python3 tools/bench/autorun.py <name> <script> [--set key=value ...]

Script format, one step per line (# comments allowed):

    wait <seconds>                    sleep
    waitlog <regex> [timeout]         block until the engine log matches
    key <name> [hold_seconds]         press and release (xdotool key names)
    pad <control> [hold_seconds]      injected controller input (A B X Y START BACK
                                      LB RB LT RT UP DOWN LEFT RIGHT L_UP L_DOWN
                                      L_LEFT L_RIGHT R_UP ...), no window focus needed
    shot <label>                      window screenshot -> <run>/<label>.png
    perf <seconds> [label]            perf record the game for N seconds, sampling
                                      GPU busy/clock/power alongside
    gpu <seconds> [label]             GPU busy/clock/power samples only
    audio <seconds> [label]           record the default output device and report
                                      RMS/peak level (run with --set audio_mute=false)
    mark <text>                       note in the run summary

--lib-dir DIR puts DIR first on the library path, so an A/B run can load a
different librexruntimerd.so without rebuilding the game.

Key names follow xdotool: space (A), Escape (Start), Up/Down/Left/Right
(D-pad), Shift_L (B), Tab (Back), w/a/s/d (left stick).
"""

import argparse
import glob
import statistics
import threading
import os
import re
import shutil
import signal
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUILD_DIR = os.path.join(ROOT, "simpsons", "out", "build", "linux-amd64-relwithdebinfo")
LIB_DIR = os.path.join(ROOT, "tools", "rexglue-bin", "linux-amd64", "lib")
GAMEDATA = os.path.join(ROOT, "gamedata")
TEST_ROOT = os.path.expanduser("~/simpsons-test")
REAL_USER_DATA = os.path.expanduser("~/.local/share/simpsons")
GPU_SYSFS = "/sys/class/drm/card0/device"
LIB_OVERRIDE = None
# Game executable to run instead of the regular build (--exe).
EXE_OVERRIDE = None
# When set, the game's audio goes to this PipeWire null sink instead of the
# speakers, and the audio step records its monitor.
TEST_SINK = None

# Settings every test run needs regardless of the player's config.
BASE_OVERRIDES = {
    "fullscreen": "false",
    "window_width": "1280",
    "window_height": "720",
    "audio_mute": "true",
    "mnk_mode": "false",  # injected pad input needs no keyboard/mouse driver; it would grab the pointer
    "draw_telemetry": "false",
    "native_telemetry": "120",
    "shader_inventory_csv": '""',
    "pipeline_inventory_json": '""',
}


def toml_value(v):
    v = v.strip()
    if re.fullmatch(r"-?\d+(\.\d+)?", v) or v in ("true", "false") or v.startswith('"'):
        return v
    return '"' + v + '"'


def write_config(run_dir, overrides):
    src = os.path.join(BUILD_DIR, "simpsons.toml")
    lines = open(src).read().splitlines() if os.path.exists(src) else []
    keys = set(overrides)
    out = []
    for line in lines:
        m = re.match(r"\s*([A-Za-z0-9_]+)\s*=", line)
        if m and m.group(1) in keys:
            continue
        out.append(line)
    out.append("")
    out.append("# >>> autorun overrides")
    for k, v in overrides.items():
        out.append(f"{k} = {toml_value(v)}")
    open(os.path.join(run_dir, "simpsons.toml"), "w").write("\n".join(out) + "\n")


def prepare(name, overrides):
    run_dir = os.path.join(TEST_ROOT, name)
    if os.path.isdir(run_dir):
        shutil.rmtree(run_dir)
    os.makedirs(run_dir)
    exe = os.path.join(run_dir, "simpsons")
    source_exe = EXE_OVERRIDE or os.path.join(BUILD_DIR, "simpsons")
    try:
        os.link(source_exe, exe)
    except OSError:
        shutil.copy2(source_exe, exe)
    user_data = os.path.join(TEST_ROOT, "userdata")
    if not os.path.isdir(user_data):
        shutil.copytree(REAL_USER_DATA, user_data, symlinks=True)
    ov = dict(BASE_OVERRIDES)
    ov["perf_log_csv"] = os.path.join(run_dir, "perf.csv")
    ov["trace_gpu_prefix"] = os.path.join(run_dir, "trace")
    ov["input_inject_file"] = os.path.join(run_dir, "input.cmd")
    ov["user_data_root"] = user_data
    ov.update(overrides)
    write_config(run_dir, ov)
    return run_dir, exe, user_data


class Run:
    def __init__(self, run_dir, exe, user_data):
        self.run_dir = run_dir
        self.exe = exe
        self.user_data = user_data
        self.proc = None
        self.log_path = None
        self.wid = None
        self.notes = []
        self.t0 = None

    def env(self):
        env = dict(os.environ)
        env["DISABLE_LSFG"] = "1"
        libs = [LIB_OVERRIDE] if LIB_OVERRIDE else []
        env["LD_LIBRARY_PATH"] = ":".join(libs + [LIB_DIR, env.get("LD_LIBRARY_PATH", "")])
        env.setdefault("DISPLAY", ":0")
        return env

    def start(self):
        logs_before = set(glob.glob(os.path.join(self.run_dir, "logs", "*.log")))
        out = open(os.path.join(self.run_dir, "stdout.txt"), "w")
        self.proc = subprocess.Popen(
            [self.exe, "--game_data_root", GAMEDATA, "--user_data_root", self.user_data],
            cwd=self.run_dir, env=self.env(), stdout=out, stderr=subprocess.STDOUT)
        self.t0 = time.time()
        if TEST_SINK:
            threading.Thread(target=self.route_audio, daemon=True).start()
        for _ in range(100):
            logs = set(glob.glob(os.path.join(self.run_dir, "logs", "*.log"))) - logs_before
            if logs:
                self.log_path = max(logs, key=os.path.getmtime)
                break
            time.sleep(0.1)
        self.note(f"pid {self.proc.pid} log {self.log_path}")

    def route_audio(self):
        """Keep every audio stream of the game on the test sink, not the speakers."""
        import json
        moved = set()
        while self.alive():
            r = subprocess.run(["pactl", "-f", "json", "list", "sink-inputs"],
                               capture_output=True, text=True, env=self.env())
            clients = subprocess.run(["pactl", "-f", "json", "list", "clients"],
                                     capture_output=True, text=True, env=self.env())
            try:
                inputs = json.loads(r.stdout or "[]")
                client_ids = {str(c["index"]) for c in json.loads(clients.stdout or "[]")
                              if str(c.get("properties", {}).get("application.process.id"))
                              == str(self.proc.pid)}
            except (ValueError, KeyError):
                inputs, client_ids = [], set()
            for si in inputs:
                props = si.get("properties", {})
                # Native PipeWire streams may put the process id only on the
                # client. Match both layouts and the older SDL app name.
                if (str(props.get("application.process.id")) != str(self.proc.pid)
                        and str(si.get("client")) not in client_ids
                        and props.get("application.name") != "rexglue"):
                    continue
                if si.get("index") in moved:
                    continue
                subprocess.run(["pactl", "move-sink-input", str(si["index"]), TEST_SINK],
                               capture_output=True, env=self.env())
                moved.add(si.get("index"))
                self.note(f"audio stream {si.get('index')} routed to {TEST_SINK}")
            time.sleep(0.05)

    def note(self, text):
        stamp = f"[{time.time() - self.t0:7.1f}s] " if self.t0 else ""
        line = stamp + text
        self.notes.append(line)
        print(line, flush=True)

    def alive(self):
        return self.proc and self.proc.poll() is None

    def geometry(self, w):
        g = subprocess.run(["xdotool", "getwindowgeometry", "--shell", w],
                           capture_output=True, text=True, env=self.env()).stdout
        return dict(l.split("=", 1) for l in g.split() if "=" in l)

    def window(self):
        # The game owns several X windows (SDL helpers are tiny); the real one
        # is the largest.
        if self.wid:
            return self.wid
        for _ in range(50):
            r = subprocess.run(["xdotool", "search", "--pid", str(self.proc.pid)],
                               capture_output=True, text=True, env=self.env())
            best, best_area = None, 0
            for w in r.stdout.split():
                g = self.geometry(w)
                area = int(g.get("WIDTH", 0)) * int(g.get("HEIGHT", 0))
                if area > best_area:
                    best, best_area = w, area
            if best and best_area >= 320 * 200:
                self.wid = best
                return best
            time.sleep(0.2)
        return None

    def key(self, name, hold):
        # Sent to the game window directly, so it lands there even when the
        # window manager refuses to hand focus to a background window.
        w = self.window()
        target = ["--window", w] if w else []
        subprocess.run(["xdotool", "keydown"] + target + [name], env=self.env())
        time.sleep(hold)
        subprocess.run(["xdotool", "keyup"] + target + [name], env=self.env())

    def pad(self, control, hold, wait_for_release=True):
        path = os.path.join(self.run_dir, "input.cmd")
        deadline = time.time() + 5
        while os.path.exists(path) and time.time() < deadline:
            time.sleep(0.05)
        with open(path + ".tmp", "w") as f:
            f.write(f"{control} {hold}\n")
        os.replace(path + ".tmp", path)
        deadline = time.time() + 5
        while os.path.exists(path) and time.time() < deadline and self.alive():
            time.sleep(0.02)
        if os.path.exists(path):
            self.note(f"pad {control}: not consumed")
        if wait_for_release:
            time.sleep(hold + 0.05)

    def shot(self, label):
        """Screenshot of the presented guest output via the engine's trigger file."""
        prefix = os.path.join(self.run_dir, "trace")
        before = set(glob.glob(prefix + "_shot_*.ppm"))
        open(prefix + ".shot", "w").close()
        deadline = time.time() + 10
        while time.time() < deadline and self.alive():
            new = set(glob.glob(prefix + "_shot_*.ppm")) - before
            if new:
                ppm = new.pop()
                time.sleep(0.3)
                png = os.path.join(self.run_dir, label + ".png")
                subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", ppm, png])
                os.remove(ppm)
                self.note(f"shot {png}")
                return
            time.sleep(0.2)
        self.note(f"shot {label}: no image")

    def shots(self, count, interval, label):
        """A timed series of screenshots, e.g. to see when things appear."""
        start = time.time()
        for i in range(count):
            if not self.alive():
                return
            self.shot(f"{label}_{i:02d}")
            next_time = start + (i + 1) * interval
            time.sleep(max(0.0, next_time - time.time()))

    def trace(self, label):
        """Capture the next frame's GPU trace via the engine's trigger file."""
        trace_dir = os.path.join(self.run_dir, "trace")
        before = set(glob.glob(os.path.join(trace_dir, "*.xtr")))
        open(trace_dir + ".request", "w").close()
        deadline = time.time() + 60
        while time.time() < deadline and self.alive():
            new = set(glob.glob(os.path.join(trace_dir, "*.xtr"))) - before
            if new:
                path = new.pop()
                last_size = -1
                size = 0
                while time.time() < deadline:
                    size = os.path.getsize(path)
                    if size and size == last_size:
                        break
                    last_size = size
                    time.sleep(0.5)
                dest = os.path.join(trace_dir, label + ".xtr")
                os.rename(path, dest)
                self.note(f"trace {label}: {dest} ({size >> 20} MB)")
                return
            time.sleep(0.2)
        self.note(f"trace {label}: no trace written")

    def gpu(self, seconds, label, stats=None):
        """Sample GPU busy %, shader clock and package power at 10 Hz."""
        busy, sclk, power = [], [], []
        deadline = time.time() + seconds
        power_files = glob.glob(os.path.join(GPU_SYSFS, "hwmon", "hwmon*", "power1_average"))
        while time.time() < deadline:
            try:
                busy.append(int(open(os.path.join(GPU_SYSFS, "gpu_busy_percent")).read()))
                for line in open(os.path.join(GPU_SYSFS, "pp_dpm_sclk")):
                    if "*" in line:
                        sclk.append(int(re.search(r"(\d+)Mhz", line).group(1)))
                if power_files:
                    power.append(int(open(power_files[0]).read()) / 1e6)
            except (OSError, AttributeError, ValueError):
                pass
            time.sleep(0.1)
        def med(v):
            return statistics.median(v) if v else float("nan")
        result = {"busy": med(busy), "sclk": med(sclk), "power": med(power)}
        if stats is not None:
            stats.update(result)
        self.note(f"gpu {label}: busy {result['busy']:.0f}% sclk {result['sclk']:.0f} MHz "
                  f"power {result['power']:.2f} W")
        return result

    def audio(self, seconds, label):
        """Record the default sink's monitor and report loudness (0 = silence)."""
        import array
        sink = TEST_SINK or subprocess.run(["pactl", "get-default-sink"], capture_output=True,
                                           text=True, env=self.env()).stdout.strip()
        raw = os.path.join(self.run_dir, f"audio_{label}.raw")
        proc = subprocess.Popen(["parecord", "--device", sink + ".monitor", "--raw",
                                 "--format=s16le", "--rate=48000", "--channels=2", raw],
                                env=self.env(), stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        time.sleep(seconds)
        proc.terminate()
        proc.wait()
        samples = array.array("h")
        with open(raw, "rb") as f:
            data = f.read()
        samples.frombytes(data[: len(data) // 2 * 2])
        if not samples:
            self.note(f"audio {label}: no samples recorded")
            return
        rms = (sum(x * x for x in samples) / len(samples)) ** 0.5
        peak = max(abs(x) for x in samples)
        frames = len(samples) // 2
        gaps = run = 0
        for i in range(frames):
            if abs(samples[2 * i]) < 8 and abs(samples[2 * i + 1]) < 8:
                run += 1
            else:
                if run >= 96:
                    gaps += 1
                run = 0
        self.note(f"audio {label}: {frames / 48000:.1f}s rms {rms:.0f} peak {peak} "
                  f"quiet gaps(>=2ms)={gaps} ({'SILENT' if peak < 50 else 'sound present'})")

    def fps_since(self, t_start):
        """Mean of the engine's [native] fps lines logged after t_start."""
        vals = []
        try:
            for line in open(self.log_path, errors="replace"):
                m = re.search(r"^\[(\d+-\d+-\d+ \d+:\d+:\d+)\.\d+\].*\(([0-9.]+) fps\)", line)
                if m and time.mktime(time.strptime(m.group(1), "%Y-%m-%d %H:%M:%S")) >= t_start:
                    vals.append(float(m.group(2)))
        except (OSError, TypeError):
            pass
        return vals

    def perf(self, seconds, label):
        path = os.path.join(self.run_dir, f"perf_{label}.data")
        stats = {}
        sampler = threading.Thread(target=self.gpu, args=(seconds, label, stats))
        t_start = time.time()
        sampler.start()
        subprocess.run(["perf", "record", "-F", "997", "-g", "--call-graph", "fp",
                        "-p", str(self.proc.pid), "-o", path, "--", "sleep", str(seconds)],
                       env=self.env(), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        sampler.join()
        fps = self.fps_since(t_start)
        fps_text = f"{statistics.mean(fps):.1f} fps over {len(fps)} samples" if fps else "no fps"
        self.note(f"perf {path} | {fps_text}")

    def waitlog(self, pattern, timeout):
        rx = re.compile(pattern)
        deadline = time.time() + timeout
        while time.time() < deadline and self.alive():
            try:
                with open(self.log_path, errors="replace") as f:
                    if rx.search(f.read()):
                        self.note(f"waitlog matched /{pattern}/")
                        return True
            except (OSError, TypeError):
                pass
            time.sleep(0.5)
        self.note(f"waitlog TIMEOUT /{pattern}/")
        return False

    def stop(self):
        if not self.alive():
            self.note(f"game already exited rc={self.proc.returncode}")
            return
        # A window-manager close request takes the same clean shutdown path as
        # the player closing the window; the engine does not act on SIGTERM.
        # windowquit asks the window manager to close the window; windowclose
        # destroys the X window out from under GTK, which then sometimes dies
        # on an X error (BadWindow from a cursor change) with exit code 1.
        w = self.window()
        if w:
            subprocess.run(["xdotool", "windowquit", w], env=self.env(), capture_output=True)
            try:
                self.proc.wait(timeout=60)
                self.note(f"closed cleanly rc={self.proc.returncode}")
                return
            except subprocess.TimeoutExpired:
                self.note("window close timed out")
        self.proc.send_signal(signal.SIGTERM)
        try:
            self.proc.wait(timeout=15)
            self.note(f"exited on SIGTERM rc={self.proc.returncode}")
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            self.note("killed")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("script")
    ap.add_argument("--set", action="append", default=[], help="config override key=value")
    ap.add_argument("--lib-dir", default=None)
    ap.add_argument("--exe", default=None, help="game executable instead of the regular build")
    ap.add_argument("--silent-audio", action="store_true",
                    help="route game audio to a null sink (recordable, not audible)")
    args = ap.parse_args()
    global LIB_OVERRIDE, TEST_SINK, EXE_OVERRIDE
    LIB_OVERRIDE = args.lib_dir
    EXE_OVERRIDE = args.exe
    sink_module = None
    if args.silent_audio:
        TEST_SINK = "simpsons_test_sink"
        sink_module = subprocess.run(
            ["pactl", "load-module", "module-null-sink", f"sink_name={TEST_SINK}",
             "sink_properties=device.description=SimpsonsTestSink"],
            capture_output=True, text=True).stdout.strip()
    overrides = {}
    for kv in args.set:
        k, v = kv.split("=", 1)
        overrides[k.strip()] = v
    run_dir, exe, user_data = prepare(args.name, overrides)
    run = Run(run_dir, exe, user_data)
    steps = [l.split("#", 1)[0].strip() for l in open(args.script)]
    steps = [s for s in steps if s]
    run.start()
    try:
        for step in steps:
            if not run.alive():
                run.note("game exited early")
                break
            op, *rest = step.split()
            if op == "wait":
                time.sleep(float(rest[0]))
            elif op == "waitlog":
                run.waitlog(rest[0], float(rest[1]) if len(rest) > 1 else 120)
            elif op == "key":
                run.key(rest[0], float(rest[1]) if len(rest) > 1 else 0.15)
            elif op == "pad":
                run.pad(rest[0], float(rest[1]) if len(rest) > 1 else 0.15)
            elif op == "padhold":
                # Like pad, but the script goes on while the button is held
                # (to take screenshots or a trace during the hold).
                run.pad(rest[0], float(rest[1]) if len(rest) > 1 else 0.15, False)
                run.note(f"holding {rest[0]}")
            elif op == "shot":
                run.shot(rest[0])
            elif op == "shots":
                run.shots(int(rest[0]), float(rest[1]), rest[2])
            elif op == "trace":
                run.trace(rest[0])
            elif op == "perf":
                run.perf(float(rest[0]), rest[1] if len(rest) > 1 else "profile")
            elif op == "audio":
                run.audio(float(rest[0]), rest[1] if len(rest) > 1 else "audio")
            elif op == "gpu":
                run.gpu(float(rest[0]), rest[1] if len(rest) > 1 else "gpu")
            elif op == "mark":
                run.note("MARK " + " ".join(rest))
            else:
                run.note(f"unknown step {step}")
    finally:
        run.stop()
        open(os.path.join(run_dir, "summary.txt"), "w").write("\n".join(run.notes) + "\n")
        if sink_module:
            subprocess.run(["pactl", "unload-module", sink_module], capture_output=True)


if __name__ == "__main__":
    main()
