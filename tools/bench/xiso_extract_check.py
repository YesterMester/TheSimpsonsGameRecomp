#!/usr/bin/env python3
"""Check the launcher's built-in XDVDFS extractor and its install fallback.

Builds small synthetic Xbox disc images (no game data), extracts them with
launcher/xiso_extract.py, rejects damaged and unsafe images, and runs the
launcher's install with an extract-xiso that cannot start (WinError 4551, #54)
or is missing, which must fall back to the built-in extractor.
"""

import importlib.util
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "launcher"))
import xiso_extract  # noqa: E402

SECTOR = 2048
HEADER = 32 * SECTOR  # partition + 0x10000


class FilledEmpty(dict):
    """An empty folder stored the way mastering tools often write it: one sector of 0xff."""


def sectors(data):
    return max(1, -(-len(data) // SECTOR))


def build_image(tree):
    """An XDVDFS image (partition at 0) of tree: {name: bytes or nested dict}.

    Every directory table is a right-linked chain (entry i's right subtree is
    entry i + 1), which is a valid, if unbalanced, XDVDFS tree. Names are not
    checked, so tests can store names a real disc would never have."""
    blocks = []

    def allocate(data):
        sector = 33 + sum(sectors(block) for block in blocks)
        blocks.append(data)
        return sector

    def table(entries):
        if not entries:
            if isinstance(entries, FilledEmpty):
                return allocate(b"\xff" * SECTOR), SECTOR
            return 0, 0
        children = []
        for name, value in sorted(entries.items()):
            if isinstance(value, dict):
                children.append((name, *table(value), 0x10))
            else:
                children.append((name, allocate(value), len(value), 0x20))
        offsets, at = [], 0
        for name, *_ in children:
            offsets.append(at)
            at += (14 + len(name.encode("latin-1")) + 3) // 4 * 4
        data = bytearray(at)
        for index, (name, sector, size, attributes) in enumerate(children):
            right = offsets[index + 1] // 4 if index + 1 < len(children) else 0
            encoded = name.encode("latin-1")
            struct.pack_into("<HHIIBB", data, offsets[index], 0, right, sector, size, attributes, len(encoded))
            data[offsets[index] + 14:offsets[index] + 14 + len(encoded)] = encoded
        return allocate(bytes(data)), len(data)

    root_sector, root_size = table(tree)
    image = bytearray((33 + sum(sectors(block) for block in blocks)) * SECTOR)
    image[HEADER:HEADER + 20] = xiso_extract.MEDIA
    image[HEADER + 2028:HEADER + 2048] = xiso_extract.MEDIA
    struct.pack_into("<II", image, HEADER + 20, root_sector, root_size)
    sector = 33
    for block in blocks:
        image[sector * SECTOR:sector * SECTOR + len(block)] = block
        sector += sectors(block)
    return image


def write_image(path, image, partition=0):
    """Sector numbers are relative to the partition, so a partition-0 image placed
    at a later offset is the same disc in the XGD2/XGD3 layout (sparse where the
    file system allows it)."""
    with open(path, "wb") as output:
        output.seek(partition)
        output.write(image)
    return path


def root_table(image):
    sector, size = struct.unpack_from("<II", image, HEADER + 20)
    return sector * SECTOR, size


GAME = {"default.xex": b"synthetic executable fixture\0not a game\n",
        "movies": {"en": {"intro movie.vp6": bytes(range(256)) * 20}},
        "empty folder": {},
        "empty filled folder": FilledEmpty(),
        "Nested Dir": {"deeper": {"note.txt": b"Nested file extraction works.\n", "zero.bin": b""}}}
GAME_FILES = {"default.xex": GAME["default.xex"],
              "movies/en/intro movie.vp6": bytes(range(256)) * 20,
              "Nested Dir/deeper/note.txt": b"Nested file extraction works.\n",
              "Nested Dir/deeper/zero.bin": b""}


def files(target):
    return {p.relative_to(target).as_posix(): p.read_bytes() for p in Path(target).rglob("*") if p.is_file()}


class Extractor(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xiso-check-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def extract(self, image, partition=0):
        case = Path(tempfile.mkdtemp(prefix="case ", dir=self.root))
        iso = write_image(case / "disc with spaces.iso", image, partition)
        target = case / "out folder"
        target.mkdir()
        return case, target, xiso_extract.extract(iso, target)

    def test_files_folders_and_empty_folders(self):
        lines = []
        case = Path(tempfile.mkdtemp(dir=self.root))
        iso = write_image(case / "disc.iso", build_image(GAME))
        (case / "out").mkdir()
        count, size = xiso_extract.extract(iso, case / "out", lines.append)
        self.assertEqual(files(case / "out"), GAME_FILES)
        self.assertTrue((case / "out/empty folder").is_dir())
        self.assertTrue((case / "out/empty filled folder").is_dir())
        self.assertEqual((count, size), (4, sum(map(len, GAME_FILES.values()))))
        self.assertIn("Extracted 4 files", lines[-1])

    def test_xgd2_and_xgd3_partitions(self):
        for partition in (0x0FD90000, 0x02080000):
            with self.subTest(hex(partition)):
                _, target, _ = self.extract(build_image(GAME), partition)
                self.assertEqual(files(target), GAME_FILES)

    def test_rejects_damaged_and_unsafe_images(self):
        good = build_image(GAME)
        cases = {"not an Xbox image": bytes(len(good)), "truncated": good[:-SECTOR]}
        damaged = bytearray(good)
        damaged[HEADER + 2040] ^= 0xFF
        cases["damaged second header magic"] = damaged
        for name in ("..", ".", "a/b", "a\\b", "c:", "bad\x01", "trailing.", "trailing "):
            cases[f"file name {name!r}"] = build_image({"default.xex": b"x", name: b"data"})
        cases["same name twice"] = build_image({"File.txt": b"1", "file.TXT": b"2"})
        # Entry chain a -> b -> c -> b.
        entry_loop = build_image({"a": b"1", "b": b"2", "c": b"3"})
        position, _ = root_table(entry_loop)
        struct.pack_into("<H", entry_loop, position + 32 + 2, 16 // 4)
        cases["entry loop"] = entry_loop
        # A folder whose table is the root table.
        folder_loop = build_image({"a": {"x": b"1"}})
        position, size = root_table(folder_loop)
        struct.pack_into("<II", folder_loop, position + 4, position // SECTOR, size)
        cases["folder loop"] = folder_loop
        outside = build_image({"default.xex": b"x"})
        position, _ = root_table(outside)
        struct.pack_into("<I", outside, position + 4, 1 << 30)
        cases["file outside the image"] = outside
        for label, image in cases.items():
            with self.subTest(label):
                case = Path(tempfile.mkdtemp(dir=self.root))
                iso = write_image(case / "disc.iso", image)
                (case / "out").mkdir()
                with self.assertRaises(xiso_extract.XisoError):
                    xiso_extract.extract(iso, case / "out")
                self.assertEqual(sorted(os.listdir(case)), ["disc.iso", "out"],
                                 "nothing may be written outside the target folder")


class InstallFallback(unittest.TestCase):
    """run_install() with an extract-xiso that cannot start or is missing."""

    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="xiso-install-")
        base = Path(cls.temp.name)
        (base / "launcher").mkdir()
        shutil.copyfile(ROOT / "launcher/launcher.py", base / "launcher/launcher.py")
        spec = importlib.util.spec_from_file_location("install_launcher", base / "launcher/launcher.py")
        cls.app = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.app)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def install(self, image, extractor_exists, popen_error=None):
        root = Path(tempfile.mkdtemp(prefix="install ", dir=self.temp.name))
        iso = write_image(root / "The Simpsons Game.iso", image)
        extractor = root / "extract-xiso.exe"
        if extractor_exists:
            extractor.write_bytes(b"stand-in")
        popen = mock.Mock(side_effect=popen_error or AssertionError("extract-xiso must not run"))
        with mock.patch.object(self.app, "ROOT", root), \
                mock.patch.object(self.app, "GAMEDATA", root / "gamedata"), \
                mock.patch.object(self.app, "EXTRACT_XISO", extractor), \
                mock.patch.object(self.app, "FROZEN", False), \
                mock.patch.object(self.app, "generate_art", lambda force=False: None), \
                mock.patch.object(self.app.subprocess, "Popen", popen):
            self.app.run_install(str(iso))
        self.assertFalse(self.app.install_state["running"])
        self.assertEqual(popen.call_count, 1 if extractor_exists else 0)
        return root, list(self.app.install_state["log"]), self.app.install_state["ok"]

    def test_blocked_extractor_falls_back(self):
        blocked = OSError(4551, "An Application Control policy has blocked this file")
        blocked.winerror = 4551
        root, log, ok = self.install(build_image(GAME), True, blocked)
        self.assertTrue(ok, log)
        self.assertTrue(any("could not start" in line for line in log), log)
        self.assertEqual(files(root / "gamedata"), GAME_FILES)
        self.assertFalse((root / "gamedata_extracting").exists())

    def test_missing_extractor_falls_back(self):
        root, log, ok = self.install(build_image(GAME), False)
        self.assertTrue(ok, log)
        self.assertTrue(any("is missing" in line for line in log), log)
        self.assertEqual(files(root / "gamedata"), GAME_FILES)

    def test_bad_image_keeps_the_installed_game(self):
        root = None
        image = build_image({"default.xex": b"x", "..": b"escape"})
        blocked = OSError(4551, "blocked")
        root, log, ok = self.install(image, True, blocked)
        self.assertFalse(ok)
        self.assertTrue(log[-1].startswith("ERROR: The ISO contains an unsupported file name"), log)
        self.assertFalse((root / "gamedata").exists())
        self.assertFalse((root / "escape").exists())

    def test_not_the_simpsons_game(self):
        root, log, ok = self.install(build_image({"default.xex": b"x"}), False)
        self.assertFalse(ok)
        self.assertIn("doesn't look like The Simpsons Game", log[-1])
        self.assertFalse((root / "gamedata").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
