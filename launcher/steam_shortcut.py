"""Read Steam's shortcut list without modifying a running client's files."""

from pathlib import Path
import os
import re
import shlex
import struct


def _binary_vdf(data):
    position = 0

    def string():
        nonlocal position
        end = data.index(0, position)
        value = data[position:end].decode("utf-8", errors="replace")
        position = end + 1
        return value

    def object_value(depth=0):
        nonlocal position
        if depth > 16:
            raise ValueError("Steam shortcut nesting is too deep")
        values = {}
        while position < len(data):
            kind = data[position]
            position += 1
            if kind == 8:
                return values
            key = string()
            if kind == 0:
                value = object_value(depth + 1)
            elif kind == 1:
                value = string()
            elif kind in (2, 3, 4, 6, 7, 10):
                size = 8 if kind in (7, 10) else 4
                if position + size > len(data):
                    raise ValueError("Truncated Steam shortcut value")
                value = struct.unpack_from("<Q" if size == 8 else "<I", data, position)[0]
                position += size
            else:
                raise ValueError("Unsupported Steam shortcut value")
            values[key] = value
        raise ValueError("Truncated Steam shortcut list")

    return object_value()


def steam_roots():
    home = Path.home()
    candidates = [home / ".steam/steam", home / ".steam/root",
                  home / ".local/share/Steam",
                  home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
    roots = []
    for candidate in candidates:
        root = candidate.resolve()
        if root.is_dir() and root not in roots:
            roots.append(root)
    return roots


def _current_shortcuts(root):
    # A shortcut in another person's account must not suppress adding one to
    # the selected account. Prefer the live client, then its last login.
    registry = root / "registry.vdf"
    if not registry.exists():
        registry = Path.home() / ".steam/registry.vdf"
    try:
        active = re.search(r'"ActiveUser"\s+"([0-9]+)"', registry.read_text())
        if active and int(active[1]):
            return root / "userdata" / active[1] / "config/shortcuts.vdf"
    except OSError:
        pass
    try:
        text = (root / "config/loginusers.vdf").read_text()
        users = re.findall(r'"([0-9]{17})"\s*\{([^{}]*)\}', text)
        if users:
            def priority(user):
                fields = dict(re.findall(r'"([^"\n]+)"\s+"([^"\n]*)"', user[1]))
                return (fields.get("MostRecent") == "1", int(fields.get("Timestamp", fields.get("timestamp", "0"))))
            account = int(max(users, key=priority)[0]) & 0xFFFFFFFF
            return root / "userdata" / str(account) / "config/shortcuts.vdf"
    except (OSError, ValueError):
        pass
    return None


def existing_shortcut(targets):
    targets = {os.path.realpath(str(path)) for path in targets}
    for root in steam_roots():
        file = _current_shortcuts(root)
        if not file:
            continue
        try:
            if file.stat().st_size > 8 * 1024 * 1024:
                continue
            entries = _binary_vdf(file.read_bytes()).get("shortcuts", {})
            for entry in entries.values():
                if not isinstance(entry, dict):
                    continue
                fields = {k.lower(): v for k, v in entry.items()}
                command = fields.get("exe", "") + " " + fields.get("launchoptions", "")
                for token in shlex.split(command):
                    if token.startswith("/") and os.path.realpath(token) in targets:
                        return fields.get("appname") or "The Simpsons Game (Recompiled)"
        except (OSError, ValueError, KeyError, AttributeError, TypeError):
            continue
    return None
