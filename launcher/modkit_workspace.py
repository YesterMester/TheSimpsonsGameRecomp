"""Native workspace widgets; all scene changes remain in the shared editor services."""
import json
import re

from PySide6.QtCore import QMimeData, QSortFilterProxyModel, Qt, QTimer, Signal
from PySide6.QtGui import QAction, QColor, QDrag, QKeySequence, QStandardItem, QStandardItemModel, QTextCursor, QTextDocument
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QDialog,
    QHBoxLayout, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMenu, QPushButton,
    QToolButton, QTreeView, QVBoxLayout, QWidget)

if __package__:
    from .modkit_viewport import ASSET_MIME
else:
    from modkit_viewport import ASSET_MIME


def asset_key(asset):
    """Bookmarks use portable source identities, never a session's native handle."""
    return json.dumps([asset.get('source_stream', '').replace('\\', '/').casefold(), asset.get('source_guid', '').upper(),
                       asset.get('class', ''), asset.get('model_resource', '').upper()], separators=(',', ':'))


def placeable(asset):
    return (asset.get('category') == 'Props' and bool(asset.get('source_stream'))
            and bool(asset.get('source_guid')) and asset['source_guid'] != '0' * 32)


class AssetFilter(QSortFilterProxyModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.words = []; self.category = ''; self.props_only = self.favorites_only = False
        self.favorites = set()
        self.setSortCaseSensitivity(Qt.CaseInsensitive)

    def filterAcceptsRow(self, row, parent):
        asset = self.sourceModel().index(row, 0, parent).data(Qt.UserRole) or {}
        if self.category and asset.get('category') != self.category:
            return False
        if self.props_only and not placeable(asset):
            return False
        if self.favorites_only and asset_key(asset) not in self.favorites:
            return False
        haystack = ' '.join(str(asset.get(key, '')) for key in
                           ('name', 'class', 'category', 'source_stream', 'source_guid', 'model_resource')).casefold()
        return all(word in haystack for word in self.words)


class AssetView(QTreeView):
    dragging = Signal(bool)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setRootIsDecorated(False); self.setAlternatingRowColors(True)
        self.setUniformRowHeights(True); self.setSortingEnabled(True)
        self.setSelectionBehavior(QAbstractItemView.SelectRows)
        self.setSelectionMode(QAbstractItemView.SingleSelection)
        self.setDragEnabled(True); self.setDragDropMode(QAbstractItemView.DragOnly)

    def selected_asset(self):
        return self.currentIndex().siblingAtColumn(0).data(Qt.UserRole)

    def startDrag(self, actions):
        asset = self.selected_asset()
        if not asset or not placeable(asset):
            return
        mime = QMimeData(); mime.setData(ASSET_MIME, json.dumps(asset['id']).encode())
        drag = QDrag(self); drag.setMimeData(mime)
        self.dragging.emit(True)
        try:
            drag.exec(Qt.CopyAction)
        finally:
            self.dragging.emit(False)


class AssetLibrary(QWidget):
    selected = Signal(object)
    place = Signal(str)
    refresh = Signal()

    def __init__(self, settings, parent=None):
        super().__init__(parent); self.settings = settings; self.records = []
        values = settings.value('assetFavorites', [])
        self.favorites = set(values if isinstance(values, list) else [values] if values else [])
        layout = QVBoxLayout(self); layout.setContentsMargins(8, 6, 8, 6)
        row = QHBoxLayout()
        self.search = QLineEdit(); self.search.setObjectName('assetSearch')
        self.search.setClearButtonEnabled(True)
        self.search.setPlaceholderText('Search names, types, archives or GUIDs…')
        row.addWidget(self.search, 1)
        self.category = QComboBox(); self.category.setObjectName('assetCategory')
        self.category.addItem('All categories', ''); row.addWidget(self.category)
        self.props = QCheckBox('Placeable props'); self.props.setObjectName('placeableAssets'); row.addWidget(self.props)
        self.starred = QToolButton(); self.starred.setText('★ Favorites'); self.starred.setCheckable(True)
        self.starred.setObjectName('favoriteAssets'); row.addWidget(self.starred)
        button = QPushButton('Refresh'); button.clicked.connect(self.refresh); row.addWidget(button)
        layout.addLayout(row)
        self.view = AssetView(); self.view.setObjectName('assetLibrary'); layout.addWidget(self.view, 1)
        self.model = QStandardItemModel(self); self.model.setHorizontalHeaderLabels(['Resource', 'Type', 'Archive'])
        self.proxy = AssetFilter(self); self.proxy.favorites = self.favorites; self.proxy.setSourceModel(self.model)
        self.view.setModel(self.proxy); self.view.sortByColumn(0, Qt.AscendingOrder)
        self.view.setColumnWidth(0, 260); self.view.setColumnWidth(1, 165)
        self.count = QLabel('Start Test and load an episode to browse native resources.')
        self.count.setWordWrap(True); layout.addWidget(self.count)
        self.debounce = QTimer(self); self.debounce.setSingleShot(True); self.debounce.setInterval(120)
        self.debounce.timeout.connect(self.filter)
        self.search.textChanged.connect(lambda: self.debounce.start())
        for signal in (self.category.currentIndexChanged, self.props.toggled, self.starred.toggled):
            signal.connect(self.filter)
        self.proxy.rowsInserted.connect(self.update_count); self.proxy.rowsRemoved.connect(self.update_count)
        self.proxy.modelReset.connect(self.update_count)
        self.view.selectionModel().currentChanged.connect(lambda current, old: self.selected.emit(self.view.selected_asset()))
        self.view.doubleClicked.connect(lambda index: self.place_current())
        self.view.setContextMenuPolicy(Qt.CustomContextMenu); self.view.customContextMenuRequested.connect(self.context_menu)

    def set_records(self, records):
        selected = self.view.selected_asset(); identity = selected.get('id') if selected else None
        self.records = records
        model = QStandardItemModel(self); model.setHorizontalHeaderLabels(['Resource', 'Type', 'Archive'])
        for asset in records:
            name = ('★ ' if asset_key(asset) in self.favorites else '') + asset.get('name', 'Unnamed resource')
            cells = [QStandardItem(name), QStandardItem(asset.get('class', '')),
                     QStandardItem(asset.get('source_stream', ''))]
            cells[0].setData(asset, Qt.UserRole)
            for cell in cells:
                cell.setEditable(False)
                cell.setToolTip('Place beside your character' if placeable(asset) else
                                'Inspect only. This resource has no qualified prop placement path.')
                if not placeable(asset): cell.setForeground(QColor('#9ba9ba'))
            model.appendRow(cells)
        previous_model = self.model; self.model = model; self.proxy.setSourceModel(model)
        previous_model.deleteLater()
        previous = self.category.currentData()
        self.category.blockSignals(True); self.category.clear(); self.category.addItem('All categories', '')
        for category in sorted({a.get('category', '') for a in records} - {''}):
            self.category.addItem(category, category)
        self.category.setCurrentIndex(max(0, self.category.findData(previous))); self.category.blockSignals(False)
        self.filter()
        if identity:
            for row in range(self.model.rowCount()):
                index = self.model.index(row, 0)
                if index.data(Qt.UserRole).get('id') == identity:
                    self.view.setCurrentIndex(self.proxy.mapFromSource(index)); break

    def filter(self, *_):
        self.debounce.stop(); self.proxy.words = self.search.text().casefold().split()
        self.proxy.category = self.category.currentData() or ''
        self.proxy.props_only = self.props.isChecked(); self.proxy.favorites_only = self.starred.isChecked()
        self.proxy.invalidateFilter(); self.update_count()

    def update_count(self, *_):
        shown, total = self.proxy.rowCount(), len(self.records)
        if not total:
            self.count.setText('No loaded resources. Start Test, load an episode, then Refresh.'); return
        self.count.setText(f'{shown:,} of {total:,} resources · Double-click a prop to place it. Right-click to bookmark.')
        if not shown:
            self.count.setText(f'No matches in {total:,} resources. Clear the search or filters to see more.')

    def toggle_favorite(self):
        asset = self.view.selected_asset()
        if not asset:
            return
        key = asset_key(asset)
        self.favorites.remove(key) if key in self.favorites else self.favorites.add(key)
        self.settings.setValue('assetFavorites', sorted(self.favorites))
        index = self.proxy.mapToSource(self.view.currentIndex().siblingAtColumn(0))
        self.model.itemFromIndex(index).setText(('★ ' if key in self.favorites else '') + asset['name'])
        self.proxy.invalidateFilter(); self.update_count()

    def place_current(self):
        asset = self.view.selected_asset()
        if asset and placeable(asset): self.place.emit(asset['id'])

    def context_menu(self, position):
        index = self.view.indexAt(position)
        if not index.isValid(): return
        self.view.setCurrentIndex(index); asset = self.view.selected_asset()
        menu = QMenu(self)
        star = menu.addAction('Remove favorite' if asset_key(asset) in self.favorites else 'Add to favorites')
        star.triggered.connect(self.toggle_favorite)
        copy = menu.addAction('Copy source identity')
        copy.triggered.connect(lambda: self.copy_identity(asset))
        menu.exec(self.view.viewport().mapToGlobal(position))

    def copy_identity(self, asset):
        from PySide6.QtWidgets import QApplication
        QApplication.clipboard().setText(json.dumps({key: asset.get(key, '') for key in
            ('name', 'class', 'source_stream', 'source_guid', 'model_resource')}, indent=2))


class CommandPalette(QDialog):
    """A keyboard-first index of actual actions, files and offline help."""
    def __init__(self, entries, parent=None, title='Command palette'):
        super().__init__(parent); self.setWindowTitle(title); self.setObjectName('commandPalette')
        self.resize(660, 440); self.entries = entries
        layout = QVBoxLayout(self)
        self.search = QLineEdit(); self.search.setObjectName('commandSearch')
        self.search.setPlaceholderText('Type to find an action…'); layout.addWidget(self.search)
        self.list = QListWidget(); self.list.setObjectName('commandResults'); layout.addWidget(self.list, 1)
        note = QLabel('↑ ↓ to select · Enter to run · Esc to close'); note.setObjectName('mutedLabel'); layout.addWidget(note)
        self.search.textChanged.connect(self.filter); self.search.returnPressed.connect(self.activate)
        self.list.itemActivated.connect(lambda item: self.activate())
        self.search.installEventFilter(self); self.filter('')
        self.selected = None

    def filter(self, text):
        self.list.clear(); words = text.casefold().split()
        for entry in self.entries:
            if not all(word in (entry['label'] + ' ' + entry.get('category', '')).casefold() for word in words):
                continue
            suffix = '   ' + entry['shortcut'] if entry.get('shortcut') else ''
            item = QListWidgetItem(entry['label'] + suffix); item.setData(Qt.UserRole, entry)
            if not entry.get('enabled', True):
                item.setFlags(item.flags() & ~Qt.ItemIsEnabled); item.setToolTip('Unavailable in the current workspace state')
            self.list.addItem(item)
        for row in range(self.list.count()):
            if self.list.item(row).flags() & Qt.ItemIsEnabled:
                self.list.setCurrentRow(row); break

    def eventFilter(self, watched, event):
        from PySide6.QtCore import QEvent
        if watched is self.search and event.type() == QEvent.KeyPress and event.key() in (Qt.Key_Up, Qt.Key_Down):
            step = 1 if event.key() == Qt.Key_Down else -1
            row = self.list.currentRow() + step
            while 0 <= row < self.list.count():
                if self.list.item(row).flags() & Qt.ItemIsEnabled:
                    self.list.setCurrentRow(row); break
                row += step
            return True
        return super().eventFilter(watched, event)

    def activate(self):
        item = self.list.currentItem()
        if item and item.flags() & Qt.ItemIsEnabled:
            self.selected = item.data(Qt.UserRole); self.accept()


class FindBar(QWidget):
    closed = Signal()

    def __init__(self, editor, parent=None):
        super().__init__(parent); self.editor = editor
        layout = QHBoxLayout(self); layout.setContentsMargins(6, 5, 6, 5)
        self.query = QLineEdit(); self.query.setObjectName('sourceFind'); self.query.setPlaceholderText('Find in this file')
        self.replacement = QLineEdit(); self.replacement.setObjectName('sourceReplace'); self.replacement.setPlaceholderText('Replace with')
        layout.addWidget(self.query, 2); layout.addWidget(self.replacement, 2)
        self.case = QCheckBox('Aa'); self.case.setToolTip('Match case'); layout.addWidget(self.case)
        self.words = QCheckBox('Word'); self.words.setToolTip('Whole words'); layout.addWidget(self.words)
        self.count = QLabel(''); self.count.setMinimumWidth(66); layout.addWidget(self.count)
        for text, callback in [('↑', lambda: self.find(True)), ('↓', self.find),
                               ('Replace', self.replace), ('All', self.replace_all), ('×', self.dismiss)]:
            button = QPushButton(text); button.clicked.connect(callback)
            if text in ('↑', '↓', '×'): button.setMaximumWidth(30)
            layout.addWidget(button)
            if text == 'Replace': self.replace_button = button
            if text == 'All': self.all_button = button
        self.delay = QTimer(self); self.delay.setSingleShot(True); self.delay.setInterval(100)
        self.delay.timeout.connect(self.update_count)
        for signal in (self.query.textChanged, self.case.toggled, self.words.toggled): signal.connect(lambda: self.delay.start())
        self.query.returnPressed.connect(self.find)
        self.replacement.returnPressed.connect(self.replace)
        self.escape = QAction(self); self.escape.setShortcut(QKeySequence('Esc'))
        self.escape.setShortcutContext(Qt.WidgetWithChildrenShortcut); self.escape.triggered.connect(self.dismiss)
        self.addAction(self.escape)
        self.hide()

    def open(self, replace=False):
        self.replacement.setVisible(replace); self.replace_button.setVisible(replace); self.all_button.setVisible(replace)
        self.show(); self.query.setFocus(); self.query.selectAll(); self.update_count()

    def dismiss(self):
        self.hide(); self.editor().setFocus(); self.closed.emit()

    def flags(self, backwards=False):
        flags = QTextDocument.FindFlags()
        if self.case.isChecked(): flags |= QTextDocument.FindCaseSensitively
        if self.words.isChecked(): flags |= QTextDocument.FindWholeWords
        if backwards: flags |= QTextDocument.FindBackward
        return flags

    def find(self, backwards=False):
        query = self.query.text(); editor = self.editor()
        if not query: return False
        if not editor.find(query, self.flags(bool(backwards))):
            cursor = editor.textCursor(); cursor.movePosition(QTextCursor.End if backwards else QTextCursor.Start)
            editor.setTextCursor(cursor)
            if not editor.find(query, self.flags(bool(backwards))):
                self.count.setText('No matches'); return False
        return True

    def replace(self):
        editor = self.editor()
        if editor.isReadOnly() or not self.query.text(): return
        cursor = editor.textCursor()
        match = editor.document().find(self.query.text(), cursor.selectionStart(), self.flags())
        equal = (not match.isNull() and cursor.hasSelection() and match.selectionStart() == cursor.selectionStart()
                 and match.selectionEnd() == cursor.selectionEnd())
        if not equal and not self.find(): return
        cursor = editor.textCursor(); cursor.insertText(self.replacement.text()); editor.setTextCursor(cursor)
        self.find(); self.update_count()

    def replace_all(self):
        editor = self.editor(); query = self.query.text()
        if editor.isReadOnly() or not query: return
        document = editor.document(); cursor = QTextCursor(document); cursor.beginEditBlock(); count = 0
        blocked = editor.blockSignals(True)
        try:
            match = document.find(query, cursor, self.flags())
            while not match.isNull():
                match.insertText(self.replacement.text()); cursor.setPosition(match.position()); count += 1
                match = document.find(query, cursor, self.flags())
        finally:
            cursor.endEditBlock(); editor.blockSignals(blocked)
        if count:
            editor.textChanged.emit()
            if hasattr(editor, 'number_margin'): editor.number_margin()
        self.count.setText(f'{count:,} replaced')

    def update_count(self):
        query = self.query.text()
        pattern = re.escape(query)
        if self.words.isChecked(): pattern = r'\b' + pattern + r'\b'
        count = len(re.findall(pattern, self.editor().toPlainText(), 0 if self.case.isChecked() else re.IGNORECASE)) if query else 0
        self.count.setText(f'{count:,} matches' if query else '')
        readonly = self.editor().isReadOnly()
        self.replace_button.setEnabled(not readonly); self.all_button.setEnabled(not readonly)
