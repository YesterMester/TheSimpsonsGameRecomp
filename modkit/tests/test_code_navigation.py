"""Code navigation must stay useful when sources, hooks and names change."""
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/modkit"))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/codegraph"))
import modkit as creator_cli
import code_navigation
import packages
import recomp_catalog as catalog


SAMPLE = """// A comment is not a function.
DEFINE_REX_FUNC(sub_82230000) {
    // sub_82230020(ctx, base);
    const char* label = "sub_82230020(ctx, base)";
    /* sub_82230020(ctx, base); */
    sub_82230000(ctx, base);
    sub_82230010(ctx, base);
    __imp__sub_82230010(ctx, base);
    REX_CALL_INDIRECT_FUNC(ctx.ctr.u32);
}
DEFINE_REX_FUNC(sub_82230010) {
    return;
}
DEFINE_REX_FUNC(sub_82230020) {
    return;
}
"""


class CodeNavigationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.generated = self.root / "simpsons/generated/default"
        self.generated.mkdir(parents=True)
        self.source = self.generated / "simpsons_recomp.0.cpp"
        self.source.write_text(SAMPLE, encoding="utf-8")
        self.notes = self.root / "simpsons/re/players.md"
        self.notes.parent.mkdir(parents=True)
        self.notes.write_text("sub_82230010 is the documented health check.\n", encoding="utf-8")
        self.symbols = self.root / "modkit/symbols.json"
        self.symbols.parent.mkdir()
        (self.symbols.parent / "api.json").write_text(json.dumps({"operations": [
            {"name": "players.health", "description": "Read health."}]}), encoding="utf-8")
        functions, _ = catalog._parse(self.source)
        self.alias = {"symbol": "sub_82230010", "name": "Player.Health",
                      "system": "players", "summary": "Read a player's health.",
                      "definition_sha256": functions[1][5],
                      "evidence": [{"path": "simpsons/re/players.md",
                                    "quote": "sub_82230010 is the documented health check."}],
                      "operations": ["players.health"]}
        self.write_aliases([self.alias])
        self.db = self.root / "index.sqlite"

    def write_aliases(self, values):
        self.symbols.write_text(json.dumps({"schema": 1, "functions": values}), encoding="utf-8")

    def build(self, **kwargs):
        return catalog.build(self.generated, self.db, repo_root=self.root, **kwargs)

    def test_comments_strings_recursion_and_original_calls(self):
        result = self.build()
        self.assertEqual((result["functions"], result["calls"], result["indirect_calls"]), (3, 3, 1))
        calls = catalog.query(self.db, "callees", "82230000")["sites"]
        self.assertEqual([r["name"] for r in calls],
                         ["sub_82230000", "sub_82230010", "sub_82230010"])
        self.assertEqual([r["kind"] for r in calls], ["direct", "direct", "original"])
        self.assertEqual([r["line"] for r in calls], [6, 7, 8])

    def test_named_lookup_and_api_evidence(self):
        self.build()
        result = catalog.query(self.db, "show", "player.health")
        self.assertEqual(result["name"], "sub_82230010")
        self.assertEqual(result["readable"]["status"], "documented")
        self.assertEqual(result["readable"]["operations"], ["players.health"])
        self.assertEqual(result["readable"]["evidence"][0]["line"], 1)
        self.assertEqual(catalog.query(self.db, "search", "health")["matches"][0]["name"],
                         "sub_82230010")
        self.assertEqual(self.source.read_text(encoding="utf-8"), SAMPLE)

    def test_content_changes_with_preserved_mtime_are_stale(self):
        self.build()
        before = self.source.stat()
        self.source.write_text(SAMPLE.replace("    return;", "    /* reviewed change */ return;", 1))
        os.utime(self.source, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertFalse(catalog.status(self.db)["fresh"])
        with self.assertRaisesRegex(ValueError, "rebuild"):
            catalog.query(self.db, "show", "Player.Health")
        self.build()
        result = catalog.query(self.db, "show", "Player.Health")
        self.assertEqual(result["readable"]["status"], "needs_review")
        self.assertIn("generated definition changed", result["readable"]["reasons"])

    def test_deleted_sources_and_added_sources_are_stale(self):
        self.build()
        extra = self.generated / "simpsons_recomp.1.cpp"
        extra.write_text("DEFINE_REX_FUNC(sub_82230030) {\n    return;\n}\n")
        self.assertFalse(catalog.status(self.db)["fresh"])
        self.assertEqual(self.build()["functions"], 4)
        extra.unlink()
        self.assertFalse(catalog.status(self.db)["fresh"])
        self.assertEqual(self.build()["functions"], 3)

    def test_unchanged_build_reuses_database(self):
        self.build()
        before = self.db.stat().st_mtime_ns
        self.assertTrue(self.build()["up_to_date"])
        self.assertEqual(self.db.stat().st_mtime_ns, before)

    def test_changed_evidence_requires_review(self):
        self.build()
        self.notes.write_text("The health interpretation needs more work.\n")
        self.build()
        result = catalog.query(self.db, "show", "Player.Health")
        self.assertEqual(result["readable"]["status"], "needs_review")
        self.assertIsNone(result["readable"]["evidence"][0]["line"])

    def test_failed_build_keeps_the_last_database(self):
        self.build()
        before = self.db.read_bytes()
        self.source.write_text("DEFINE_REX_FUNC(sub_82230000) {\n")
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            self.build()
        self.assertEqual(self.db.read_bytes(), before)
        self.assertEqual(list(self.root.glob("*.partial")), [])

    def test_source_change_during_build_does_not_publish(self):
        self.build()
        before = self.db.read_bytes()
        parse = catalog._parse
        def modify(path):
            result = parse(path)
            self.notes.write_text("Changed while the index was being built.\n")
            return result
        with mock.patch.object(catalog, "_parse", side_effect=modify):
            with self.assertRaisesRegex(ValueError, "Source changed during"):
                self.build(force=True)
        self.assertEqual(self.db.read_bytes(), before)

    def test_duplicate_names_and_unknown_api_are_rejected(self):
        duplicate = {**self.alias, "symbol": "sub_82230020"}
        self.write_aliases([self.alias, duplicate])
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            self.build()
        self.write_aliases([{**self.alias, "operations": ["made.up"]}])
        with self.assertRaisesRegex(ValueError, "unknown public operation"):
            self.build()

    def test_evidence_cannot_escape_the_checkout(self):
        self.write_aliases([{**self.alias, "evidence": [
            {"path": "../outside.md", "quote": "Untrusted identity"}]}])
        with self.assertRaisesRegex(ValueError, "within the source checkout"):
            self.build()

    def test_no_implicit_database_or_mods_creation(self):
        missing = self.root / "missing.sqlite"
        with self.assertRaisesRegex(ValueError, "No code index"):
            catalog.status(missing)
        self.assertFalse(missing.exists())
        with mock.patch.object(creator_cli, "resolve_paths") as paths, \
                contextlib.redirect_stdout(io.StringIO()) as output:
            creator_cli.main(["code", "--db", str(self.db), "build", "--source", str(self.root)])
        paths.assert_not_called()
        self.assertEqual(json.loads(output.getvalue())["functions"], 3)

    def test_query_limits_and_literal_search(self):
        self.build()
        result = catalog.query(self.db, "search", "sub_", 1)
        self.assertEqual(len(result["matches"]), 1)
        self.assertTrue(result["more"])
        self.assertEqual(catalog.query(self.db, "search", "%")["matches"], [])
        with self.assertRaisesRegex(ValueError, "between 1 and 500"):
            catalog.query(self.db, "search", "health", 100000)

    def test_neighbors_do_not_claim_function_extents(self):
        self.build()
        result = catalog.query(self.db, "gap", "0x82230008")
        self.assertEqual(result["below"]["name"], "sub_82230000")
        self.assertEqual(result["above"]["name"], "sub_82230010")
        self.assertIn("do not prove", result["note"])
        with self.assertRaisesRegex(ValueError, "Unknown function"):
            catalog.query(self.db, "show", "0x82230008")

    def test_database_cannot_replace_source(self):
        before = self.source.read_bytes()
        with self.assertRaisesRegex(ValueError, "must not replace"):
            catalog.build(self.generated, self.source, repo_root=self.root)
        self.assertEqual(self.source.read_bytes(), before)

    def test_registration_audit_keeps_helpers_and_gaps_explicit(self):
        (self.generated / "simpsons_init.cpp").write_text(
            "PPCFuncMapping PPCFuncMappings[] = {\n"
            "    { 0x82230000, sub_82230000 },\n"
            "    { 0x82230030, sub_82230030 }, // manually added\n"
            "    { 0x82B70000, __imp__XamInputGetState },\n"
            "    // { 0x8223FFFF, sub_8223FFFF },\n"
            "};\n")
        audit = self.build()["registration"]
        self.assertEqual((audit["entries"], audit["address_functions"], audit["runtime_helpers"]), (3, 2, 1))
        self.assertEqual(audit["missing_definitions"], ["sub_82230030"])
        self.assertEqual(audit["unmapped_definitions"], ["sub_82230010", "sub_82230020"])
        self.assertEqual(audit["address_mismatches"], [])

    def test_packaged_tools_can_omit_the_generated_code_indexer(self):
        with mock.patch.object(code_navigation, "CATALOG_DIR", self.root / "not-shipped"):
            args = creator_cli.build_parser().parse_args(["list"])
            self.assertEqual(args.command, "list")
            with self.assertRaisesRegex(packages.ModError, "source checkout"):
                code_navigation._catalog()


if __name__ == "__main__":
    unittest.main()
