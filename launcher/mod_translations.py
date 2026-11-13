"""Editable translation projects for game text and mod-authored strings.

Project catalogs are UTF-8 JSON. Keep the hash/key and the source column intact;
edit translation, leaving null for the original text. Only changed resources or
translated mod strings go into the finished package.
"""
import collections
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

if __package__:
    from . import mod_packages as packages
else:
    import mod_packages as packages

LOCALE = re.compile(r"[A-Za-z]{2,8}(?:[-_][A-Za-z0-9]{1,8}){0,3}\Z")
TOKENS = re.compile(r"\[[^\]\r\n]*\]|%%|%(?:\d+\$)?[-+ #0]*\d*(?:\.\d+)?[sdifuxXc]|%\d+|(?<!\{)\{[A-Za-z_][A-Za-z0-9_.-]*\}(?!\})")
WORKSPACE = ".translations"
_catalog_lock = threading.RLock()


class CatalogConflict(packages.ModError):
    pass


def locale(value):
    if not isinstance(value, str) or not LOCALE.fullmatch(value):
        raise packages.ModError("Use a language code such as fr, pt-BR or es-MX")
    return value.lower().replace("_", "-")


def validate_text(source, translated):
    if translated is None:
        return
    if not isinstance(translated, str) or "\0" in translated:
        raise packages.ModError("Translations must be text without NUL characters")
    if len(translated.encode("utf-8")) > 65536:
        raise packages.ModError("A translated string exceeds 64 KiB")
    original = collections.Counter(TOKENS.findall(source))
    replacement = collections.Counter(TOKENS.findall(translated))
    if original != replacement:
        missing = list((original - replacement).elements())
        extra = list((replacement - original).elements())
        raise packages.ModError(f"Keep button tags and placeholders unchanged; missing {missing}, added {extra}")


class TextTable:
    """An LH2 resource, including its RenderWare wrapper.

    The game's sub_8282D618 fixes up the table at +32 and its following string
    offsets. The high flags bit selects UTF-16; ordinary tables use Windows-1252.
    Other chunks and unsupported table variants are rejected before editing.
    """
    def __init__(self, data):
        self.data = data
        if len(data) < 64 or data[:4] != b"\x16\x07\0\0":
            raise packages.ModError("Not a named game resource")
        if struct.unpack_from("<I", data, 4)[0] != len(data) - 12:
            raise packages.ModError("Resource wrapper length does not match its data")
        self.base = struct.unpack_from(">I", data, 12)[0] + 16
        at = self.base
        if at < 32 or at + 32 > len(data) or data[at:at + 4] != b"2HCL":
            raise packages.ModError("Not an LH2 text table")
        self.size, self.flags, names, count, self.columns = struct.unpack_from(">5I", data, at + 4)
        if names or not 1 <= self.columns <= 2 or self.flags & ~0x80000001:
            raise packages.ModError("Unsupported LH2 table variant; the original file is unchanged")
        end = at + self.size
        start = at + 32 + count * 4 * (1 + self.columns)
        if self.size < 32 or end > len(data) or start > end:
            raise packages.ModError("LH2 table exceeds its resource")
        # sub_82711B28 follows these relative pointers to the resource's
        # allocation header. Padding before the payload varies with alignment;
        # its size field is not at a fixed distance from the LH2 magic.
        def word(offset):
            if offset < 0 or offset + 4 > at:
                raise packages.ModError("Resource header pointer is outside its header")
            return struct.unpack_from(">I", data, offset)[0]
        guid = 20 + word(16)
        class_name = guid + 20
        metadata = class_name + word(guid + 16)
        indirect = metadata + word(metadata) + 4
        allocation = indirect + word(indirect)
        self.size_at = allocation + 8
        if word(self.size_at) != self.size:
            raise packages.ModError("LH2 wrapper and table sizes disagree")
        self.keys = [f"{struct.unpack_from('>I', data, at + 32 + i * 4)[0]:08x}" for i in range(count)]
        if self.keys != sorted(set(self.keys)):
            raise packages.ModError("LH2 keys must be unique and sorted")
        self.strings = {}
        self.raw_columns = []
        self.context = {}
        wide = bool(self.flags & 0x80000000)
        for i in range(count * self.columns):
            key = self.keys[i % count]
            offset = struct.unpack_from(">I", data, at + 32 + (count + i) * 4)[0]
            p = at + offset
            if p < start or p >= end or (wide and offset % 2):
                raise packages.ModError("LH2 string offset is outside its text area")
            if wide:
                stop = p
                while stop + 1 < end and data[stop:stop + 2] != b"\0\0":
                    stop += 2
                if stop + 1 >= end:
                    raise packages.ModError("Unterminated UTF-16 text")
            else:
                stop = data.find(b"\0", p, end)
                if stop == -1:
                    raise packages.ModError("Unterminated game text")
            raw = data[p:stop]
            try:
                text = raw.decode("utf-16-be" if wide else "cp1252")
            except UnicodeError as error:
                raise packages.ModError(f"Unsupported source text encoding: {error}") from error
            if i < count:
                self.strings[key] = text
            else:
                self.context[key] = text
            self.raw_columns.append(raw)
        self.body_sha256 = hashlib.sha256(data[at:end]).hexdigest()

    def rebuild(self, changes):
        unknown = set(changes) - self.strings.keys()
        if unknown:
            raise packages.ModError("Unknown text IDs: " + ", ".join(sorted(unknown)))
        effective = {key: value for key, value in changes.items()
                     if value is not None and value != self.strings[key]}
        if not effective:
            return self.data
        wide = bool(self.flags & 0x80000000)
        encoded = dict(zip(self.keys, self.raw_columns))
        for key, value in effective.items():
            validate_text(self.strings[key], value)
            try:
                encoded[key] = value.encode("utf-16-be" if wide else "cp1252")
            except UnicodeError as error:
                raise packages.ModError(
                    f"{key}: this table/font uses Windows-1252. These characters need a "
                    "Unicode font import; replacing them with '?' would lose your text.") from error
        at, count = self.base, len(self.keys)
        # Keep every untouched string and its offset. Some engine users retain
        # pointers into the original table; rebuilding/deduplicating its whole
        # string pool needlessly changes those identities. Append edited text.
        body = bytearray(self.data[at:at + self.size])
        terminator = b"\0\0" if wide else b"\0"
        if wide and len(body) % 2:
            body.append(0)
        for i, key in enumerate(self.keys):
            if key not in effective:
                continue
            struct.pack_into(">I", body, 32 + (count + i) * 4, len(body))
            body.extend(encoded[key] + terminator)
        struct.pack_into(">I", body, 4, len(body))
        result = bytearray(self.data[:at]) + body
        struct.pack_into(">I", result, self.size_at, len(body))
        # Following chunks may contain Havok or other aligned data. Keep their
        # original alignment when an earlier text chunk changes length.
        result.extend(b"\0" * ((len(self.data) - len(result)) % 16))
        struct.pack_into("<I", result, 4, len(result) - 12)
        # Parse the result before it can enter a package.
        check = TextTable(bytes(result))
        for key in self.keys:
            if check.strings[key] != effective.get(key, self.strings[key]):
                raise packages.ModError("Rebuilt text failed verification")
        return bytes(result)


def stream_tool(explicit=None):
    path = explicit or os.environ.get("SIMPSONS_MODKIT_STREAM_TOOL") or shutil.which("modkit_streams")
    if not path:
        suffix = ".exe" if sys.platform == "win32" else ""
        path = next((folder / ("modkit_streams" + suffix)
                     for folder in (Path(sys.executable).parent,
                                    Path(sys.executable).resolve().parent.parent,
                                    Path(__file__).resolve().parent,
                                    Path(__file__).resolve().parent.parent)
                     if (folder / ("modkit_streams" + suffix)).is_file()), None)
    if not path:
        raise packages.ModError("Set --stream-tool to the modkit_streams executable from the modkit tools build")
    path = Path(path).resolve()
    if not path.is_file():
        raise packages.ModError("Cannot find modkit_streams: " + str(path))
    return path


def tool_run(tool, *args):
    result = subprocess.run([str(tool), *map(str, args)], capture_output=True, text=True,
                            encoding="utf-8", errors="replace")
    if result.returncode:
        raise packages.ModError(result.stderr.strip() or "The stream tool failed")
    return result.stdout


def read_resource(tool, stream, index, temporary):
    destination = Path(temporary) / "resource.bin"
    destination.unlink(missing_ok=True)
    tool_run(tool, "read", stream, index, destination)
    return destination.read_bytes()


def catalog(entries, context=None, **metadata):
    return {"version": 1, **metadata, "entries": {
        key: {"source": value, "translation": None,
              **({"context": context[key]} if context and key in context else {})}
        for key, value in entries.items()}}


def new(args, gamedata, mods):
    target = locale(args.locale)
    identifier = args.id or re.sub(r"[^a-z0-9_.-]+", "-", args.name.lower()).strip(".-_")
    if not packages.ID.fullmatch(identifier):
        raise packages.ModError("Choose an id with --id creator.language-pack")
    destination = packages.inside(mods, getattr(args, "folder", None) or args.name)
    if destination.exists():
        raise packages.ModError("Choose a new project folder")
    mods.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".translation-", dir=mods) as temporary:
        folder = Path(temporary) / "project"
        workspace = folder / WORKSPACE
        workspace.mkdir(parents=True)
        manifest = {"id": identifier, "name": args.name, "version": "1.0.0", "api": 1,
                    "kind": "data", "save_mode": "stateless", "author": args.author,
                    "description": f"{args.locale} translation", "translation_locale": target}
        info = {"version": 1, "locale": target, "catalogs": []}
        if args.of:
            original = packages.manifest(packages.project_folder(args.of, mods))
            base = locale(original.get("default_locale", "en"))
            locales = original.get("locales", {})
            if base not in locales:
                raise packages.ModError("That mod needs a locales/ source catalog and default_locale in mod.json first")
            source = packages.read_json(packages.inside(Path(original["folder"]), locales[base]))
            if not isinstance(source, dict) or not all(isinstance(k, str) and isinstance(v, str) for k, v in source.items()):
                raise packages.ModError("Mod source catalogs are JSON objects of text keys and strings")
            packages.write_json(workspace / "strings.json", catalog(source, mod=original["id"]))
            info.update(kind="mod", base=base, owner=original["id"])
            info["catalogs"].append({"file": "strings.json"})
            manifest["dependencies"] = {original["id"]: original["version"]}
            manifest["translations"] = {original["id"]: {target: "locales/strings.json"}}
        else:
            if not gamedata or not gamedata.is_dir():
                raise packages.ModError("Point --gamedata at your own installed game files")
            tool = stream_tool(args.stream_tool)
            index = Path(temporary) / "index.json"
            print("Indexing game resources…", flush=True)
            tool_run(tool, "index", gamedata.resolve(), index, "--deep")
            inventory = packages.read_json(index, maximum=128 * 1024 * 1024)
            base = locale(args.base)
            resources = [r for r in inventory["resources"] if r["name"].lower().endswith(".lh2") and
                         (r["name"].lower().endswith("." + base + ".lh2") or
                          (base == "en" and not re.search(r"\.[a-z]{2,3}\.lh2$", r["name"], re.I)))]
            if not resources:
                raise packages.ModError("No text tables found for base language " + base)
            info.update(kind="game", base=base)
            manifest["game_text"] = "locales/game-text.json"
            known, previous, payload = {}, None, None
            for i, resource in enumerate(sorted(resources, key=lambda r: (r["stream"], r["entry"]))):
                print(f"Reading text table {i + 1}/{len(resources)}: {resource['name']}", flush=True)
                key = (resource["stream"], resource["entry"])
                if key != previous:
                    payload = read_resource(tool, packages.inside(gamedata, resource["stream"]), resource["entry"], temporary)
                    previous = key
                offset, size = resource["chunk_offset"], resource["chunk_size"]
                data = payload[offset:offset + size]
                digest = hashlib.sha256(data).hexdigest()
                target = {"stream": resource["stream"], "entry": resource["entry"], "offset": offset, "size": size}
                if digest in known:
                    known[digest]["targets"].append(target)
                    continue
                table = TextTable(data)
                file = f"{len(known) + 1:03d}-" + re.sub(r"[^A-Za-z0-9_.-]", "_", resource["name"]) + ".json"
                packages.write_json(workspace / file, catalog(table.strings, context=table.context, resource=resource["name"]))
                record = {"file": file, "targets": [target], "sha256": digest, "body_sha256": table.body_sha256,
                          "source_sha256": hashlib.sha256(json.dumps(table.strings, ensure_ascii=False, sort_keys=True).encode()).hexdigest()}
                info["catalogs"].append(record)
                known[digest] = record
        packages.write_json(workspace / "project.json", info)
        packages.write_json(folder / "mod.json", manifest)
        (folder / "README.md").write_text(
            f"# {args.name}\n\nTranslation by {args.author or 'the package author'}.\n\n"
            f"Edit {WORKSPACE}/*.json, keeping source, text keys and button/format tags. "
            "A null translation uses the original. Check and build with `modkit translate`, then pack and install.\n"
            + (f"\nSelect the original {info['base']} language in the game to use this replacement.\n" if info["kind"] == "game" else ""),
            encoding="utf-8")
        packages.write_json(folder / ".modkit/project.json", {"schema": 1, "template": "translation-pack"})
        folder.rename(destination)
    print("Created", destination)
    print("Open the translation workspace in Modkit, or use modkit translate edit.")
    return destination


def load_project(folder):
    workspace = folder / WORKSPACE
    info = packages.read_json(workspace / "project.json")
    if info.get("version") != 1 or info.get("kind") not in ("game", "mod") or not isinstance(info.get("catalogs"), list):
        raise packages.ModError("Invalid translation project")
    locale(info["locale"])
    catalogs = []
    seen = set()
    for record in info["catalogs"]:
        path = packages.inside(workspace, record["file"])
        if path in seen:
            raise packages.ModError("Duplicate translation catalog")
        seen.add(path)
        data = packages.read_json(path)
        entries = data.get("entries")
        if data.get("version") != 1 or not isinstance(entries, dict):
            raise packages.ModError("Invalid translation catalog: " + record["file"])
        for key, item in entries.items():
            if not isinstance(item, dict) or not isinstance(item.get("source"), str) or "translation" not in item:
                raise packages.ModError("Invalid text entry: " + key)
        catalogs.append((record, data))
    return info, catalogs


def check_project(folder):
    info, catalogs = load_project(folder)
    total = complete = 0
    errors, glyphs = [], set()
    for record, data in catalogs:
        source_digest = record.get("source_sha256")
        if source_digest and source_digest != hashlib.sha256(json.dumps(
                {key: item["source"] for key, item in data["entries"].items()},
                ensure_ascii=False, sort_keys=True).encode()).hexdigest():
            errors.append({"file": record["file"], "error": "Source text was edited; change only translation"})
        for key, item in data["entries"].items():
            total += 1
            value = item["translation"]
            if value is None:
                continue
            complete += 1
            try:
                validate_text(item["source"], value)
                if info["kind"] == "game":
                    for char in value:
                        try:
                            char.encode("cp1252")
                        except UnicodeError:
                            glyphs.add(char)
            except (ValueError, UnicodeError) as error:
                errors.append({"file": record["file"], "key": key, "error": str(error)})
    if glyphs:
        errors.append({"error": "Characters need a Unicode font import", "characters": "".join(sorted(glyphs))})
    return {"locale": info["locale"], "translated": complete, "total": total,
            "remaining": total - complete, "percent": round(100 * complete / total, 2) if total else 100,
            "errors": errors}


def read_catalog(folder, name):
    _, catalogs = load_project(folder)
    if name not in {record["file"] for record, _ in catalogs}:
        raise packages.ModError("Choose a catalog from this project")
    path = packages.inside(folder / WORKSPACE, name)
    source = path.read_bytes()
    return {"catalog": json.loads(source), "revision": hashlib.sha256(source).hexdigest()}


def save_catalog(folder, name, values, revision):
    with _catalog_lock:
        original = read_catalog(folder, name)
        if revision != original["revision"]:
            raise CatalogConflict("The catalog changed on disk. Reload before saving so the other editor's work is kept.")
        catalog = original["catalog"]
        if not isinstance(values, dict) or values.keys() != catalog["entries"].keys():
            raise packages.ModError("Catalog keys changed")
        for key, value in values.items():
            if value is not None and (not isinstance(value, str) or "\0" in value or len(value.encode("utf-8")) > 65536):
                raise packages.ModError("Translations must be text or null, at most 64 KiB each")
            catalog["entries"][key]["translation"] = value
        # Keep unfinished drafts, then report tag/font errors. Export still
        # requires a clean check and never rewrites the original game files.
        packages.write_json(packages.inside(folder / WORKSPACE, name), catalog)
        return {"revision": read_catalog(folder, name)["revision"],
                "progress": check_project(folder)}


def check(args, gamedata, mods):
    result = check_project(packages.project_folder(args.name, mods))
    print(json.dumps(result, ensure_ascii=False, indent=2))
    if result["errors"]:
        raise packages.ModError("Fix the reported translation errors before building")


def build(args, gamedata, mods):
    folder = packages.project_folder(args.name, mods)
    result = check_project(folder)
    if result["errors"]:
        raise packages.ModError(json.dumps(result["errors"], ensure_ascii=False, indent=2))
    info, catalogs = load_project(folder)
    with tempfile.TemporaryDirectory(prefix=".translation-build-", dir=folder) as temporary:
        staging = Path(temporary)
        if info["kind"] == "mod":
            translated = {key: item["translation"] for _, data in catalogs
                          for key, item in data["entries"].items() if item["translation"] is not None}
            packages.write_json(staging / "strings.json", translated)
            destination = folder / "locales/strings.json"
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(staging / "strings.json", destination)
        else:
            old_assets = folder / "gamedata"
            if old_assets.exists() and any(p.is_file() for p in old_assets.rglob("*")):
                raise packages.ModError("Move old experimental archive outputs out of gamedata before building a native text pack")
            tables = {}
            tool = None
            for record, data in catalogs:
                entries = {key: {"source": item["source"], "text": item["translation"]}
                           for key, item in data["entries"].items()
                           if item["translation"] is not None and item["translation"] != item["source"]}
                if not entries:
                    continue
                digest = record.get("body_sha256")
                if not digest:
                    if not gamedata or not gamedata.is_dir():
                        raise packages.ModError("Re-export this older project or select its original --gamedata folder")
                    tool = tool or stream_tool(args.stream_tool)
                    target = record["targets"][0]
                    payload = read_resource(tool, packages.inside(gamedata, target["stream"]), target["entry"], temporary)
                    chunk = payload[target["offset"]:target["offset"] + target["size"]]
                    if hashlib.sha256(chunk).hexdigest() != record["sha256"]:
                        raise packages.ModError("Source game files changed; export a new project")
                    table = TextTable(chunk)
                    if {key: item["source"] for key, item in data["entries"].items()} != table.strings:
                        raise packages.ModError("Source text/IDs changed in " + record["file"])
                    digest = record["body_sha256"] = table.body_sha256
                if not isinstance(digest, str) or not re.fullmatch("[0-9a-f]{64}", digest):
                    raise packages.ModError("Invalid source table SHA-256")
                destination = tables.setdefault(digest, {})
                for key, entry in entries.items():
                    if key in destination and destination[key] != entry:
                        raise packages.ModError("Conflicting translations of the same source table and text ID")
                    destination[key] = entry
            packages.write_json(staging / "game-text.json", {"version": 1, "locale": info["locale"], "base": info["base"], "tables": tables})
            destination = folder / "locales/game-text.json"
            destination.parent.mkdir(parents=True, exist_ok=True)
            os.replace(staging / "game-text.json", destination)
            manifest = packages.read_json(folder / "mod.json")
            manifest["game_text"] = "locales/game-text.json"
            packages.write_json(folder / "mod.json", manifest)
            packages.write_json(folder / WORKSPACE / "project.json", info)
        (folder / "checksums.json").unlink(missing_ok=True)
    print(f"Built {result['translated']}/{result['total']} translations. Untranslated text uses the original.")


def edit(args, gamedata, mods):
    import translation_editor
    translation_editor.serve(packages.project_folder(args.name, mods), open_browser=not args.no_browser)


def register(sub):
    parser = sub.add_parser("translate", help="create, edit, check and build a translation mod")
    parser.add_argument("--stream-tool", help="path to the native modkit_streams tool (game text only)")
    children = parser.add_subparsers(dest="translation_command", required=True)
    p = children.add_parser("new", help="export a translation project from the game or another mod")
    p.add_argument("name")
    p.add_argument("--id")
    p.add_argument("--author", default="")
    p.add_argument("--locale", required=True, help="translation language, for example fr or pt-BR")
    p.add_argument("--base", default="en", help="installed game text language (en, ss, etc.)")
    p.add_argument("--of", help="translate a mod with a locales source catalog instead of game text")
    p.set_defaults(func=new)
    for name, function in (("check", check), ("build", build), ("edit", edit)):
        p = children.add_parser(name)
        p.add_argument("name")
        if name == "edit":
            p.add_argument("--no-browser", action="store_true")
        p.set_defaults(func=function)
