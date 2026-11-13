#!/usr/bin/env python3
"""Check Steam shortcut discovery using disposable client profiles."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'launcher'))
import steam_shortcut as steam


def text(key, value):
    return b'\1' + key.encode() + b'\0' + value.encode() + b'\0'


def shortcuts(command):
    return (b'\0shortcuts\0\x000\0' + text('AppName', 'The Simpsons Game')
            + text('Exe', command) + text('LaunchOptions', '')
            + b'\2appid\0' + struct.pack('<I', 0xDEADBEEF)
            + b'\0tags\0' + text('0', 'Games') + b'\10\10\10\10')


class Shortcuts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / 'config').mkdir()
        (self.root / 'registry.vdf').write_text('"ActiveUser" "0"')
        self.profile = self.root / 'userdata/1/config'
        self.profile.mkdir(parents=True)
        (self.root / 'config/loginusers.vdf').write_text(
            '"users" { "76561197960265729" { "MostRecent" "1" "Timestamp" "2" } }')
        self.target = self.root / 'a folder #1/launcher.sh'
        self.patcher = mock.patch.object(steam, 'steam_roots', return_value=[self.root])
        self.patcher.start()
        self.addCleanup(self.patcher.stop)

    def test_named_shortcut_and_spaces_are_found_without_changing_the_file(self):
        file = self.profile / 'shortcuts.vdf'
        data = shortcuts('"' + str(self.target) + '"')
        file.write_bytes(data)
        self.assertEqual(steam.existing_shortcut([self.target]), 'The Simpsons Game')
        self.assertEqual(file.read_bytes(), data)
        self.assertIsNone(steam.existing_shortcut([self.root / 'different.sh']))

    def test_a_different_accounts_shortcut_does_not_suppress_registration(self):
        other = self.root / 'userdata/2/config'
        other.mkdir(parents=True)
        (other / 'shortcuts.vdf').write_bytes(shortcuts('"' + str(self.target) + '"'))
        self.assertIsNone(steam.existing_shortcut([self.target]))

    def test_live_account_takes_priority_over_last_login(self):
        (self.profile / 'shortcuts.vdf').write_bytes(shortcuts('"' + str(self.target) + '"'))
        (self.root / 'registry.vdf').write_text('"ActiveUser" "2"')
        self.assertIsNone(steam.existing_shortcut([self.target]))

    def test_corrupt_or_unrecognized_data_is_not_treated_as_an_existing_game(self):
        file = self.profile / 'shortcuts.vdf'
        for data in (b'', b'\0shortcuts\0\x00', b'\xffweird\0', shortcuts(str(self.target))[:-2]):
            file.write_bytes(data)
            self.assertIsNone(steam.existing_shortcut([self.target]))


if __name__ == '__main__':
    unittest.main(verbosity=2)
