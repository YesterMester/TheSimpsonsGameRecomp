"""Source-checked texture images, with original native headers and ownership.

DDS files retain all original mip levels and their BC1/BC2/BC3 or RGBA8 format.
Edit those images in your texture editor; the native tool handles Xbox tiling
and byte order. Packages contain image changes rather than copied archives.
"""
import json
from pathlib import Path
import tempfile

import commands
import packages
from translations import stream_tool, tool_run


def document(path):
    value = packages.read_json(Path(path), 16 * 1024**2)
    if not isinstance(value, dict) or value.get("format") != "EARS_ITXD" or \
            type(value.get("version")) is not int or value["version"] != 1:
        raise packages.ModError("Use a document from texture inspect")
    return value


def source(value, gamedata):
    if not gamedata or not gamedata.is_dir():
        raise packages.ModError("Choose your installed game files with --gamedata")
    location = value.get("source", {})
    if not isinstance(location, dict) or type(location.get("entry")) is not int or location["entry"] < 0:
        raise packages.ModError("Use a document from texture inspect")
    return packages.inside(gamedata, location.get("stream")), location["entry"]


def fresh(path):
    path = Path(path).resolve()
    if path.exists():
        raise packages.ModError("Choose a new texture output file")
    return path


def inspect(args, gamedata, mods):
    stream, entry = source({"source": {"stream": args.stream, "entry": args.entry}}, gamedata)
    output = fresh(args.output)
    tool_run(stream_tool(args.stream_tool), "texture-export", stream, entry, args.resource, output)
    value = document(output)
    value["source"]["stream"] = args.stream
    packages.write_json(output, value)
    print("Exported", value["name"], "to", output)
    for texture in value["textures"]:
        print(texture["id"], texture["name"], f'{texture["width"]}x{texture["height"]}',
              f'{texture["mips"]} mip(s)', "editable" if texture["editable"] else texture["reason"])


def image(args, gamedata, mods):
    value = document(args.document)
    stream, entry = source(value, gamedata)
    command = "texture-image" if args.texture_command == "export" else "texture-reimport"
    extra = [Path(args.dds).resolve()] if args.texture_command == "import" else []
    output = fresh(args.output)
    tool_run(stream_tool(args.stream_tool), command, stream, entry, Path(args.document).resolve(),
             args.texture, *extra, output)
    print("Exported DDS image to" if not extra else "Imported texture project to", output)
    if extra:
        print("Run texture build to put the checked image changes into your mod.")


def build(args, gamedata, mods):
    value = document(args.document)
    stream, entry = source(value, gamedata)
    folder = commands.project(args.name, mods)
    manifest = packages.manifest(folder, verify=False)
    with tempfile.TemporaryDirectory(prefix="simpsons-texture-") as temporary:
        output = Path(temporary) / "textures.json"
        tool_run(stream_tool(args.stream_tool), "texture-compile", stream, entry,
                 Path(args.document).resolve(), output)
        compiled = packages.read_json(output, 16 * 1024**2)["textures"][0]
    relative = manifest.get("texture_edits", "assets/texture-edits.json")
    destination = packages.inside(folder, relative)
    if "texture_edits" not in manifest and destination.exists():
        raise packages.ModError("The default texture output exists; choose its catalog path in mod.json")
    existing = packages.texture_variants(folder, manifest)
    catalog = {"version": 1, "textures": [d for d in existing if d["resource_id"] != compiled["resource_id"]] + [compiled]}
    if len(catalog["textures"]) > 64 or len(json.dumps(catalog, ensure_ascii=False).encode("utf-8")) > 16 * 1024**2:
        raise packages.ModError("Texture catalog exceeds its budget")
    original = packages.read_json(folder / "mod.json")
    original["texture_edits"] = relative
    packages.write_json(destination, catalog)
    packages.write_json(folder / "mod.json", original)
    (folder / "checksums.json").unlink(missing_ok=True)
    print("Built texture edits in", destination)
    print("Restart the test session to load edited textures.")


def register(sub):
    parser = sub.add_parser("texture", help="inspect and edit native texture images")
    actions = parser.add_subparsers(dest="texture_command", required=True)
    inspect_ = actions.add_parser("inspect", help="list a texture dictionary and its editable images")
    inspect_.add_argument("stream"); inspect_.add_argument("entry", type=int)
    inspect_.add_argument("resource"); inspect_.add_argument("output"); inspect_.set_defaults(func=inspect)
    export = actions.add_parser("export", help="export one texture's complete DDS mip chain")
    import_ = actions.add_parser("import", help="import a same-format DDS image")
    for action in (export, import_):
        action.add_argument("document"); action.add_argument("texture", help="texture:N from inspect")
        if action is import_: action.add_argument("dds")
        action.add_argument("output"); action.set_defaults(func=image)
    build_ = actions.add_parser("build", help="verify and package texture image changes")
    build_.add_argument("name"); build_.add_argument("document"); build_.set_defaults(func=build)
    for action in (inspect_, export, import_, build_): action.add_argument("--stream-tool")
