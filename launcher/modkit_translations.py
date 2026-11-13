"""Native desktop language-pack authoring on the shared translation backend."""
from pathlib import Path
import re

from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QCheckBox, QComboBox, QDialog, QDialogButtonBox,
    QFileDialog, QFormLayout, QHBoxLayout, QHeaderView, QLabel, QLineEdit,
    QMessageBox, QPlainTextEdit, QPushButton, QTableWidget, QTableWidgetItem,
    QVBoxLayout)

if __package__:
    from . import mod_packages as packages, mod_translations as translations
else:
    import mod_packages as packages
    import mod_translations as translations


class NewTranslation(QDialog):
    def __init__(self, mods, parent=None):
        super().__init__(parent)
        self.setWindowTitle('New language pack'); self.setObjectName('newTranslationDialog')
        self.resize(550, 370)
        layout = QVBoxLayout(self); form = QFormLayout()
        self.source = QComboBox(); self.source.setObjectName('translationSource')
        self.source.addItem('The Simpsons Game', '')
        for mod in packages.installed(Path(mods)).values():
            base = mod.get('default_locale', 'en')
            if base in mod.get('locales', {}):
                self.source.addItem(mod['name'], mod['id'])
        self.locale = QLineEdit(); self.locale.setObjectName('translationLocale')
        self.locale.setPlaceholderText('fr, pt-BR, es-MX…')
        self.base = QLineEdit('en'); self.base.setObjectName('translationBase')
        self.name = QLineEdit(); self.name.setObjectName('translationName')
        self.name.setPlaceholderText('Name shown in the Mods tab')
        self.author = QLineEdit(); self.author.setObjectName('translationAuthor')
        self.identifier = QLineEdit(); self.identifier.setObjectName('translationId')
        self.identifier.setPlaceholderText('Optional — generated from your name and language')
        for title, field in [('Translate', self.source), ('Target language code', self.locale),
                             ('Installed source language', self.base), ('Pack name', self.name),
                             ('Translator credit', self.author), ('Stable pack id', self.identifier)]:
            form.addRow(title, field)
        layout.addLayout(form)
        hint = QLabel('Modkit reads your installed text into an editable catalog. Translate as much '
                      'as you like; missing entries keep the original text. The ZIP contains your '
                      'translations and credit, without game archives.')
        hint.setWordWrap(True); layout.addWidget(hint)
        self.source.currentIndexChanged.connect(lambda: self.base.setEnabled(not self.source.currentData()))
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText('Create language pack')
        buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def values(self):
        language = translations.locale(self.locale.text().strip())
        author = self.author.text().strip()
        slug = re.sub(r'[^a-z0-9_.-]+', '-', author.lower()).strip('.-_') or 'translator'
        return dict(name=self.name.text().strip() or language + ' language pack', locale=language,
                    base='en' if self.source.currentData() else translations.locale(self.base.text().strip()), author=author,
                    id=self.identifier.text().strip() or (slug[:65] + '.' + language)[:96],
                    of=self.source.currentData() or None)

    def accept(self):
        try:
            value = self.values()
            if not value['author'] or len(value['author'].encode('utf-8')) > 160 or '\0' in value['author']:
                raise packages.ModError('Enter the translator name to credit in the package')
            if not packages.ID.fullmatch(value['id']) or len(value['name'].encode('utf-8')) > 160 or '\0' in value['name']:
                raise packages.ModError('Use a stable id such as yourname.french and a shorter pack name')
        except ValueError as error:
            QMessageBox.warning(self, 'Check language pack', str(error)); return
        super().accept()


class TranslationEditor(QDialog):
    def __init__(self, folder, parent=None):
        super().__init__(parent)
        self.folder = Path(folder); self.current = None; self.revision = None
        self.draft = {}; self.entries = {}; self.dirty = False; self.loading = False; self.key = None
        self.setWindowTitle('Translate — ' + packages.read_json(self.folder / 'mod.json')['name'])
        self.setObjectName('translationEditor'); self.resize(1080, 760)
        layout = QVBoxLayout(self); top = QHBoxLayout()
        self.catalogs = QComboBox(); self.catalogs.setObjectName('translationCatalog')
        info, catalogs = translations.load_project(self.folder)
        for record, data in catalogs:
            self.catalogs.addItem(data.get('resource', record['file']), record['file'])
        self.search = QLineEdit(); self.search.setObjectName('translationSearch')
        self.search.setPlaceholderText('Search source, translation, key or dialogue context')
        self.untranslated = QCheckBox('Untranslated only')
        top.addWidget(self.catalogs, 2); top.addWidget(self.search, 3); top.addWidget(self.untranslated)
        layout.addLayout(top)
        self.progress = QLabel(); self.progress.setObjectName('translationProgress'); layout.addWidget(self.progress)
        self.table = QTableWidget(0, 3); self.table.setObjectName('translationTable')
        self.table.setHorizontalHeaderLabels(['Key / context', 'Original text', 'Translation'])
        self.table.setSelectionBehavior(QTableWidget.SelectRows)
        self.table.setSelectionMode(QTableWidget.SingleSelection)
        self.table.setEditTriggers(QTableWidget.NoEditTriggers)
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.table.verticalHeader().hide(); self.table.setWordWrap(True)
        layout.addWidget(self.table, 3)
        self.original = QPlainTextEdit(); self.original.setReadOnly(True)
        self.original.setMaximumHeight(85); self.original.setObjectName('translationOriginal')
        layout.addWidget(self.original)
        row = QHBoxLayout(); self.fallback = QCheckBox('Keep the original text')
        self.tokens = QComboBox(); self.tokens.setMinimumWidth(140)
        insert = QPushButton('Insert tag'); insert.clicked.connect(self.insert_token)
        row.addWidget(self.fallback); row.addStretch(); row.addWidget(self.tokens); row.addWidget(insert)
        layout.addLayout(row)
        self.edit = QPlainTextEdit(); self.edit.setObjectName('translationText')
        self.edit.setPlaceholderText('Write your translation here'); layout.addWidget(self.edit, 1)
        hint = QLabel('Keep button tags and placeholders unchanged. Insert tag copies them exactly. '
                      'Drafts can be saved with errors; exporting requires a clean check.')
        hint.setWordWrap(True); layout.addWidget(hint)
        self.messages = QLabel(); self.messages.setWordWrap(True); self.messages.setTextFormat(Qt.PlainText)
        layout.addWidget(self.messages)
        bottom = QHBoxLayout()
        save = QPushButton('Save table'); save.setObjectName('saveTranslation'); save.clicked.connect(self.save)
        check = QPushButton('Check pack'); check.clicked.connect(self.check)
        export = QPushButton('Export language pack…'); export.setObjectName('exportTranslation'); export.clicked.connect(self.export)
        close = QPushButton('Close'); close.clicked.connect(self.close)
        for widget in [save, check, export]: bottom.addWidget(widget)
        bottom.addStretch(); bottom.addWidget(close); layout.addLayout(bottom)
        self.catalogs.currentIndexChanged.connect(self.choose_catalog)
        self.search.textChanged.connect(self.filter); self.untranslated.toggled.connect(self.filter)
        self.table.currentCellChanged.connect(self.choose_entry)
        self.edit.textChanged.connect(self.edited); self.fallback.toggled.connect(self.use_original)
        self.choose_catalog(); self.show_progress(translations.check_project(self.folder))

    def show_progress(self, result):
        self.progress.setText(f"{result['locale']} · {result['translated']:,} / {result['total']:,} translated · "
                              f"{result['remaining']:,} remaining · {result['percent']}%")
        errors = result['errors']
        self.messages.setText('\n'.join((str(e.get('file', '')) + ' ' + str(e.get('key', '')) + ': ' +
                                         e['error']).strip() for e in errors[:4]) if errors else 'Ready to export; untranslated entries keep the original.')

    def keep_edits(self):
        if not self.dirty: return True
        answer = QMessageBox.question(self, 'Unsaved translations', 'Save edits to this table?',
                                      QMessageBox.Save | QMessageBox.Discard | QMessageBox.Cancel)
        if answer == QMessageBox.Cancel: return False
        if answer == QMessageBox.Save: return self.save()
        self.dirty = False
        return True

    def choose_catalog(self, *_):
        name = self.catalogs.currentData()
        if name == self.current: return
        if not self.keep_edits():
            self.catalogs.blockSignals(True); self.catalogs.setCurrentIndex(self.catalogs.findData(self.current))
            self.catalogs.blockSignals(False); return
        if not name: return
        try: data = translations.read_catalog(self.folder, name)
        except ValueError as error:
            QMessageBox.warning(self, 'Cannot read text', str(error)); return
        self.current, self.revision = name, data['revision']; self.entries = data['catalog']['entries']
        self.draft = {key: entry['translation'] for key, entry in self.entries.items()}; self.dirty = False
        self.loading = True; self.key = None; self.table.setRowCount(0)
        for key, entry in self.entries.items():
            row = self.table.rowCount(); self.table.insertRow(row)
            for column, text in enumerate([entry.get('context', key), entry['source'], self.draft[key] if self.draft[key] is not None else 'Uses original']):
                item = QTableWidgetItem(text); item.setData(Qt.UserRole, key); item.setToolTip(key + '\n' + entry.get('context', ''))
                self.table.setItem(row, column, item)
        self.loading = False; self.filter()
        if self.table.rowCount():
            self.table.setCurrentCell(0, 1); self.choose_entry(0)

    def filter(self, *_):
        needle = self.search.text().casefold()
        for row in range(self.table.rowCount()):
            key = self.table.item(row, 0).data(Qt.UserRole); entry = self.entries[key]
            visible = needle in '\n'.join([key, entry['source'], entry.get('context', ''), self.draft[key] or '']).casefold()
            visible = visible and (not self.untranslated.isChecked() or self.draft[key] is None)
            self.table.setRowHidden(row, not visible)

    def choose_entry(self, row, *_):
        if self.loading or row < 0: return
        self.key = self.table.item(row, 0).data(Qt.UserRole); entry = self.entries[self.key]
        self.loading = True; self.original.setPlainText(entry['source'])
        self.fallback.setChecked(self.draft[self.key] is None)
        self.edit.setReadOnly(self.fallback.isChecked())
        self.edit.setPlainText(entry['source'] if self.draft[self.key] is None else self.draft[self.key])
        self.tokens.clear(); self.tokens.addItems(list(dict.fromkeys(translations.TOKENS.findall(entry['source']))))
        self.loading = False

    def use_original(self, checked):
        if self.loading or self.key is None: return
        self.loading = True; self.edit.setReadOnly(checked)
        if checked: self.edit.setPlainText(self.entries[self.key]['source'])
        self.loading = False; self.edited()

    def edited(self):
        if self.loading or self.key is None: return
        value = None if self.fallback.isChecked() else self.edit.toPlainText()
        if self.draft[self.key] == value: return
        self.draft[self.key] = value; self.dirty = True
        for row in range(self.table.rowCount()):
            if self.table.item(row, 0).data(Qt.UserRole) == self.key:
                self.table.item(row, 2).setText(value if value is not None else 'Uses original'); break
        self.messages.setText('Unsaved edits — Save table keeps your draft.'); self.filter()

    def insert_token(self):
        if self.key is None or not self.tokens.currentText(): return
        self.fallback.setChecked(False); self.edit.insertPlainText(self.tokens.currentText()); self.edit.setFocus()

    def save(self):
        if not self.dirty: return True
        try:
            result = translations.save_catalog(self.folder, self.current, self.draft, self.revision)
            self.revision = result['revision']; self.dirty = False; self.show_progress(result['progress']); return True
        except (OSError, ValueError) as error:
            QMessageBox.warning(self, 'Could not save translations', str(error)); return False

    def check(self):
        if self.save(): self.show_progress(translations.check_project(self.folder))

    def export(self):
        if not self.save(): return
        result = translations.check_project(self.folder); self.show_progress(result)
        if result['errors']: return
        manifest = packages.read_json(self.folder / 'mod.json')
        default = self.folder.parent / 'exports' / (manifest['id'] + '-' + manifest['version'] + '.zip')
        output, _ = QFileDialog.getSaveFileName(self, 'Export language pack', str(default), 'Mod package (*.zip)')
        if not output: return
        try:
            from argparse import Namespace
            translations.build(Namespace(name=str(self.folder)), None, self.folder.parent)
            packages.pack(self.folder, Path(output))
            self.messages.setText('Exported ' + str(output) + '\nImport this ZIP in the launcher’s Mods tab on desktop or Android.')
        except (OSError, ValueError) as error:
            QMessageBox.warning(self, 'Could not export language pack', str(error))

    def closeEvent(self, event):
        if self.keep_edits(): super().closeEvent(event)
        else: event.ignore()

    def reject(self):
        if self.keep_edits(): super().reject()
