"""Editable creator projects. All writes stay inside a selected mod project."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import tempfile
import uuid

if __package__:
    from . import mod_packages as packages
    from . import mod_scenes as scenes
    from . import mod_documents as documents
else:
    import mod_packages as packages
    import mod_scenes as scenes
    import mod_documents as documents

MAX_TEXT = 128 * 1024
TEXT_TYPES = {".lua", ".json", ".md", ".txt", ".ini", ".cfg", ".xml"}
DEFAULT_BUNDLE = Path(__file__).resolve().parent / "ui/modkit-templates.json"


def catalog(path):
    data = packages.read_json(Path(path))
    if data.get("schema") != 1 or data.get("api") != packages.API_VERSION:
        raise packages.ModError("These project templates need a different Modkit version")
    return data


def templates(path):
    return [{key: t[key] for key in ("id", "name", "description")}
            for t in catalog(path)["templates"]]


def list_projects(root):
    """Creator browsing does not depend on the player's enabled mod profile."""
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    result = []
    for manifest in packages.installed(root).values():
        folder = Path(manifest["folder"])
        marker = folder / ".modkit/project.json"
        if marker.is_file() and not marker.is_symlink():
            result.append({**describe(folder), "editable": True})
    return result


def text(value, field, maximum, required=False):
    if not isinstance(value, str) or len(value.encode("utf-8")) > maximum or \
            any(ord(c) < 32 for c in value) or (required and not value.strip()):
        raise packages.ModError(field + " needs readable text of at most " + str(maximum) + " UTF-8 bytes")
    return value.strip()


def create(root, bundle, request, folder_name=None):
    root = Path(root)
    data = catalog(bundle)
    template = next((t for t in data["templates"] if t["id"] == request.get("template")), None)
    if template is None:
        raise packages.ModError("Choose a project template")
    name = text(request.get("name"), "Name", 160, True)
    identifier = request.get("id") or re.sub(r"[^a-z0-9_.-]+", "-", name.lower()).strip(".-_")
    if not isinstance(identifier, str) or not packages.ID.fullmatch(identifier):
        raise packages.ModError("Choose a stable id such as yourname.my-mod")
    author = text(request.get("author", ""), "Author", 160)
    description = text(request.get("description", ""), "Description", 1024)
    root.mkdir(parents=True, exist_ok=True)
    destination = packages.inside(root, folder_name or identifier)
    if destination.exists() or identifier in packages.installed(root):
        raise packages.ModError("That project folder or mod id already exists")
    staging = Path(tempfile.mkdtemp(prefix=".create-", dir=root))
    try:
        manifest = dict(template["manifest"])
        manifest.update(id=identifier, name=name, author=author,
                        description=description or template["description"], version="1.0.0", api=1)
        (staging / "gamedata").mkdir()
        for relative, content in template["files"].items():
            output = packages.inside(staging, relative)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(content, encoding="utf-8")
        packages.write_json(staging / "mod.json", manifest)
        packages.write_json(staging / ".modkit/project.json", {"schema": 1, "template": template["id"]})
        if manifest["kind"] == "lua":
            (staging / ".modkit/api/game.lua").parent.mkdir(parents=True, exist_ok=True)
            (staging / ".modkit/api/game.lua").write_text(data["completion"], encoding="utf-8")
            packages.write_json(staging / ".luarc.json", {"runtime.version": "Lua 5.4",
                "workspace.library": [".modkit/api"], "diagnostics.globals": ["game"]})
        packages.manifest(staging)
        if destination.exists():
            raise packages.ModError("That project folder already exists")
        staging.rename(destination)
    finally:
        if staging.exists():
            shutil.rmtree(staging)
    return describe(destination)


def find(root, identifier):
    if not isinstance(identifier, str) or not packages.ID.fullmatch(identifier):
        raise packages.ModError("Choose a mod project")
    root = Path(root).resolve()
    for candidate in root.iterdir():
        if not candidate.name.startswith(".") and candidate.is_symlink():
            raise packages.ModError("Mod folders cannot be symbolic links")
    for m in packages.installed(root).values():
        if m["id"] == identifier:
            folder = Path(m["folder"])
            if not folder.is_relative_to(root) or (folder / ".modkit").is_symlink() or \
                    (folder / ".modkit/project.json").is_symlink() or not (folder / ".modkit/project.json").is_file():
                break
            marker = packages.read_json(folder / ".modkit/project.json")
            if marker.get("schema") != 1:
                break
            return folder
    raise packages.ModError("This mod is not an editable creator project")


def describe(folder):
    m = packages.manifest(folder, contents=False)
    editable = [name for name, p in packages.files(folder)
                if p.suffix.lower() in TEXT_TYPES and p.stat().st_size <= MAX_TEXT
                and not name.startswith(".")]
    return {"id": m["id"], "name": m["name"], "author": m.get("author", ""),
            "translation": (Path(folder) / ".translations/project.json").is_file(),
            "version": m["version"], "files": editable[:256], "folder": str(folder), "has_scene": "scene" in m, "documents": [kind for kind in documents.KINDS if kind in m]}


def source_path(folder, relative):
    path = packages.inside(folder, relative)
    # Refuse links even when they point back into the project.
    current = Path(folder)
    for part in Path(relative).parts:
        current /= part
        if current.is_symlink():
            raise packages.ModError("Source files cannot be symbolic links")
    if relative.startswith(".") or path.suffix.lower() not in TEXT_TYPES or not path.is_file():
        raise packages.ModError("Choose an existing text source file")
    if path.stat().st_size > MAX_TEXT:
        raise packages.ModError("The source editor supports files up to 128 KiB")
    return path


def read(folder, relative):
    path = source_path(folder, relative)
    raw = path.read_bytes()
    try:
        content = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise packages.ModError("This source file is not UTF-8 text") from None
    return {"path": relative, "text": content, "revision": hashlib.sha256(raw).hexdigest()}


def write(folder, relative, content, revision):
    path = source_path(folder, relative)
    if not isinstance(content, str) or len(content.encode("utf-8")) > MAX_TEXT or "\0" in content:
        raise packages.ModError("Source must be UTF-8 text of at most 128 KiB")
    if read(folder, relative)["revision"] != revision:
        raise packages.ModError("This file changed since you opened it. Reopen it before saving")
    if path.suffix.lower() == ".json":
        def invalid_constant(value):
            raise packages.ModError("JSON cannot contain " + value)
        value = json.loads(content, parse_constant=invalid_constant)
        if relative == "mod.json":
            if not isinstance(value, dict) or value.get("id") != packages.read_json(path).get("id"):
                raise packages.ModError("Keep this project's stable mod id")
            with tempfile.TemporaryDirectory(prefix=".metadata-", dir=folder) as directory:
                probe = Path(directory)
                (probe / "mod.json").write_text(content, encoding="utf-8")
                packages.manifest(probe, contents=False)
    temporary = path.with_name("." + path.name + ".source-" + os.urandom(8).hex())
    try:
        with temporary.open("xb") as output:
            output.write(content.encode("utf-8"))
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)
    return read(folder, relative)


def validate(root, identifier, bundle=DEFAULT_BUNDLE):
    folder = find(root, identifier)
    m = packages.manifest(folder)
    check_capabilities(folder, catalog(bundle), "These creator tools do not provide: ")
    order = packages.resolve(packages.installed(Path(root)), [identifier])
    return {"id": identifier, "fingerprint": m["fingerprint"],
            "load_order": [item["id"] for item in order], "conflicts": packages.conflicts(order),
            "msg": "Package structure, files and dependencies are valid. Test checks script behavior in the game."}


def check_capabilities(folder, descriptor, message="Update the game build before testing this project. It needs: "):
    m = packages.manifest(folder, contents=False)
    operations = {item["name"] if isinstance(item, dict) else item for item in descriptor.get("operations", [])}
    events = set(descriptor.get("events", {}))
    missing = sorted(set(m.get("requires_operations", [])) - operations)
    missing += sorted(set(m.get("requires_events", [])) - events)
    if missing:
        raise packages.ModError(message + ", ".join(missing))


def prepare_test(root, identifier, user_data, descriptor=None, preview=None, game=None):
    folder = find(root, identifier)
    if descriptor is not None:
        check_capabilities(folder, descriptor)
    validate(root, identifier)
    # A fixed private session per project keeps test progress across launches;
    # the normal profile and vanilla saves are neither copied nor changed.
    session = Path(user_data) / "mod-creator-tests" / identifier
    if session.parent.is_symlink() or session.is_symlink():
        raise packages.ModError("Private test folders cannot be symbolic links")
    session.mkdir(parents=True, exist_ok=True)
    current = session
    if preview is not None:
        if not descriptor or 'editor-viewport-1' not in descriptor.get('features', []):
            raise packages.ModError('Update the game build to use direct level previews')
        if __package__:
            from . import modkit_levels
        else:
            import modkit_levels
        script = modkit_levels.installed_script(game, folder)
        entry = next((item for item in modkit_levels.catalog(script)
                      if item['id'] == preview and item['mode'] == 'MODE_STANDARD'), None)
        if entry is None:
            raise packages.ModError('Choose a standard map entry from this installation')
        content = modkit_levels.select_entry(script, entry)
        # Each direct preview starts fresh. Never Continue into a previous
        # level's progress, change the normal profile or copy game archives.
        current = packages.inside(session, 'viewport/' + uuid.uuid4().hex)
        if (session / 'viewport').is_symlink():
            raise packages.ModError('Private viewport folders cannot be symbolic links')
        target = current / 'editor-content'
        target.mkdir(parents=True)
        (target / modkit_levels.GAMEFLOW).write_bytes(content.encode('utf-8', errors='surrogateescape'))
    profile = current / "mods-profile.json"
    packages.write_json(profile, {"version": 1, "enabled": [identifier]})
    return {"user_data": current, "profile": profile, "project": describe(folder),
            "preview": preview, "session_root": session}


def record_test(session):
    """Record only a successfully launched Test for later draft application."""
    base = session['session_root']
    packages.write_json(base / 'last-test.json', {'schema': 1,
        'relative': session['user_data'].relative_to(base).as_posix()})


def apply_drafts(folder, user_data, kinds=None):
    """Apply matching documents in one revision-checked metadata replacement."""
    if user_data is None:
        raise packages.ModError("Use this project's Test action and save a draft first")
    m = packages.manifest(folder)
    kinds = list(kinds) if kinds is not None else [k for k in documents.KINDS if k in m]
    if not kinds or any(k not in documents.KINDS or k not in m for k in kinds):
        raise packages.ModError("Choose a scene or mission editor project")
    root = Path(user_data)
    updates, available = {}, {}
    test = root / 'mod-creator-tests' / m['id']
    marker = test / 'last-test.json'
    if marker.is_symlink():
        raise packages.ModError('Project test storage cannot contain symbolic links')
    if marker.is_file():
        relative = packages.read_json(marker).get('relative')
        if relative != '.':
            if not isinstance(relative, str) or not re.fullmatch(r'viewport/[a-f0-9]{32}', relative):
                raise packages.ModError('The last Test session path is invalid')
            test = packages.inside(test, relative)
    storage = test.relative_to(root).as_posix() + "/mod-storage/" + m["id"] + "/"
    def saved_path(name):
        current = root
        if root.is_symlink():
            raise packages.ModError("Project test storage cannot contain symbolic links")
        relative = storage + name
        for part in Path(relative).parts:
            current /= part
            if current.is_symlink():
                raise packages.ModError("Project test storage cannot contain symbolic links")
        return packages.inside(root, relative)
    candidates = {}
    path = saved_path("project-drafts.json")
    if path.is_file():
        try:
            combined = documents.bundle(packages.read_json(path, 1024 * 1024))
        except (ValueError, TypeError, OverflowError) as error:
            raise packages.ModError("Invalid project drafts: " + str(error)) from error
        for kind, value in combined["documents"].items():
            candidates[kind] = (path.stat().st_mtime_ns, combined["fingerprint"], value)
    for kind in kinds:
        path = saved_path(kind + "-draft.json")
        if path.is_file():
            try:
                draft = documents.draft(packages.read_json(path, 1024 * 1024), kind)
            except (ValueError, TypeError, OverflowError) as error:
                raise packages.ModError("Invalid " + kind + " draft: " + str(error)) from error
            if kind not in candidates or path.stat().st_mtime_ns > candidates[kind][0]:
                candidates[kind] = (path.stat().st_mtime_ns, draft["fingerprint"], draft[kind])
        if kind not in candidates:
            continue
        _, fingerprint, value = candidates[kind]
        available[kind] = value
        if value == documents.validate(kind, m[kind]):
            continue
        if fingerprint != m["fingerprint"]:
            raise packages.ModError("Project sources changed after this draft. Test the current project and save a new draft")
        updates[kind] = value
    if not available:
        raise packages.ModError("Save a draft in this project's Test session, then close the game")
    counts = {"objects": len(available.get("scene", {}).get("objects", [])),
              "objectives": len(available.get("mission", {}).get("objectives", []))}
    if not updates:
        return {"msg": "These saved drafts are already applied", "documents": [], **counts}
    opened = read(folder, "mod.json")
    source = json.loads(opened["text"])
    source.update(updates)
    if packages.manifest(folder)["fingerprint"] != m["fingerprint"]:
        raise packages.ModError("Project sources changed while applying the saved drafts")
    result = write(folder, "mod.json", json.dumps(source, indent=2, ensure_ascii=False) + "\n", opened["revision"])
    return {"msg": "Saved drafts applied to mod.json; Validate, Test or Export ZIP", "documents": list(updates),
            "revision": result["revision"], **counts}


def apply_scene(folder, user_data):
    result = apply_drafts(folder, user_data, ["scene"])
    result["msg"] = "Scene applied to mod.json; Validate, Test or Export ZIP" if result["documents"] else "This scene draft is already applied"
    return result


def dispatch(root, bundle, request, user_data=None):
    action = request.get("action")
    if action == "templates":
        return {"templates": templates(bundle)}
    if action == "create":
        return {"project": create(root, bundle, request), "msg": "Project created, disabled. Edit its source, then Validate and Test."}
    folder = find(root, request.get("id"))
    if action == "open":
        return {"project": describe(folder)}
    if action == "read":
        return read(folder, request.get("path"))
    if action == "write":
        result = write(folder, request.get("path"), request.get("text"), request.get("revision"))
        result["msg"] = "Source saved. Restart or reload the mod to apply it."
        return result
    if action == "validate":
        return validate(root, request.get("id"), bundle)
    if action == "apply-drafts":
        return apply_drafts(folder, user_data)
    if action == "apply-scene":
        return apply_scene(folder, user_data)
    if action == "export":
        validate(root, request.get("id"), bundle)
        m = packages.manifest(folder)
        output = Path(root) / "exports" / (m["id"] + "-" + m["version"] + ".zip")
        packages.pack(folder, output)
        return {"path": str(output), "msg": "Package exported to " + str(output)}
    raise packages.ModError("Unknown project action")
