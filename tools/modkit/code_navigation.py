"""Offline game-code navigation, shared with the existing codegraph tool."""
from pathlib import Path
import importlib
import sqlite3
import sys

import packages

ROOT = Path(__file__).resolve().parents[2]
CATALOG_DIR = ROOT / "tools/codegraph"


def _catalog():
    # Packaged creator tools can omit generated-code navigation. That must not
    # prevent a modder from building, packaging or translating their own mod.
    if not (CATALOG_DIR / "recomp_catalog.py").is_file():
        raise packages.ModError("Code navigation needs a source checkout with tools/codegraph")
    if str(CATALOG_DIR) not in sys.path:
        sys.path.insert(0, str(CATALOG_DIR))
    return importlib.import_module("recomp_catalog")


def execute(args, gamedata, mods):
    recomp_catalog = _catalog()
    try:
        if args.code_command == "build":
            root = Path(args.source).expanduser().resolve()
            result = recomp_catalog.build(
                args.generated_dir or root / "simpsons/generated/default",
                args.db, force=args.force, repo_root=root, symbols_path=args.symbols)
        elif args.code_command == "status":
            result = recomp_catalog.status(args.db)
        else:
            result = recomp_catalog.query(args.db, args.code_command, args.query, args.limit)
        recomp_catalog.print_result(result)
    except sqlite3.DatabaseError as error:
        raise packages.ModError("Code index: " + str(error)) from error


def register(sub):
    parser = sub.add_parser("code", help="find game functions by system, readable name or address")
    parser.add_argument("--db", default=str(CATALOG_DIR / "recomp_index.sqlite"), help="rebuildable code index path")
    parser.add_argument("--limit", type=int, default=40, help="maximum results, from 1 to 500")
    actions = parser.add_subparsers(dest="code_command", required=True)
    build = actions.add_parser("build", help="index the current sources, notes and reviewed names")
    build.add_argument("--source", default=str(ROOT), help="source checkout root")
    build.add_argument("--generated-dir", help="override the checkout's generated source directory")
    build.add_argument("--symbols", help="override the checkout's modkit/symbols.json")
    build.add_argument("--force", action="store_true")
    actions.add_parser("status", help="coverage counts and content-hash freshness")
    for command in ("search", "show", "callers", "callees", "gap"):
        action = actions.add_parser(command)
        action.add_argument("query", help="search text, readable name, sub_XXXXXXXX or hexadecimal address")
    parser.set_defaults(func=execute)
