"""Shared launcher and creator-tool package handling. Standard library only."""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct
import tempfile
import uuid
import zipfile

API_VERSION = 1
ID = re.compile(r"[a-z0-9][a-z0-9_.-]{0,95}\Z")
VERSION = re.compile(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z")
TARGETS = {"windows-x64", "linux-x64", "android-arm64"}
MAX_FILES = 100000
MAX_BYTES = 8 * 1024**3


class ModError(ValueError):
    pass


def project_folder(name, mods):
    direct = Path(name)
    if direct.is_dir() and (direct / "mod.json").is_file():
        return direct.resolve()
    candidate = inside(mods, name)
    if (candidate / "mod.json").is_file():
        return candidate
    for item in installed(mods).values():
        if item["id"] == name or item["name"] == name:
            return Path(item["folder"])
    raise ModError("Cannot find mod project: " + name)


def inside(root: Path, relative: str) -> Path:
    if not isinstance(relative, str) or not relative or len(relative.encode("utf-8")) > 512 or \
            any(c in relative for c in '\\:<>"|?*') or any(ord(c) < 32 for c in relative):
        raise ModError("Use a relative path with forward slashes")
    parts = relative.split("/")
    if len(parts) > 64 or any(not part or part.endswith((".", " ")) for part in parts):
        raise ModError("A mod path has an invalid component")
    reserved = {"CON", "PRN", "AUX", "NUL"} | {f"{name}{i}" for name in ("COM", "LPT") for i in range(1, 10)}
    if any(part.split(".")[0].upper() in reserved for part in parts):
        raise ModError("A mod path uses a reserved Windows device name")
    p = PurePosixPath(relative)
    if p.is_absolute() or ".." in p.parts or "." in relative.split("/"):
        raise ModError("A mod path cannot be absolute or contain dot segments")
    root = root.resolve()
    result = root.joinpath(*p.parts).resolve()
    if result == root or not result.is_relative_to(root):
        raise ModError("A mod path escaped its folder")
    return result


def read_json(path: Path, maximum=4 * 1024**2):
    if path.stat().st_size > maximum:
        raise ModError("JSON file is too large: " + str(path))
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid.uuid4().hex + ".part")
    try:
        temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False, allow_nan=False) + "\n", encoding="utf-8")
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def version(text: str):
    if not isinstance(text, str) or not VERSION.fullmatch(text):
        raise ModError("Use major.minor.patch versions such as 1.0.0")
    values = tuple(map(int, text.split(".")))
    if any(v > 2**32 - 1 for v in values):
        raise ModError("Version component is too large")
    return values


def matches(value: str, requirement: str):
    v = version(value)
    if requirement == "*":
        return True
    if requirement.startswith("^"):
        low = version(requirement[1:])
        high = (low[0] + 1, 0, 0) if low[0] else ((0, low[1] + 1, 0) if low[1] else (0, 0, low[2] + 1))
        return low <= v < high
    return v == version(requirement)


def files(folder: Path):
    folder = folder.resolve()
    found = []
    seen = set()
    for directory, dirs, names in os.walk(folder, followlinks=False):
        base = Path(directory)
        for name in dirs + names:
            if (base / name).is_symlink():
                raise ModError("Mod packages cannot contain symbolic links")
        dirs[:] = sorted(d for d in dirs if not d.startswith(".") and d not in {"build", "__pycache__"})
        for name in names:
            p = base / name
            rel = p.relative_to(folder).as_posix()
            if rel == "checksums.json":
                continue
            inside(folder, rel)
            if not p.is_file():
                raise ModError("Mod packages must contain regular files")
            # Match the game's ASCII-insensitive resource names on every host.
            key = rel.translate(str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"))
            if key in seen:
                raise ModError("Mod paths differ only in letter case: " + rel)
            seen.add(key)
            found.append((rel, p))
    if len(found) > MAX_FILES or sum(p.stat().st_size for _, p in found) > MAX_BYTES:
        raise ModError("Mod package exceeds the file or size limit")
    return sorted(found, key=lambda item: item[0].encode("utf-8"))


def sha256(path: Path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def inventory(folder: Path):
    return {name: sha256(path) for name, path in files(folder)}


def fingerprint(folder: Path):
    return _fingerprint(inventory(folder))


def _fingerprint(content):
    h = hashlib.sha256()
    for name, digest in content.items():
        h.update((name + "\0" + digest + "\n").encode("utf-8"))
    return h.hexdigest()


def manifest(folder: Path, *, verify=True, contents=True):
    data = read_json(folder / "mod.json")
    if not isinstance(data, dict):
        raise ModError("mod.json must be an object")
    m = dict(data)
    name = m.get("name")
    if not isinstance(name, str) or not name or len(name.encode("utf-8")) > 160:
        raise ModError("A mod needs a name of at most 160 UTF-8 bytes")
    m.setdefault("id", "legacy." + hashlib.sha256(name.encode()).hexdigest()[:16])
    if not isinstance(m["id"], str) or not ID.fullmatch(m["id"]):
        raise ModError("Mod ids use lower-case letters, numbers, periods, underscores and hyphens")
    m.setdefault("version", "1.0.0")
    if m["version"] == "1.0" and "api" not in m:
        m["version"] = "1.0.0"
    version(m["version"])
    if type(m.get("api", API_VERSION)) is not int or m.get("api", API_VERSION) != API_VERSION:
        raise ModError("This mod needs a different Modkit API version")
    m.setdefault("kind", "data")
    if m["kind"] not in {"data", "lua", "native"}:
        raise ModError("Mod kind must be data, lua or native")
    m.setdefault("save_mode", "stateless" if m["kind"] == "data" else "checkpoint")
    if m["save_mode"] not in {"stateless", "state", "checkpoint"}:
        raise ModError("Invalid save_mode")
    for field in ("author", "description"):
        if not isinstance(m.get(field, ""), str):
            raise ModError(field + " must be text")
    for field in ("requires_operations", "requires_events"):
        required = m.get(field, [])
        if not isinstance(required, list) or len(required) > 128 or any(
                not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_.]{0,79}", name) for name in required) or len(set(required)) != len(required):
            raise ModError(field + " must contain at most 128 unique API names")
    if m["kind"] == "lua":
        entry = inside(folder, m.get("entry", ""))
        if contents and not entry.is_file():
            raise ModError("Lua entrypoint is missing")
    if not isinstance(m.get("dependencies", {}), dict):
        raise ModError("dependencies must map mod ids to versions")
    for dependency, requirement in m.get("dependencies", {}).items():
        if not ID.fullmatch(dependency) or not isinstance(requirement, str):
            raise ModError("Invalid dependency")
        matches("1.0.0", requirement)
    if not isinstance(m.get("conflicts", []), list) or any(not isinstance(x, str) or not ID.fullmatch(x) for x in m.get("conflicts", [])):
        raise ModError("conflicts must contain mod ids")
    if not isinstance(m.get("binaries", {}), dict):
        raise ModError("binaries must map platforms to files")
    for target, relative in m.get("binaries", {}).items():
        if target not in TARGETS:
            raise ModError("Unknown native target: " + target)
        if contents and not inside(folder, relative).is_file():
            raise ModError("Native binary is missing: " + relative)
    if contents and m["kind"] == "native" and not m.get("binaries"):
        raise ModError("A native mod needs at least one platform binary")
    if "scene" in m:
        try:
            if __package__:
                from . import mod_scenes
            else:
                import mod_scenes
            mod_scenes.validate(m["scene"])
        except (ValueError, TypeError, OverflowError) as error:
            raise ModError("Invalid scene: " + str(error)) from error
    if "mission" in m:
        try:
            if __package__:
                from . import mod_documents
            else:
                import mod_documents
            mod_documents.validate("mission", m["mission"])
        except (ValueError, TypeError, OverflowError) as error:
            raise ModError("Invalid mission: " + str(error)) from error
    if "model_edits" in m:
        inside(folder, m["model_edits"])
        if contents:
            model_variants(folder, m)
    if "mesh_edits" in m:
        inside(folder, m["mesh_edits"])
        if contents:
            mesh_variants(folder, m)
    if "texture_edits" in m:
        inside(folder, m["texture_edits"])
        if contents:
            texture_variants(folder, m)
    content = files(folder) if contents else []
    if any(name.split("/")[-1].lower() == "default.xex" for name, _ in content):
        raise ModError("A mod cannot contain the game's executable")
    actual = {name: sha256(path) for name, path in content} if contents else None
    checksums = folder / "checksums.json"
    if verify and contents and checksums.is_file() and read_json(checksums, 64 * 1024**2) != actual:
        raise ModError("Package checksums do not match its files")
    if contents:
        m["fingerprint"] = _fingerprint(actual)
    else:
        m.pop("fingerprint", None)
    m["folder"] = str(folder.resolve())
    return m


def model_variants(folder, manifest):
    """Validate portable variant documents; the native loader checks stock bytes."""
    if "model_edits" not in manifest:
        return []
    catalog = read_json(inside(folder, manifest["model_edits"]), 1024**2)
    if not isinstance(catalog, dict) or set(catalog) != {"version", "models"} or \
            type(catalog["version"]) is not int or catalog["version"] != 1 or \
            not isinstance(catalog["models"], list) or len(catalog["models"]) > 256:
        raise ModError("A model variant catalog needs version 1 and at most 256 models")
    seen = set()
    for document in catalog["models"]:
        if not isinstance(document, dict) or document.get("format") != "MMdl" or \
                type(document.get("version")) is not int or document["version"] != 10 or \
                not isinstance(document.get("resource_id"), str) or \
                not re.fullmatch(r"[0-9A-F]{32}", document["resource_id"]) or \
                document["resource_id"] == "0" * 32 or document["resource_id"] in seen or \
                not isinstance(document.get("source_sha256"), str) or \
                not re.fullmatch(r"[0-9a-f]{64}", document["source_sha256"]) or \
                not isinstance(document.get("changes"), list) or not 1 <= len(document["changes"]) <= 4096:
            raise ModError("Model variants need unique identities, source SHA-256 and named changes")
        seen.add(document["resource_id"])
        changed = set()
        for change in document["changes"]:
            if not isinstance(change, dict) or set(change) != {"field", "value"} or \
                    not isinstance(change["field"], str) or \
                    not re.fullmatch(r"property:(?:0|[1-9][0-9]{0,4})", change["field"]) or \
                    not isinstance(change["value"], str) or \
                    not re.fullmatch(r"reference:(?:0|[1-9][0-9]{0,4})", change["value"]) or \
                    change["field"] in changed:
                raise ModError("Live model variants select unique properties and existing references")
            changed.add(change["field"])
    return catalog["models"]


def validate_mesh_normal(normal):
    if not isinstance(normal, (list, tuple)) or len(normal) != 3 or any(type(v) not in (int, float) or abs(v) > 1 or not math.isfinite(v) for v in normal):
        raise ModError("Mesh normals must be finite unit vectors")
    # The native writer consumes float32 components. Validate the same values
    # before packaging so a boundary does not depend on the host Python float.
    values = struct.unpack("=3f", struct.pack("=3f", *normal))
    if not .995**2 <= sum(v * v for v in values) <= 1.005**2:
        raise ModError("Mesh normals must be finite unit vectors")
    return normal


def validate_mesh_uv(uv):
    if not isinstance(uv, (list, tuple)) or len(uv) != 2 or any(type(v) not in (int, float) or abs(v) > 3.4028234663852886e38 or not math.isfinite(v) for v in uv):
        raise ModError("Texture coordinates need two finite float components")
    return uv


def mesh_variants(folder, manifest):
    if "mesh_edits" not in manifest:
        return []
    catalog = read_json(inside(folder, manifest["mesh_edits"]), 16 * 1024**2)
    if not isinstance(catalog, dict) or set(catalog) != {"version", "meshes"} or \
            type(catalog["version"]) is not int or catalog["version"] != 1 or \
            not isinstance(catalog["meshes"], list) or len(catalog["meshes"]) > 64:
        raise ModError("A mesh catalog needs version 1 and at most 64 resources")
    seen = set()
    for document in catalog["meshes"]:
        if not isinstance(document, dict) or document.get("format") != "EARS_MESH" or \
                type(document.get("version")) is not int or document["version"] != 1 or \
                not isinstance(document.get("resource_id"), str) or not re.fullmatch(r"[0-9A-F]{32}", document["resource_id"]) or \
                document["resource_id"] == "0" * 32 or document["resource_id"] in seen or \
                not isinstance(document.get("source_sha256"), str) or not re.fullmatch(r"[0-9a-f]{64}", document["source_sha256"]) or \
                not isinstance(document.get("changes"), list) or not 1 <= len(document["changes"]) <= 4096:
            raise ModError("Mesh edits need unique identities, source hashes and geometry changes")
        seen.add(document["resource_id"])
        changed = set()
        for change in document["changes"]:
            if not isinstance(change, dict) or not 2 <= len(change) <= 4 or "geometry" not in change or set(change) - {"geometry", "positions", "normals", "uvs"} or \
                    not isinstance(change["geometry"], str) or not re.fullmatch(r"geometry:(?:0|[1-9][0-9]{0,3})", change["geometry"]) or change["geometry"] in changed:
                raise ModError("Mesh changes need unique geometry records and bounded vertex attributes")
            changed.add(change["geometry"])
            count = None
            for field in ("positions", "normals", "uvs"):
                if field not in change:
                    continue
                values = change[field]
                if not isinstance(values, list) or len(values) > 1000000 or count is not None and count != len(values):
                    raise ModError("Edited vertex attributes must have matching bounded counts")
                count = len(values)
                for value in values:
                    if field == "normals":
                        validate_mesh_normal(value)
                    elif field == "uvs":
                        validate_mesh_uv(value)
                    elif not isinstance(value, list) or len(value) != 3 or any(type(v) not in (int, float) or abs(v) > 3.4028234663852886e38 or not math.isfinite(v) for v in value):
                        raise ModError("Each mesh vertex needs three finite float coordinates")

    return catalog["meshes"]


def installed(root: Path):
    result = {}
    if not root.is_dir():
        return result
    for folder in sorted(root.iterdir()):
        if not folder.name.startswith(".") and folder.is_dir() and (folder / "mod.json").is_file():
            m = manifest(folder, verify=False, contents=False)
            if m["id"] in result:
                raise ModError("Duplicate mod id: " + m["id"])
            result[m["id"]] = m
    return result


def resolve(all_mods, enabled, *, verify_contents=True):
    visited, ordered = {}, []
    if len(enabled) > 256 or any(not isinstance(i, str) or not ID.fullmatch(i) for i in enabled) or len(set(enabled)) != len(enabled):
        raise ModError("A mod profile needs unique valid ids (at most 256)")
    def visit(identifier, depth=0):
        if depth >= 64 or len(visited) >= 256 and identifier not in visited:
            raise ModError("A mod profile is limited to 256 mods and 64 dependency levels")
        if identifier not in all_mods:
            raise ModError("Mod is not installed: " + identifier)
        if visited.get(identifier) == 1:
            raise ModError("Dependency cycle at " + identifier)
        if visited.get(identifier) == 2:
            return
        visited[identifier] = 1
        m = all_mods[identifier]
        for dependency, requirement in sorted(m.get("dependencies", {}).items()):
            if dependency not in all_mods or not matches(all_mods[dependency]["version"], requirement):
                raise ModError(identifier + " needs " + dependency + " " + requirement)
            visit(dependency, depth + 1)
        visited[identifier] = 2
        ordered.append(m)
    for identifier in reversed(enabled):
        visit(identifier)
    for m in ordered:
        for other in m.get("conflicts", []):
            if visited.get(other) == 2:
                raise ModError(m["id"] + " conflicts with " + other)
        if verify_contents and "folder" in m:
            checked = manifest(Path(m["folder"]))
            for field in ("id", "version", "dependencies", "conflicts"):
                if checked.get(field) != m.get(field):
                    raise ModError("Mod metadata changed during launch: " + m["id"])
            m.update(checked)
    return ordered


def profile_path(root: Path, profile="default"):
    if not ID.fullmatch(profile):
        raise ModError("Invalid profile name")
    return inside(root / "profiles", profile + ".json")


def profile(root: Path, name="default"):
    path = profile_path(root, name)
    data = read_json(path) if path.is_file() else {"version": 1, "enabled": []}
    if not isinstance(data, dict) or data.get("version") != 1 or not isinstance(data.get("enabled"), list):
        raise ModError("Invalid mod profile")
    if any(not isinstance(x, str) or not ID.fullmatch(x) for x in data["enabled"]) or len(set(data["enabled"])) != len(data["enabled"]):
        raise ModError("A profile needs unique mod ids")
    return data


def set_enabled(root: Path, identifier: str, enabled: bool, name="default"):
    data = profile(root, name)
    selected = [x for x in data["enabled"] if x != identifier]
    if enabled:
        selected.insert(0, identifier)
    resolve(installed(root), selected)
    data["enabled"] = selected
    write_json(profile_path(root, name), data)
    return data


def texture_variants(folder, manifest):
    if "texture_edits" not in manifest:
        return []
    catalog = read_json(inside(folder, manifest["texture_edits"]), 16 * 1024**2)
    if not isinstance(catalog, dict) or set(catalog) != {"version", "textures"} or \
            type(catalog["version"]) is not int or catalog["version"] != 1 or \
            not isinstance(catalog["textures"], list) or len(catalog["textures"]) > 64:
        raise ModError("A texture catalog needs version 1 and at most 64 resources")
    seen = set()
    for document in catalog["textures"]:
        if not isinstance(document, dict) or document.get("format") != "EARS_ITXD" or \
                type(document.get("version")) is not int or document["version"] != 1 or \
                not isinstance(document.get("resource_id"), str) or \
                not re.fullmatch(r"[0-9A-F]{32}", document["resource_id"]) or \
                document["resource_id"] == "0" * 32 or document["resource_id"] in seen or \
                not isinstance(document.get("source_sha256"), str) or \
                not re.fullmatch(r"[0-9a-f]{64}", document["source_sha256"]) or \
                not isinstance(document.get("changes"), list) or not 1 <= len(document["changes"]) <= 1024:
            raise ModError("Texture edits need unique source identities, hashes and image changes")
        seen.add(document["resource_id"])
        changed = set()
        for change in document["changes"]:
            if not isinstance(change, dict) or set(change) != {"texture", "levels"} or \
                    not isinstance(change["texture"], str) or \
                    not re.fullmatch(r"texture:(?:0|[1-9][0-9]{0,3})", change["texture"]) or \
                    change["texture"] in changed or not isinstance(change["levels"], list) or \
                    not 1 <= len(change["levels"]) <= 14:
                raise ModError("Texture changes need unique source records and a complete mip chain")
            changed.add(change["texture"])
            for level in change["levels"]:
                if not isinstance(level, str) or not level or len(level) > 16 * 1024**2 or \
                        len(level) % 2 or not re.fullmatch(r"[0-9a-f]+", level):
                    raise ModError("Texture blocks need bounded lowercase hexadecimal bytes")
    if len(json.dumps(catalog, separators=(",", ":"), ensure_ascii=False).encode("utf-8")) > 16 * 1024**2:
        raise ModError("Texture catalog exceeds 16 MiB")
    return catalog["textures"]


def conflicts(ordered):
    owners = {}
    for m in reversed(ordered):
        for document in texture_variants(Path(m["folder"]), m):
            owners.setdefault("texture:" + document["resource_id"], []).append(m["id"])
        for document in mesh_variants(Path(m["folder"]), m):
            owners.setdefault("mesh:" + document["resource_id"], []).append(m["id"])
        for document in model_variants(Path(m["folder"]), m):
            owners.setdefault("model:" + document["resource_id"], []).append(m["id"])
        folder = Path(m["folder"]) / "gamedata"
        if folder.is_dir():
            for name, _ in files(folder):
                owners.setdefault(name.lower(), []).append(m["id"])
    return [{"path": key, "winner": value[0], "overridden": value[1:]} for key, value in sorted(owners.items()) if len(value) > 1]


def playthrough_key(ordered):
    """Stable across hosts; every dependency and its priority is part of a session."""
    digest = hashlib.sha256()
    for m in ordered:
        content = m.get("fingerprint") or fingerprint(Path(m["folder"]))
        digest.update((m["id"] + "\0" + m["version"] + "\0" + content + "\n").encode("utf-8"))
    return digest.hexdigest()


def prepare_playthrough(user_data: Path, ordered):
    """Seed independent checkpoint files once. Never hardlink writable game saves."""
    if not ordered:
        return user_data
    root = user_data / "mod-playthroughs"
    root.mkdir(parents=True, exist_ok=True)
    destination = root / playthrough_key(ordered)
    if destination.exists():
        if not (destination / "mod-session.json").is_file():
            raise ModError("Mod playthrough is incomplete: " + str(destination))
        return destination
    staging = Path(tempfile.mkdtemp(prefix=".prepare-", dir=root))
    try:
        # User profiles use a 16-digit XUID; title data is below that folder.
        # Snapshots, configs, logs and caches are not checkpoint progress.
        for source in user_data.iterdir():
            if not re.fullmatch(r"[0-9A-Fa-f]{16}", source.name) or not source.is_dir():
                continue
            if source.is_symlink():
                raise ModError("A game profile cannot be a symbolic link")
            for directory, dirs, names in os.walk(source, followlinks=False):
                for name in dirs + names:
                    if (Path(directory) / name).is_symlink():
                        raise ModError("Game checkpoint folders cannot contain symbolic links")
            shutil.copytree(source, staging / source.name)
        write_json(staging / "mod-session.json", {
            "api": API_VERSION, "profile": destination.name,
            "mods": [{"id": m["id"], "version": m["version"], "author": m.get("author", "")}
                     for m in ordered],
        })
        staging.rename(destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return destination


def pack(folder: Path, output: Path):
    manifest(folder, verify=False)
    content = files(folder)
    checksums = {name: sha256(path) for name, path in content}
    output = output.resolve()
    if output.is_relative_to(folder.resolve()):
        raise ModError("Write the package outside its source project")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_name(output.name + ".part")
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
            for name, path in content:
                item = zipfile.ZipInfo(name, (2020, 1, 1, 0, 0, 0))
                item.external_attr = (stat.S_IFREG | 0o644) << 16
                item.compress_type = zipfile.ZIP_DEFLATED
                with path.open("rb") as incoming, archive.open(item, "w", force_zip64=True) as outgoing:
                    shutil.copyfileobj(incoming, outgoing, 65536)
            item = zipfile.ZipInfo("checksums.json", (2020, 1, 1, 0, 0, 0))
            item.external_attr = (stat.S_IFREG | 0o644) << 16
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, json.dumps(checksums, sort_keys=True, indent=2) + "\n")
        os.replace(temporary, output)
    finally:
        temporary.unlink(missing_ok=True)
    return output


def install(archive: Path, root: Path):
    root.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".import-", dir=root))
    try:
        with zipfile.ZipFile(archive) as source:
            members = source.infolist()
            if len(members) > MAX_FILES or sum(m.file_size for m in members) > MAX_BYTES:
                raise ModError("Package exceeds the file or size limit")
            seen = set()
            for member in members:
                path = inside(staging, member.filename.rstrip("/"))
                key = path.relative_to(staging).as_posix().lower()
                if key in seen:
                    raise ModError("Duplicate archive path: " + member.filename)
                seen.add(key)
                mode = member.external_attr >> 16
                if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                    raise ModError("Package contains a non-regular file")
                if member.flag_bits & 1:
                    raise ModError("Encrypted mod packages are not supported")
                if member.is_dir():
                    path.mkdir(parents=True, exist_ok=True)
                else:
                    path.parent.mkdir(parents=True, exist_ok=True)
                    with source.open(member) as incoming, path.open("wb") as outgoing:
                        shutil.copyfileobj(incoming, outgoing, 65536)
            m = manifest(staging)
        destination = inside(root, m["id"])
        if destination.exists():
            raise ModError("Mod already installed: " + m["id"] + ". Remove it before importing this version.")
        staging.rename(destination)
        m["folder"] = str(destination)
        return m
    finally:
        if staging.exists():
            shutil.rmtree(staging)


def seed_bundled(bundled: Path, root: Path):
    """Put the example mods that ship with the launcher into the mods folder.

    Each one is added once and stays disabled until the player enables it. A
    copy the player removed is not brought back, and one they edited is never
    replaced; an untouched copy follows the version the launcher ships.
    Returns the ids that were added or refreshed.
    """
    if not bundled.is_dir():
        return []
    record_path = root / ".bundled.json"
    try:
        record = read_json(record_path) if record_path.is_file() else {}
    except (OSError, ValueError):
        record = {}
    known = record.get("mods") if isinstance(record, dict) else None
    known = dict(known) if isinstance(known, dict) else {}
    before, placed = dict(known), []
    for folder in sorted(bundled.iterdir()):
        if folder.name.startswith(".") or not (folder / "mod.json").is_file():
            continue
        try:
            m = manifest(folder, verify=False, contents=False)
            content = files(folder)
            mark = _fingerprint({name: sha256(path) for name, path in content})
            destination = inside(root, m["id"])
            entry = known.get(m["id"])
            if destination.exists():
                current = fingerprint(destination)
                if current == mark:
                    known[m["id"]] = {"version": m["version"], "fingerprint": mark}
                    continue
                if not isinstance(entry, dict) or entry.get("fingerprint") != current:
                    continue  # the player's own copy, or one they changed
            elif entry is not None:
                continue  # removed by the player
            root.mkdir(parents=True, exist_ok=True)
            staging = Path(tempfile.mkdtemp(prefix=".bundled-", dir=root))
            try:
                for name, path in content:
                    target = staging / "new" / name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(path, target)
                if destination.exists():
                    destination.rename(staging / "old")
                try:
                    (staging / "new").rename(destination)
                except OSError:
                    if (staging / "old").exists():
                        (staging / "old").rename(destination)
                    raise
            finally:
                shutil.rmtree(staging, ignore_errors=True)
            known[m["id"]] = {"version": m["version"], "fingerprint": mark}
            placed.append(m["id"])
        except (ModError, OSError, ValueError):
            continue  # one bad example must not keep the launcher from starting
    if known != before:
        write_json(record_path, {"version": 1, "mods": known})
    return placed
