# codegraph — fast code navigation for this repo

Two tools, for the two very different halves of this codebase.

## 1. `clangd_query.py` — the hand-written engine (`tools/rexglue-sdk`)

Real semantic navigation (respects overloads, macros, includes) via the
clangd bundled at `tools/clang20/bin/clangd`, driven over raw LSP.

```
cd tools/codegraph
R=/home/deck/simpsons-recomp/tools/rexglue-sdk
B=$R/build   # wherever that project's compile_commands.json lives

python3 clangd_query.py --root "$R" --compile-commands-dir "$B" symbol <name>
python3 clangd_query.py --root "$R" --compile-commands-dir "$B" outline <file>
python3 clangd_query.py --root "$R" --compile-commands-dir "$B" def <file> <line> <col>
python3 clangd_query.py --root "$R" --compile-commands-dir "$B" refs <file> <line> <col>
python3 clangd_query.py --root "$R" --compile-commands-dir "$B" callers <file> <line> <col>
python3 clangd_query.py --root "$R" --compile-commands-dir "$B" callees <file> <line> <col>
```

`symbol` is fuzzy and project-wide (find a function/class by name without
knowing what file it's in). The rest need a precise line/col — get those
from `outline` or from `symbol`'s own output first.

Each invocation starts a fresh clangd, so `symbol`/`def`/`refs`/`callers`/
`callees` all pay a real one-time cost (~10-15s) to load the on-disk index
cache (`build/.cache/clangd/index/`, safe to keep across sessions — delete
it if results look stale after a big refactor). `outline` is near-instant
since it only needs the one open file, no cross-file index.

If `compile_commands.json` is missing or stale (only a handful of thirdparty
entries, no real project files), the project wasn't configured with
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`. Fix: `cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=ON .`
in the build directory, no rebuild needed, Ninja regenerates it instantly.

**Only point this at `tools/rexglue-sdk`.** Do not try to background-index
`simpsons/generated/default` with it — 203MB of near-identical mechanically
generated functions is exactly the wrong shape for clangd; use tool #2.

## 2. `recomp_index.py` — the generated recompiled game code

The generated game has address-based functions and explicit register operations.
The structural index finds their definitions and literal direct calls. Indirect
dispatch through function pointers, vtables and runtime helpers remains a separate
analysis task; an empty caller list does not prove a function is unused.

The modkit shares this implementation through `code` commands. It adds reviewed
descriptive names from `modkit/symbols.json`, source evidence, public API links,
adapter/research references and a registration-table audit. Names describe known
behavior; they do not recover original source types or create a public ABI.

```
cd tools/codegraph
python3 recomp_index.py build
python3 recomp_index.py status
python3 recomp_index.py search health
python3 recomp_index.py show ScoreKeeper.GetScore
python3 recomp_index.py name sub_8297AEB0
python3 recomp_index.py addr 0x8297AEB0
python3 recomp_index.py gap 0x8297AEB0
python3 recomp_index.py callers ScoreKeeper.GetScore
python3 recomp_index.py callees sub_8297AEB0
```

The legacy address/name commands remain available. Results now use JSON, including
bounded pages and call-site lines. `--db PATH` selects a private index. No compiler,
game data, game window or mod profile is required.

Content hashes detect changed, added and deleted inputs. Queries refuse stale data;
rebuild after changing sources, notes, names or API definitions. Rebuilding an unchanged
tree reuses the database. Builds replace it atomically after rechecking source hashes.
The `gap` command reports neighboring starts, not proven function extents.

See [the modkit guide](../../modkit/CODE_NAVIGATION.md) for reviewed names and the path
from an internal function to a supported mod API.
