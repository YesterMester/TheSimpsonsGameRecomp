"""Named native model variants, verified against the creator's installed files.

Inspect exports a readable document. Put changes in its changes list, then build
it into an existing data project. The package carries only the checked edits;
the game owns the original model and all of its resource dependencies.
"""
import json
from pathlib import Path
import tempfile

import commands
import packages
from translations import stream_tool, tool_run


def inspect(args, gamedata, mods):
    if not gamedata or not gamedata.is_dir():
        raise packages.ModError("Choose your installed game files with --gamedata")
    stream = packages.inside(gamedata, args.stream)
    output = Path(args.output).resolve()
    if output.exists():
        raise packages.ModError("Choose a new model document output")
    tool_run(stream_tool(args.stream_tool), "model-export", stream, args.entry, args.resource, output)
    document = packages.read_json(output, 1024**2)
    document["source"]["stream"] = args.stream
    packages.write_json(output, document)
    print("Exported", document["name"], "to", output)
    print("Edit changes; keep the resource identity, source hash and inspection fields intact.")


def build(args, gamedata, mods):
    if not gamedata or not gamedata.is_dir():
        raise packages.ModError("Choose your installed game files with --gamedata")
    folder = commands.project(args.name, mods)
    # A creator may have edited source in an imported package. Rebuilding
    # intentionally replaces its old inventory after the native check succeeds.
    manifest = packages.manifest(folder, verify=False)
    document_path = Path(args.document).resolve()
    document = packages.read_json(document_path, 1024**2)
    if not isinstance(document, dict):
        raise packages.ModError("Use a model document object exported by modkit model inspect")
    source = document.get("source", {})
    if not isinstance(source, dict) or type(source.get("entry")) is not int or source["entry"] < 0:
        raise packages.ModError("Use a document exported by modkit model inspect")
    stream = packages.inside(gamedata, source.get("stream"))
    with tempfile.TemporaryDirectory(prefix="simpsons-model-") as temporary:
        compiled = Path(temporary) / "variant.json"
        tool_run(stream_tool(args.stream_tool), "model-compile", stream, source["entry"], document_path, compiled)
        variant = packages.read_json(compiled, 1024**2)["models"][0]
    relative = manifest.get("model_edits", "assets/model-edits.json")
    destination = packages.inside(folder, relative)
    if "model_edits" not in manifest and destination.exists():
        raise packages.ModError("The default model output already exists; set model_edits to its catalog path in mod.json")
    existing = packages.model_variants(folder, manifest)
    catalog = {"version": 1, "models": [m for m in existing if m["resource_id"] != variant["resource_id"]] + [variant]}
    # Validate before committing either project file. Rebuilding this explicit
    # creator output intentionally invalidates the old packed checksum list.
    serialized = json.dumps(catalog, ensure_ascii=False, allow_nan=False)
    if len(serialized.encode("utf-8")) > 1024**2 or len(catalog["models"]) > 256:
        raise packages.ModError("This project exceeds its model variant budget")
    original = packages.read_json(folder / "mod.json")
    original["model_edits"] = relative
    packages.write_json(destination, catalog)
    packages.write_json(folder / "mod.json", original)
    (folder / "checksums.json").unlink(missing_ok=True)
    print("Built", variant["resource_id"], "in", destination)
    print("Restart the test session to load model changes; export with modkit pack.")


def register(sub):
    parser = sub.add_parser("model", help="inspect and build native model variants")
    commands_ = parser.add_subparsers(dest="model_command", required=True)
    inspect_ = commands_.add_parser("inspect", help="export a named model definition")
    inspect_.add_argument("stream", help="STR path relative to --gamedata")
    inspect_.add_argument("entry", type=int)
    inspect_.add_argument("resource", help="exact resource name or uppercase native identity")
    inspect_.add_argument("output")
    inspect_.set_defaults(func=inspect)
    build_ = commands_.add_parser("build", help="verify an edited document and build its project")
    build_.add_argument("name", help="existing mod project path, name or id")
    build_.add_argument("document")
    build_.set_defaults(func=build)
    for command in (inspect_, build_):
        command.add_argument("--stream-tool", help="path to the native modkit_streams executable")
