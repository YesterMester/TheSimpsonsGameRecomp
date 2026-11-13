"""Creator commands built on the same package format as the launchers."""
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile

import packages
import mod_projects

REPO = Path(__file__).resolve().parents[2]
SDK = REPO / "modkit"


def project(name, mods):
    return packages.project_folder(name, mods)


def new(args, gamedata, mods):
    template = getattr(args, "template", "health-tools")
    if template != "health-tools" and args.kind != "lua":
        raise packages.ModError("This template uses Lua; choose --kind lua")
    if args.kind in {"lua", "data"}:
        result = mod_projects.create(mods, REPO / "launcher/ui/modkit-templates.json",
            {"template": template if args.kind == "lua" else "file-replacements",
             "name": args.name, "id": args.id, "author": args.author, "description": args.description},
            folder_name=args.name)
        print("Created", result["folder"])
        print("Edit your source, then run modkit validate and modkit run.")
        return
    folder = packages.inside(mods, args.name)
    if folder.exists():
        raise packages.ModError("That project folder already exists")
    identifier = args.id or re.sub(r"[^a-z0-9_.-]+", "-", args.name.lower()).strip(".-_")
    if not packages.ID.fullmatch(identifier):
        raise packages.ModError("Choose an id with --id creator.mod-name")
    manifest = {"id": identifier, "name": args.name, "version": "1.0.0", "api": 1,
                "author": args.author, "description": args.description, "kind": args.kind,
                "save_mode": "state" if args.kind == "lua" else "stateless"}
    folder.mkdir(parents=True)
    (folder / "gamedata").mkdir()
    if args.kind == "lua":
        manifest["entry"] = "main.lua"
        manifest["default_locale"] = "en"
        manifest["locales"] = {"en": "locales/en.json"}
        example = SDK / "examples" / template
        shutil.copy2(example / "main.lua", folder / "main.lua")
        shutil.copytree(example / "locales", folder / "locales")
    elif args.kind == "native":
        shutil.copy2(SDK / "examples/native-tools/main.cpp", folder / "main.cpp")
        shutil.copy2(SDK / "examples/native-tools/CMakeLists.txt", folder / "CMakeLists.txt")
        manifest["binaries"] = {}
    packages.write_json(folder / "mod.json", manifest)
    (folder / ".luarc.json").write_text(json.dumps({"runtime.version": "Lua 5.4", "workspace.library": [str(SDK / "generated")], "diagnostics.globals": ["game"]}, indent=2) + "\n")
    print("Created", folder)
    print("Edit main.lua or main.cpp, then run modkit validate and modkit run.")


def validate(args, gamedata, mods):
    folder = project(args.name, mods)
    m = packages.manifest(folder)
    mod_projects.check_capabilities(folder, packages.read_json(SDK / "api.json"),
                                    "These creator tools do not provide: ")
    print(json.dumps({"id": m["id"], "name": m["name"], "author": m.get("author", ""),
                      "kind": m["kind"], "fingerprint": m["fingerprint"],
                      "files": len(packages.files(folder)), "targets": sorted(m.get("binaries", {}))}, indent=2))


def inspect(args, gamedata, mods):
    if args.name:
        folder = project(args.name, mods)
        m = packages.manifest(folder)
        m["files"] = [name for name, _ in packages.files(folder)]
        print(json.dumps(m, indent=2, ensure_ascii=False))
    else:
        print((SDK / "api.json").read_text(encoding="utf-8"))


def pack(args, gamedata, mods):
    folder = project(args.name, mods)
    mod_projects.check_capabilities(folder, packages.read_json(SDK / "api.json"),
                                    "These creator tools do not provide: ")
    output = Path(args.out) if args.out else mods / (folder.name + ".zip")
    print(packages.pack(folder, output))


def install(args, gamedata, mods):
    m = packages.install(Path(args.archive).resolve(), mods)
    print("Installed", m["name"], "by", m.get("author", ""), "(disabled)")


def enable(args, gamedata, mods):
    m = packages.manifest(project(args.name, mods))
    data = packages.set_enabled(mods, m["id"], args.command == "enable", args.profile)
    order = packages.resolve(packages.installed(mods), data["enabled"])
    print(json.dumps({"enabled": data["enabled"], "load_order": [m["id"] for m in order],
                      "file_conflicts": packages.conflicts(order)}, indent=2))


def build(args, gamedata, mods):
    folder = project(args.name, mods)
    m = packages.read_json(folder / "mod.json")
    if m.get("kind") != "native":
        raise packages.ModError("Lua and data mods do not need compilation")
    target = args.target or ("windows-x64" if os.name == "nt" else "linux-x64")
    if target not in packages.TARGETS:
        raise packages.ModError("Unsupported build target")
    build_dir = folder / ".modkit-build" / target
    command = ["cmake", "-S", str(folder), "-B", str(build_dir),
               "-DMODKIT_SDK_DIR=" + str(SDK), "-DCMAKE_BUILD_TYPE=Release"]
    command.extend(args.cmake_arg)
    subprocess.run(command, check=True)
    subprocess.run(["cmake", "--build", str(build_dir), "--config", "Release", "-j", str(args.jobs)], check=True)
    suffix = ".dll" if target.startswith("windows") else ".so"
    binaries = list(build_dir.rglob("simpsons_mod" + suffix))
    if len(binaries) != 1:
        raise packages.ModError("Build did not produce exactly one simpsons_mod" + suffix)
    binary = folder / "bin" / target / binaries[0].name
    binary.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(binaries[0], binary)
    m.setdefault("binaries", {})[target] = binary.relative_to(folder).as_posix()
    packages.write_json(folder / "mod.json", m)
    # A developer rebuild intentionally replaces any previous packed inventory.
    (folder / "checksums.json").unlink(missing_ok=True)
    print("Built", binary)


def run(args, gamedata, mods):
    folder = project(args.name, mods)
    m = packages.manifest(folder)
    if not gamedata or not (gamedata / "default.xex").is_file():
        raise packages.ModError("Point --gamedata at your installed game files")
    exe = Path(args.exe).resolve()
    if not exe.is_file():
        raise packages.ModError("Game executable does not exist")
    run_root = Path(tempfile.mkdtemp(prefix="simpsons-mod-"))
    user = run_root / "userdata"
    user.mkdir()
    root = mods.resolve()
    # A project can live outside the installed mods directory. Use an isolated
    # package root and copy only that project, leaving originals untouched.
    if folder.parent != root:
        root = run_root / "mods"
        shutil.copytree(folder, root / m["id"], ignore=shutil.ignore_patterns(".git", ".modkit-build", "build", "__pycache__"))
    packages.write_json(user / "mods-profile.json", {"version": 1, "enabled": [m["id"]]})
    # Use an absolute private profile; installed enablement stays intact.
    command = [str(exe), "--game_data_root=" + str(gamedata.resolve()),
               "--user_data_root=" + str(user), "--mods_root=" + str(root),
               "--mods_profile_file=" + str(user / "mods-profile.json")]
    if args.developer:
        command.append("--mods_developer=true")
    env = os.environ.copy()
    if args.lib_dir:
        libraries = str(Path(args.lib_dir).resolve())
        key = "PATH" if os.name == "nt" else "LD_LIBRARY_PATH"
        env[key] = libraries + os.pathsep + env.get(key, "")
    print("Private test data and logs:", run_root, flush=True)
    raise SystemExit(subprocess.run(command + args.game_arg, env=env, cwd=exe.parent).returncode)


def doctor(args, gamedata, mods):
    checks = {"sdk": SDK.is_dir(), "cmake": bool(shutil.which("cmake")),
              "compiler": next((shutil.which(c) for c in ("clang++", "g++", "cl") if shutil.which(c)), None),
              "game_data": bool(gamedata and (gamedata / "default.xex").is_file()),
              "mods": str(mods), "platform": platform.platform()}
    try:
        installed = packages.installed(mods)
        selected = packages.profile(mods)["enabled"]
        checks["load_order"] = [m["id"] for m in packages.resolve(installed, selected)]
        checks["installed"] = len(installed)
    except (ValueError, OSError) as error:
        checks["error"] = str(error)
    print(json.dumps(checks, indent=2))


def register(parser, sub):
    parser.add_argument("--profile", default="default", help="mod profile (default: default)")
    for command, callback in [("validate", validate), ("inspect", inspect), ("install", install),
                              ("enable", enable), ("disable", enable), ("build", build), ("run", run), ("doctor", doctor)]:
        p = sub.add_parser(command, help=(callback.__doc__ or command))
        if command == "install":
            p.add_argument("archive")
        elif command != "doctor":
            p.add_argument("name", nargs="?" if command == "inspect" else None)
        if command == "build":
            p.add_argument("--target", choices=sorted(packages.TARGETS))
            p.add_argument("--jobs", type=int, default=2)
            p.add_argument("--cmake-arg", action="append", default=[])
        if command == "run":
            p.add_argument("--exe", required=True)
            p.add_argument("--lib-dir")
            p.add_argument("--game-arg", action="append", default=[])
            p.add_argument("--developer", action="store_true", help="enable the local creator inspection bridge")
        p.set_defaults(func=callback)
