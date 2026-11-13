#!/usr/bin/env python3
"""Exercise real native widgets, shared catalogs and installable language ZIPs."""
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'launcher'))
import mod_packages as packages
import mod_projects
import mod_translations as translations
from argparse import Namespace

# The editor is a Qt window. The release checks run on a plain Python, so the
# test skips there instead of failing the whole run; it runs wherever the
# launcher's PySide6 is installed.
try:
    from PySide6.QtWidgets import QApplication, QDialog
    from modkit_translations import NewTranslation, TranslationEditor
except ImportError as error:
    QT_MISSING = str(error)
else:
    QT_MISSING = ''


@unittest.skipIf(QT_MISSING, 'needs PySide6: ' + QT_MISSING)
class TranslationWorkspace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.mods = Path(self.temporary.name) / 'mods'
        source = self.mods / 'source'
        packages.write_json(source / 'mod.json', dict(id='test.source', name='Source', version='1.0.0',
            api=1, kind='data', save_mode='stateless', default_locale='en', locales={'en': 'locales/en.json'}))
        packages.write_json(source / 'locales/en.json', {'title': 'Hello {name} [RDDown]', 'other': 'Keep me'})
        self.source = source / 'locales/en.json'
        self.original = self.source.read_bytes()
        self.folder = translations.new(Namespace(name='French', id='test.french', folder='test.french',
            author='Translator', locale='fr', base='en', of='test.source'), None, self.mods)
        translations.build(Namespace(name=str(self.folder)), None, self.mods)
        self.editor = TranslationEditor(self.folder)

    def tearDown(self):
        self.editor.dirty = False
        self.editor.close()
        self.editor.deleteLater()
        self.app.processEvents()
        self.assertEqual(self.source.read_bytes(), self.original)
        self.temporary.cleanup()

    def translate_title(self, value):
        row = next(i for i in range(self.editor.table.rowCount())
                   if self.editor.table.item(i, 0).data(256) == 'title')
        self.editor.table.setCurrentCell(row, 1)
        self.editor.fallback.setChecked(False)
        self.editor.edit.setPlainText(value)
        self.app.processEvents()

    def test_native_editor_exports_partial_disabled_credited_pack(self):
        self.translate_title('Bonjour {name} [RDDown]')
        output = self.mods / 'exports/test.french.zip'
        with patch('modkit_translations.QFileDialog.getSaveFileName', return_value=(str(output), '')):
            self.editor.export()
        self.assertTrue(output.is_file())
        imported = packages.install(output, Path(self.temporary.name) / 'installed')
        strings = packages.read_json(Path(imported['folder']) / 'locales/strings.json')
        self.assertEqual(strings, {'title': 'Bonjour {name} [RDDown]'})
        self.assertEqual(imported['author'], 'Translator')
        self.assertFalse((Path(imported['folder']) / '.translations').exists())
        self.assertEqual(packages.profile(Path(self.temporary.name) / 'installed')['enabled'], [])
        self.assertTrue(mod_projects.describe(self.folder)['translation'])
        self.assertIn('1 / 2', self.editor.progress.text())

    def test_tag_error_is_a_saved_draft_but_cannot_export(self):
        self.translate_title('Bonjour {name} [WrongButton]')
        self.assertTrue(self.editor.save())
        self.assertIn('Keep button tags', self.editor.messages.text())
        with patch('modkit_translations.QFileDialog.getSaveFileName') as picker:
            self.editor.export()
            picker.assert_not_called()
        self.assertTrue(translations.check_project(self.folder)['errors'])

    def test_stale_edit_keeps_the_other_editors_work(self):
        self.translate_title('Bonjour {name} [RDDown]')
        source = translations.read_catalog(self.folder, 'strings.json')
        values = {key: entry['translation'] for key, entry in source['catalog']['entries'].items()}
        values['other'] = 'Autre'
        translations.save_catalog(self.folder, 'strings.json', values, source['revision'])
        with patch('modkit_translations.QMessageBox.warning'):
            self.assertFalse(self.editor.save())
        actual = translations.read_catalog(self.folder, 'strings.json')['catalog']['entries']
        self.assertEqual(actual['other']['translation'], 'Autre')
        self.assertIsNone(actual['title']['translation'])

    def test_new_language_dialog_generates_identity_and_keeps_credit(self):
        dialog = NewTranslation(self.mods)
        dialog.locale.setText('pt-BR'); dialog.author.setText('Creator')
        dialog.source.setCurrentIndex(dialog.source.findData('test.source'))
        dialog.accept()
        self.assertEqual(dialog.result(), QDialog.Accepted)
        values = dialog.values()
        self.assertEqual((values['locale'], values['id'], values['author']), ('pt-br', 'creator.pt-br', 'Creator'))
        self.assertEqual(values['of'], 'test.source')
        dialog.deleteLater()


if __name__ == '__main__':
    unittest.main()
