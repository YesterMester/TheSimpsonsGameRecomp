#!/usr/bin/env python3
"""Synthetic text resources, real editor HTTP and normal mod packaging."""
import hashlib
import http.client
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/modkit"))
import packages
import translations as t


def resource(strings, identifiers=None, wide=False):
    count, columns = len(strings), 2 if identifiers else 1
    body = bytearray(32 + count * 4 * (1 + columns))
    body[:4] = b"2HCL"
    struct.pack_into(">4I", body, 8, 0x80000001 if wide else 1, 0, count, columns)
    for i in range(count):
        struct.pack_into(">I", body, 32 + i * 4, i + 1)
    for i, string in enumerate([*strings, *(identifiers or [])]):
        struct.pack_into(">I", body, 32 + (count + i) * 4, len(body))
        body.extend(string.encode("utf-16-be" if wide else "cp1252") + (b"\0\0" if wide else b"\0"))
    struct.pack_into(">I", body, 4, len(body))
    header = bytearray(96)
    struct.pack_into("<I", header, 0, 0x716)
    struct.pack_into(">2I", header, 12, 80, 16)
    header[20:32] = b"test.en.LH2\0"
    struct.pack_into(">I", header, 52, 4)
    header[56:60] = b"LH2\0"
    struct.pack_into(">I", header, 60, 4)
    struct.pack_into(">I", header, 68, 4)
    struct.pack_into(">I", header, 80, len(body))
    result = header + body
    result.extend(b"\0" * (-len(result) % 4))
    struct.pack_into("<I", result, 4, len(result) - 12)
    return bytes(result)


class Translations(unittest.TestCase):
    def test_no_edit_is_byte_identical(self):
        original = resource(["Continue", "Press [RDDown] %1 {name}", "caf\u00e9"])
        table = t.TextTable(original)
        self.assertEqual(table.rebuild({}), original)
        self.assertEqual(table.rebuild({key: None for key in table.keys}), original)

    def test_rebuild_preserves_identifiers_and_following_alignment(self):
        original = resource(["Hello {name}", "Press [RDDown]"], ["voice_00001", "voice_00002"])
        table = t.TextTable(original)
        rebuilt = table.rebuild({"00000001": "Bonjour {name}, bienvenue !"})
        checked = t.TextTable(rebuilt)
        self.assertEqual(checked.strings["00000001"], "Bonjour {name}, bienvenue !")
        self.assertEqual(checked.strings["00000002"], "Press [RDDown]")
        self.assertEqual(checked.context, table.context)
        self.assertEqual((len(rebuilt) - len(original)) % 16, 0)
        # Unchanged text and dialogue IDs keep their original relative offsets.
        at = table.base + 32 + len(table.keys) * 4
        self.assertEqual(original[at + 4:at + 16], rebuilt[at + 4:at + 16])

    def test_no_lossy_character_replacement(self):
        table = t.TextTable(resource(["Test"]))
        with self.assertRaisesRegex(packages.ModError, "Unicode font"):
            table.rebuild({"00000001": "\u65e5\u672c\u8a9e"})
        wide = t.TextTable(resource(["Test"], wide=True))
        self.assertEqual(t.TextTable(wide.rebuild({"00000001": "\u65e5\u672c\u8a9e"})).strings["00000001"], "\u65e5\u672c\u8a9e")

    def test_placeholders_and_button_tags(self):
        original = "[RDDown] %1 {name} %02d %1$s %%"
        t.validate_text(original, "%1$s {name}: [RDDown] %02d %1 %%")
        for wrong in ("[A] %1 {name} %02d %1$s %%", "[RDDown] %1 %02d %1$s %%", "[RDDown] %1 {name} %02f %1$s %%"):
            with self.assertRaises(packages.ModError):
                t.validate_text(original, wrong)

    def test_reject_malformed_tables_before_writing(self):
        original = resource(["Text"])
        for offset, value in ((12, 0xffffffff), (96 + 16, 999999), (96 + 36, 1), (52, 0xfffffff0)):
            bad = bytearray(original)
            struct.pack_into(">I", bad, offset, value)
            with self.assertRaises(packages.ModError):
                t.TextTable(bytes(bad))

    def test_mod_translation_package_is_partial_and_disabled(self):
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as temporary:
            mods = Path(temporary) / "mods"
            source = mods / "Tools"
            packages.write_json(source / "mod.json", {"id": "test.tools", "name": "Tools", "version": "1.0.0",
                "api": 1, "kind": "data", "save_mode": "stateless", "default_locale": "en", "locales": {"en": "locales/en.json"}})
            packages.write_json(source / "locales/en.json", {"title": "Tools", "health": "Health {value}"})
            args = Namespace(name="French", locale="fr", id="test.french", author="Translator", of="test.tools")
            t.new(args, None, mods)
            folder = mods / "French"
            path = folder / t.WORKSPACE / "strings.json"
            data = packages.read_json(path)
            data["entries"]["title"]["translation"] = "Outils"
            packages.write_json(path, data)
            t.build(args, None, mods)
            self.assertEqual(packages.read_json(folder / "locales/strings.json"), {"title": "Outils"})
            result = t.check_project(folder)
            self.assertEqual((result["translated"], result["remaining"]), (1, 1))
            archive = packages.pack(folder, Path(temporary) / "translation.zip")
            installed = packages.install(archive, Path(temporary) / "installed")
            self.assertEqual(installed["author"], "Translator")
            self.assertEqual(installed["dependencies"], {"test.tools": "1.0.0"})
            self.assertEqual(packages.profile(Path(temporary) / "installed")["enabled"], [])
            self.assertFalse((Path(installed["folder"]) / t.WORKSPACE).exists())

    def test_game_translation_build_needs_no_archive_replacements(self):
        from argparse import Namespace
        with tempfile.TemporaryDirectory() as temporary:
            mods = Path(temporary)
            folder = mods / "French"
            workspace = folder / t.WORKSPACE
            packages.write_json(folder / "mod.json", {"id": "test.french", "name": "French", "version": "1.0.0",
                "api": 1, "kind": "data", "save_mode": "stateless", "author": "Translator"})
            packages.write_json(workspace / "project.json", {"version": 1, "kind": "game", "locale": "fr", "base": "en",
                "catalogs": [{"file": "text.json", "body_sha256": "ab" * 32, "targets": []}]})
            data = t.catalog({"00000001": "Press [RDDown] %1", "00000002": "Original"})
            data["entries"]["00000001"]["translation"] = "Appuyer [RDDown] %1"
            packages.write_json(workspace / "text.json", data)
            t.build(Namespace(name="French", stream_tool=None), None, mods)
            built = packages.read_json(folder / "locales/game-text.json")
            self.assertEqual(built["tables"]["ab" * 32], {"00000001": {"source": "Press [RDDown] %1", "text": "Appuyer [RDDown] %1"}})
            self.assertFalse((folder / "gamedata").exists())
            packages.manifest(folder)


if __name__ == "__main__":
    unittest.main()
