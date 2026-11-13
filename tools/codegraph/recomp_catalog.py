"""Index generated functions, direct calls and documented game-system names.

This is structural navigation, not a decompiler. Indirect dispatch is counted
explicitly; its targets cannot be recovered from literal calls alone. Original
address symbols stay intact so regeneration and native hooks keep working.
"""
import argparse
from bisect import bisect_right
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GENERATED_DIR = str(ROOT / "simpsons/generated/default")
DEFAULT_DB = str(Path(__file__).with_name("recomp_index.sqlite"))
SCHEMA = 2
FUNC_DEF_RE = re.compile(
    r"^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?(sub_[0-9A-Fa-f]{8})\)\s*\{",
    re.MULTILINE)
CALL_RE = re.compile(r"\b(?P<original>__imp__)?(?P<name>sub_[0-9A-Fa-f]{8})"
                     r"\s*\(\s*ctx\s*,\s*base\s*\)")
INDIRECT_RE = re.compile(r"\b(?:REX|PPC)_CALL_INDIRECT_FUNC\s*\(")
ADDRESS_RE = re.compile(r"\b(?:__imp__)?sub_([0-9A-Fa-f]{8})\b|\b0x([0-9A-Fa-f]{8})\b")
MAPPING_RE = re.compile(r"^\s*\{\s*0x([0-9A-Fa-f]{8})\s*,\s*(\w+)\s*\},?\s*$", re.MULTILINE)
NON_CODE_RE = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|/\*.*?\*/|//[^\n]*',
                         re.DOTALL)


def _digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").hexdigest()


def _code(text):
    return NON_CODE_RE.sub(lambda m: re.sub(r"[^\n]", " ", m.group()), text)


def _inside(root, relative):
    path = (root / relative).resolve()
    if Path(relative).is_absolute() or not path.is_relative_to(root):
        raise ValueError("Code evidence must be a file within the source checkout")
    return path


def _inputs(generated, root, symbols):
    paths = {path.resolve(): "generated" for path in generated.glob("*.cpp")}
    if not paths:
        raise ValueError("No generated C++ sources found in " + str(generated))
    paths[Path(__file__).resolve()] = "indexer"
    for folder, suffixes in ((root / "simpsons/re", {".md"}),
                             (root / "simpsons/src", {".cpp", ".h"})):
        for path in folder.glob("*"):
            if path.suffix in suffixes and path.is_file():
                paths[path.resolve()] = "reference"
    for path in (symbols, root / "modkit/api.json"):
        if path.is_file():
            paths[path.resolve()] = "catalog"
    if symbols.is_file():
        catalog = json.loads(symbols.read_text(encoding="utf-8"))
        if catalog.get("schema") != 1 or not isinstance(catalog.get("functions"), list):
            raise ValueError("Unsupported readable-symbol catalog")
        for entry in catalog["functions"]:
            for evidence in entry.get("evidence", []):
                path = _inside(root, evidence["path"])
                if not path.is_file():
                    raise ValueError("Missing code evidence: " + evidence["path"])
                paths.setdefault(path, "reference")
    return [{"path": str(path), "kind": kind, "sha256": _digest(path)}
            for path, kind in sorted(paths.items())]


def _parse(path):
    text = path.read_text(encoding="utf-8")
    # Keep offsets and lines when removing comments and strings. This handles
    # the regular generated files; it is not an arbitrary C++ parser.
    code = _code(text)
    line_starts = [0] + [m.end() for m in re.finditer("\n", text)]
    starts = list(FUNC_DEF_RE.finditer(code))
    functions, calls = [], []
    for index, match in enumerate(starts):
        next_start = starts[index + 1].start() if index + 1 < len(starts) else len(code)
        closing = re.search(r"^}", code[match.end():next_start], re.MULTILINE)
        if not closing:
            raise ValueError(f"Incomplete generated function {match.group(1)} in {path}")
        end = match.end() + closing.end()
        name = "sub_" + match.group(1)[4:].upper()
        body = code[match.end():end]
        functions.append((name, int(name[4:], 16), str(path),
                          bisect_right(line_starts, match.start()),
                          bisect_right(line_starts, end - 1),
                          hashlib.sha256(text[match.start():end].encode("utf-8")).hexdigest(),
                          len(INDIRECT_RE.findall(body))))
        for call in CALL_RE.finditer(body):
            target = "sub_" + call.group("name")[4:].upper()
            calls.append((name, target,
                          bisect_right(line_starts, match.end() + call.start()),
                          "original" if call.group("original") else "direct"))
    return functions, calls


def _annotations(conn, root, symbols):
    operations = {}
    api = root / "modkit/api.json"
    if api.is_file():
        operations = {op["name"]: op for op in json.loads(api.read_text(encoding="utf-8"))["operations"]}
    conn.executemany("INSERT INTO api VALUES (?, ?)",
                     [(name, op["description"]) for name, op in operations.items()])
    if not symbols.is_file():
        return
    entries = json.loads(symbols.read_text(encoding="utf-8"))["functions"]
    names, addresses = set(), set()
    for entry in entries:
        name, symbol = entry["name"], entry["symbol"]
        if not re.fullmatch(r"sub_[0-9A-F]{8}", symbol):
            raise ValueError("Invalid address symbol: " + symbol)
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_.]*", name):
            raise ValueError("Invalid readable name: " + name)
        if name.lower() in names or symbol in addresses:
            raise ValueError("Duplicate readable name or address: " + name)
        names.add(name.lower())
        addresses.add(symbol)
        found = conn.execute("SELECT sha256 FROM functions WHERE name = ?", (symbol,)).fetchone()
        if not found:
            raise ValueError("Readable name has no generated definition: " + symbol)
        expected = entry.get("definition_sha256", "")
        if not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError("Readable name needs a reviewed definition hash: " + name)
        reasons = [] if found[0] == expected else ["generated definition changed"]
        evidence = []
        for source in entry.get("evidence", []):
            path = _inside(root, source["path"])
            text = path.read_text(encoding="utf-8")
            quote = source.get("quote", "")
            if not quote:
                raise ValueError("Readable names need nonempty evidence quotes")
            offset = text.find(quote)
            if offset < 0:
                reasons.append("evidence changed: " + source["path"])
            evidence.append({"path": str(path), "line": text.count("\n", 0, offset) + 1
                             if offset >= 0 else None, "quote": quote})
        if not evidence:
            raise ValueError("Readable name needs source evidence: " + name)
        public = entry.get("operations", [])
        if not isinstance(public, list) or any(op not in operations for op in public):
            raise ValueError("Readable name refers to an unknown public operation: " + name)
        conn.execute("INSERT INTO aliases VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                     (symbol, name, entry["system"], entry["summary"],
                      "needs_review" if reasons else "documented", json.dumps(reasons),
                      json.dumps(evidence), json.dumps(public)))


def _connect(path):
    path = Path(path).expanduser().resolve()
    if not path.is_file():
        raise ValueError(f"No code index at {path}; run code build first")
    conn = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True)
    conn.row_factory = sqlite3.Row
    try:
        if conn.execute("PRAGMA user_version").fetchone()[0] != SCHEMA:
            raise ValueError("The code index needs rebuilding with code build")
    except BaseException:
        conn.close()
        raise
    return conn


def _meta(conn):
    return {row[0]: json.loads(row[1]) for row in conn.execute("SELECT key, value FROM metadata")}


def _fresh(conn):
    meta = _meta(conn)
    current = _inputs(Path(meta["generated"]), Path(meta["root"]), Path(meta["symbols"]))
    if current != meta["inputs"]:
        raise ValueError("Code or evidence changed; rebuild with code build before using this index")


def build(generated_dir, db_path, force=False, repo_root=ROOT, symbols_path=None):
    generated, root = Path(generated_dir).resolve(), Path(repo_root).resolve()
    symbols = Path(symbols_path).resolve() if symbols_path else root / "modkit/symbols.json"
    inputs = _inputs(generated, root, symbols)
    db_path = Path(db_path).expanduser().resolve()
    if db_path in [Path(item["path"]) for item in inputs]:
        raise ValueError("The database must not replace a source file")
    if db_path.exists() and not force:
        old = None
        try:
            old = _connect(db_path)
            meta = _meta(old)
            if meta["inputs"] == inputs and meta["root"] == str(root):
                return {**status(db_path, verify=False), "up_to_date": True}
        except (ValueError, sqlite3.DatabaseError):
            pass
        finally:
            if old is not None:
                old.close()
    db_path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(prefix="." + db_path.name + ".", suffix=".partial", dir=db_path.parent)
    os.close(descriptor)
    conn = None
    started = time.monotonic()
    try:
        conn = sqlite3.connect(temporary)
        conn.executescript("""
            CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE functions (name TEXT PRIMARY KEY, addr INTEGER UNIQUE, file TEXT,
                line INTEGER, end_line INTEGER, sha256 TEXT, indirect_calls INTEGER);
            CREATE TABLE calls (caller TEXT, callee TEXT, line INTEGER, kind TEXT);
            CREATE TABLE refs (name TEXT, file TEXT, line INTEGER, text TEXT,
                PRIMARY KEY(name, file, line));
            CREATE TABLE aliases (symbol TEXT PRIMARY KEY, name TEXT UNIQUE COLLATE NOCASE,
                system TEXT, summary TEXT, status TEXT, reasons TEXT, evidence TEXT, operations TEXT);
            CREATE TABLE api (name TEXT PRIMARY KEY, description TEXT);
        """)
        for item in inputs:
            if item["kind"] != "generated":
                continue
            functions, calls = _parse(Path(item["path"]))
            conn.executemany("INSERT INTO functions VALUES (?, ?, ?, ?, ?, ?, ?)", functions)
            conn.executemany("INSERT INTO calls VALUES (?, ?, ?, ?)", calls)
        if not conn.execute("SELECT COUNT(*) FROM functions").fetchone()[0]:
            raise ValueError("No generated function definitions found")
        known = {row[0] for row in conn.execute("SELECT name FROM functions")}
        for item in inputs:
            if item["kind"] != "reference":
                continue
            path = Path(item["path"])
            for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                matches = {"sub_" + (m.group(1) or m.group(2)).upper()
                           for m in ADDRESS_RE.finditer(line)} & known
                conn.executemany("INSERT INTO refs VALUES (?, ?, ?, ?)",
                                 [(name, str(path), line_number, line.strip()[:2000]) for name in matches])
        _annotations(conn, root, symbols)
        mapping_file = generated / "simpsons_init.cpp"
        mappings = MAPPING_RE.findall(_code(mapping_file.read_text(encoding="utf-8"))) if mapping_file.is_file() else []
        mapped = {"sub_" + name[4:].upper() for _, name in mappings if re.fullmatch(r"sub_[0-9A-Fa-f]{8}", name)}
        missing, unmapped = sorted(mapped - known), sorted(known - mapped) if mapping_file.is_file() else []
        mismatches = [name for address, name in mappings
                      if re.fullmatch(r"sub_[0-9A-Fa-f]{8}", name) and int(address, 16) != int(name[4:], 16)]
        registration = {"table_present": mapping_file.is_file(), "entries": len(mappings),
                        "address_functions": len(mapped), "runtime_helpers": len(mappings) - len(mapped),
                        "missing_definition_count": len(missing), "missing_definitions": missing[:40],
                        "unmapped_definition_count": len(unmapped), "unmapped_definitions": unmapped[:40],
                        "address_mismatch_count": len(mismatches), "address_mismatches": mismatches[:40],
                        "diagnostics_truncated": any(len(items) > 40 for items in (missing, unmapped, mismatches))}
        if _inputs(generated, root, symbols) != inputs:
            raise ValueError("Source changed during indexing; previous index preserved, retry the build")
        metadata = {"root": str(root), "generated": str(generated), "symbols": str(symbols),
                    "inputs": inputs, "elapsed_seconds": round(time.monotonic() - started, 3)}
        metadata["registration"] = registration
        conn.executemany("INSERT INTO metadata VALUES (?, ?)",
                         [(key, json.dumps(value)) for key, value in metadata.items()])
        conn.executescript("""
            CREATE INDEX idx_caller ON calls(caller);
            CREATE INDEX idx_callee ON calls(callee);
            PRAGMA user_version = 2;
        """)
        conn.commit()
        conn.close()
        conn = None
        os.replace(temporary, db_path)
    finally:
        if conn is not None:
            conn.close()
        Path(temporary).unlink(missing_ok=True)
        Path(temporary + "-journal").unlink(missing_ok=True)
    return {**status(db_path, verify=False), "up_to_date": False}


def status(db_path, verify=True):
    conn = _connect(db_path)
    try:
        result = {"database": str(Path(db_path).resolve()), "schema": SCHEMA}
        for table in ("functions", "calls", "refs", "aliases"):
            result[table] = conn.execute("SELECT COUNT(*) FROM " + table).fetchone()[0]
        result["indirect_calls"] = conn.execute("SELECT COALESCE(SUM(indirect_calls), 0) FROM functions").fetchone()[0]
        result["names_needing_review"] = conn.execute("SELECT COUNT(*) FROM aliases WHERE status != 'documented'").fetchone()[0]
        result["unresolved_direct_targets"] = conn.execute("SELECT COUNT(DISTINCT callee) FROM calls WHERE callee NOT IN (SELECT name FROM functions)").fetchone()[0]
        meta = _meta(conn)
        result["source_files"] = len(meta["inputs"])
        result["elapsed_seconds"] = meta["elapsed_seconds"]
        result["registration"] = meta["registration"]
        if verify:
            try:
                _fresh(conn)
                result["fresh"] = True
            except (ValueError, OSError) as error:
                result.update(fresh=False, reason=str(error))
        return result
    finally:
        conn.close()


def _resolve(conn, query):
    if re.fullmatch(r"(?:0x)?[0-9A-Fa-f]{8}", query):
        query = "sub_" + query.removeprefix("0x").upper()
    row = conn.execute("SELECT f.* FROM functions f LEFT JOIN aliases a ON a.symbol = f.name "
                       "WHERE f.name = ? COLLATE NOCASE OR a.name = ? COLLATE NOCASE", (query, query)).fetchone()
    if not row:
        raise ValueError("Unknown function or readable name: " + query)
    return dict(row)


def _label(conn, row):
    result = dict(row)
    alias = conn.execute("SELECT * FROM aliases WHERE symbol = ?", (result["name"],)).fetchone()
    if alias:
        result["readable"] = {key: alias[key] for key in ("name", "system", "summary", "status")}
        for key in ("reasons", "evidence", "operations"):
            result["readable"][key] = json.loads(alias[key])
    return result


def query(db_path, action, value=None, limit=40):
    if not 1 <= limit <= 500:
        raise ValueError("Code query limit must be between 1 and 500")
    conn = _connect(db_path)
    try:
        _fresh(conn)
        if action == "search":
            if not value or len(value) > 200:
                raise ValueError("Search text must contain 1 to 200 characters")
            needle = "%" + value.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%"
            rows = conn.execute(r"""
                SELECT f.* FROM functions f LEFT JOIN aliases a ON a.symbol = f.name
                WHERE f.name LIKE ? ESCAPE '\' OR a.name LIKE ? ESCAPE '\'
                   OR a.system LIKE ? ESCAPE '\' OR a.summary LIKE ? ESCAPE '\'
                   OR a.operations LIKE ? ESCAPE '\' OR EXISTS
                    (SELECT 1 FROM refs r WHERE r.name = f.name AND r.text LIKE ? ESCAPE '\')
                ORDER BY a.name IS NULL, f.addr LIMIT ?
            """, (needle,) * 6 + (limit + 1,)).fetchall()
            return {"matches": [_label(conn, row) for row in rows[:limit]], "more": len(rows) > limit}
        if action == "gap":
            address = int(value, 16)
            below = conn.execute("SELECT * FROM functions WHERE addr <= ? ORDER BY addr DESC LIMIT 1", (address,)).fetchone()
            above = conn.execute("SELECT * FROM functions WHERE addr > ? ORDER BY addr LIMIT 1", (address,)).fetchone()
            return {"below": _label(conn, below) if below else None,
                    "above": _label(conn, above) if above else None,
                    "note": "Neighboring starts do not prove which function contains an address."}
        function = _label(conn, _resolve(conn, value))
        if action in ("callers", "callees"):
            column, target = ("caller", "callee") if action == "callers" else ("callee", "caller")
            rows = conn.execute(f"SELECT c.{column} AS name, c.line, c.kind, f.file FROM calls c "
                                "JOIN functions f ON f.name = c.caller "
                                f"WHERE c.{target} = ? ORDER BY c.{column}, c.line LIMIT ?",
                                (function["name"], limit + 1)).fetchall()
            return {"function": function, "sites": [_label(conn, row) for row in rows[:limit]],
                    "more": len(rows) > limit, "coverage": "literal direct calls; indirect targets remain unknown"}
        references = conn.execute("SELECT file, line, text FROM refs WHERE name = ? ORDER BY file, line LIMIT ?",
                                  (function["name"], limit + 1)).fetchall()
        function["references"] = [dict(row) for row in references[:limit]]
        function["more_references"] = len(references) > limit
        return function
    finally:
        conn.close()


def print_result(result):
    """Stable JSON is also accepted by editors and other creator automation."""
    print(json.dumps(result, indent=2, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated-dir", default=DEFAULT_GENERATED_DIR)
    parser.add_argument("--repo-root", default=str(ROOT))
    parser.add_argument("--symbols", help="reviewed symbol catalog (default: modkit/symbols.json)")
    parser.add_argument("--db", default=DEFAULT_DB)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--limit", type=int, default=40)
    parser.add_argument("command", choices=["build", "status", "search", "show", "addr", "gap", "name", "callers", "callees"])
    parser.add_argument("arg", nargs="?")
    args = parser.parse_args()
    try:
        if args.command == "build":
            result = build(args.generated_dir, args.db, args.force, args.repo_root, args.symbols)
        elif args.command == "status":
            result = status(args.db)
        else:
            if not args.arg:
                parser.error(args.command + " requires an argument")
            result = query(args.db, args.command, args.arg, args.limit)
        print_result(result)
    except (ValueError, OSError, sqlite3.DatabaseError) as error:
        parser.exit(1, "code index: " + str(error) + "\n")


if __name__ == "__main__":
    main()
