"""Standalone native desktop creator workspace using the shared mod backend."""
import json
from pathlib import Path
import re
import sys
import time

from PySide6.QtCore import QByteArray, QEvent, QObject, QRect, QRunnable, QSettings, QSize, Qt, QThreadPool, QTimer, QUrl, Signal, Slot
from PySide6.QtGui import QAction, QColor, QDesktopServices, QFont, QIcon, QKeySequence, QPainter, QSyntaxHighlighter, QTextCharFormat, QTextCursor
from PySide6.QtWidgets import (QApplication, QCheckBox, QComboBox, QCompleter, QDialog, QDialogButtonBox,
    QDockWidget, QDoubleSpinBox, QFileDialog, QFormLayout, QHBoxLayout, QInputDialog, QLabel, QLineEdit,
    QListWidget, QListWidgetItem, QMainWindow, QMessageBox, QPlainTextEdit, QPushButton,
    QScrollArea, QSplitter, QTabWidget, QTextBrowser, QToolBar, QTreeWidget, QTreeWidgetItem,
    QProgressBar, QStyle, QVBoxLayout, QWidget)
from PySide6 import QtSvg  # Keep the SVG icon engine in the shared desktop bundle.

if __package__:
    from . import mod_packages, mod_projects, modkit_levels, mod_translations
    from .modkit_translations import NewTranslation, TranslationEditor
    from .modkit_session import EditorSession
    from .modkit_viewport import ASSET_MIME, GameViewport
    from .modkit_workspace import AssetLibrary, CommandPalette, FindBar, asset_key, placeable
    from .modkit_preview import ViewportTools, GameLibrary, DefinitionPicker
    from .modkit_asset_store import AssetStore
    from .modkit_mesh_view import HoverModel, ModelPreview
else:
    import mod_packages
    import mod_projects
    import modkit_levels
    import mod_translations
    from modkit_translations import NewTranslation, TranslationEditor
    from modkit_session import EditorSession
    from modkit_viewport import ASSET_MIME, GameViewport
    from modkit_workspace import AssetLibrary, CommandPalette, FindBar, asset_key, placeable
    from modkit_preview import ViewportTools, GameLibrary, DefinitionPicker
    from modkit_asset_store import AssetStore
    from modkit_mesh_view import HoverModel, ModelPreview


class TaskSignals(QObject):
    completed = Signal(object, object, str)


class Task(QRunnable):
    def __init__(self, function):
        super().__init__(); self.setAutoDelete(False)
        self.function = function; self.signals = TaskSignals()
        self.callback = None; self.quiet = False
        self.label = ''
    def run(self):
        value, error = None, ''
        try:
            value = self.function()
        except Exception as failure:
            error = str(failure) or type(failure).__name__
        self.signals.completed.emit(self, value, error)


class SourceHighlight(QSyntaxHighlighter):
    def highlightBlock(self, text):
        positions = [0]
        for character in text: positions.append(positions[-1] + (2 if ord(character) > 0xFFFF else 1))
        for pattern, color in [(r'\b(?:function|local|end|if|then|else|elseif|for|in|do|return|true|false|nil|and|or|not|while|repeat|until)\b', '#d7a9e3'),
                (r'\bgame\.[a-z_]+\.[a-z_]+', '#8acbd4'),
                (r'\b\d+(?:\.\d+)?\b', '#dfbd82'),
                (r'"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27', '#b5cf99'),
                (r'--.*$', '#82918c')]:
            fmt = QTextCharFormat(); fmt.setForeground(QColor(color))
            for match in re.finditer(pattern, text):
                self.setFormat(positions[match.start()], positions[match.end()] - positions[match.start()], fmt)


class LineNumbers(QWidget):
    def __init__(self, editor):
        super().__init__(editor); self.editor = editor
    def sizeHint(self):
        return QSize(self.editor.number_width(), 0)
    def paintEvent(self, event):
        painter = QPainter(self); painter.fillRect(event.rect(), QColor('#171b20'))
        painter.setPen(QColor('#778594')); painter.setFont(self.editor.font())
        block = self.editor.firstVisibleBlock()
        top = self.editor.blockBoundingGeometry(block).translated(self.editor.contentOffset()).top()
        height = self.editor.fontMetrics().height()
        while block.isValid() and top <= event.rect().bottom():
            if block.isVisible() and top + height >= event.rect().top():
                painter.drawText(0, int(top), self.width() - 8, height, Qt.AlignRight, str(block.blockNumber() + 1))
            top += self.editor.blockBoundingRect(block).height(); block = block.next()


class SourceEditor(QPlainTextEdit):
    def __init__(self):
        super().__init__(); self.setObjectName('sourceEditor')
        self.setFont(QFont('monospace', 10)); self.setLineWrapMode(QPlainTextEdit.NoWrap)
        self.setTabStopDistance(self.fontMetrics().horizontalAdvance(' ') * 2)
        self.highlight = SourceHighlight(self.document())
        self.source = None
        self.numbers = LineNumbers(self)
        self.blockCountChanged.connect(self.number_margin)
        self.updateRequest.connect(self.number_update); self.number_margin()
        self.completer = QCompleter(self); self.completer.setWidget(self)
        self.completer.setCaseSensitivity(Qt.CaseInsensitive)
        self.completer.activated.connect(self.complete)
        self.setPlaceholderText('Open a source file from Project files.\n\nCtrl+S saves. Select API reference entries for documentation.')
    def keyPressEvent(self, event):
        if self.isReadOnly():
            super().keyPressEvent(event); return
        if self.completer.popup().isVisible() and event.key() in (Qt.Key_Return, Qt.Key_Enter, Qt.Key_Tab, Qt.Key_Escape):
            event.ignore(); return
        if event.key() in (Qt.Key_Return, Qt.Key_Enter) and event.modifiers() == Qt.NoModifier:
            cursor = self.textCursor(); prefix = cursor.block().text()[:cursor.positionInBlock()]
            indentation = re.match(r'[ \t]*', prefix).group()
            self.insertPlainText('\n' + indentation); return
        if event.key() == Qt.Key_Space and event.modifiers() == Qt.ControlModifier:
            cursor = self.textCursor(); prefix = re.search(r'[\w.]*$', self.toPlainText()[:cursor.position()]).group()
            self.completion_prefix = prefix; self.completer.setCompletionPrefix(prefix)
            self.completer.complete(self.cursorRect()); return
        if event.key() in (Qt.Key_Tab, Qt.Key_Backtab) and event.modifiers() in (Qt.NoModifier, Qt.ShiftModifier):
            cursor = self.textCursor()
            if not cursor.hasSelection() and event.key() == Qt.Key_Tab:
                self.insertPlainText('  '); return
            first = self.document().findBlock(cursor.selectionStart())
            last = self.document().findBlock(max(cursor.selectionStart(), cursor.selectionEnd() - 1))
            blocks = []; block = first
            while block.isValid() and block.blockNumber() <= last.blockNumber():
                blocks.append((block.position(), block.text())); block = block.next()
            start, finish = cursor.selectionStart(), cursor.selectionEnd(); delta = 0
            cursor.beginEditBlock()
            for position, text in reversed(blocks):
                cursor.setPosition(position)
                if event.key() == Qt.Key_Backtab:
                    count = min(2, len(text) - len(text.lstrip(' ')))
                    for _ in range(count): cursor.deleteChar()
                    delta -= count
                else:
                    cursor.insertText('  '); delta += 2
            cursor.endEditBlock(); cursor.setPosition(max(0, start)); cursor.setPosition(max(start, finish + delta), QTextCursor.KeepAnchor)
            self.setTextCursor(cursor)
        else:
            super().keyPressEvent(event)
    def complete(self, value):
        if self.isReadOnly():
            return
        cursor = self.textCursor()
        for _ in getattr(self, 'completion_prefix', ''):
            cursor.deletePreviousChar()
        cursor.insertText(value); self.setTextCursor(cursor)
    def number_width(self):
        return 12 + self.fontMetrics().horizontalAdvance('9') * len(str(max(1, self.blockCount())))
    def number_margin(self, *_):
        self.setViewportMargins(self.number_width(), 0, 0, 0)
    def number_update(self, rect, dy):
        if dy: self.numbers.scroll(0, dy)
        else: self.numbers.update(0, rect.y(), self.numbers.width(), rect.height())
        if rect.contains(self.viewport().rect()): self.number_margin()
    def resizeEvent(self, event):
        super().resizeEvent(event); rect = self.contentsRect()
        self.numbers.setGeometry(QRect(rect.left(), rect.top(), self.number_width(), rect.height()))


class NewProject(QDialog):
    def __init__(self, templates, parent=None):
        super().__init__(parent); self.setWindowTitle('New mod project'); self.resize(520, 370)
        self.setObjectName('newProjectDialog')
        layout = QVBoxLayout(self); form = QFormLayout()
        self.template = QComboBox(); self.template.setObjectName('projectTemplate')
        for value in templates:
            self.template.addItem(value['name'], value)
        self.name = QLineEdit(); self.name.setObjectName('projectName'); self.name.setMaxLength(160)
        self.identifier = QLineEdit(); self.identifier.setObjectName('projectId'); self.identifier.setMaxLength(96)
        self.identifier.setPlaceholderText('yourname.my-mod')
        self.author = QLineEdit(); self.author.setObjectName('projectAuthor'); self.author.setMaxLength(160)
        self.description = QLineEdit(); self.description.setMaxLength(1024)
        for title, field in [('Template', self.template), ('Name', self.name), ('Stable id', self.identifier),
                             ('Creator credit', self.author), ('Description', self.description)]:
            form.addRow(title, field)
        layout.addLayout(form)
        hint = QLabel(); hint.setWordWrap(True); layout.addWidget(hint)
        self.template.currentIndexChanged.connect(lambda: hint.setText(self.template.currentData()['description']))
        self.template.currentIndexChanged.emit(0)
        note = QLabel('The project starts disabled. Test uses separate progress.'); note.setWordWrap(True); layout.addWidget(note)
        buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        buttons.button(QDialogButtonBox.Ok).setText('Create project')
        buttons.accepted.connect(self.accept); buttons.rejected.connect(self.reject); layout.addWidget(buttons)
    def values(self):
        return {'template': self.template.currentData()['id'], 'name': self.name.text(),
                'id': self.identifier.text(), 'author': self.author.text(), 'description': self.description.text()}
    def accept(self):
        try:
            mod_projects.text(self.name.text(), 'Name', 160, True)
            mod_projects.text(self.author.text(), 'Creator credit', 160)
            mod_projects.text(self.description.text(), 'Description', 1024)
            if not mod_packages.ID.fullmatch(self.identifier.text()):
                raise ValueError('Choose a stable id such as yourname.my-mod')
        except ValueError as error:
            QMessageBox.warning(self, 'Check project details', str(error)); return
        super().accept()


class ModkitWindow(QMainWindow):
    def __init__(self, backend, project=None, settings=None):
        super().__init__(); self.backend = backend
        self.settings = settings or QSettings('YesterMester', 'SimpsonsModkit')
        self.project = self.source = self.session = None
        self.dirty = self.loading_source = self.inspector_loading = False
        self.jobs = set(); self.pool = QThreadPool(self); self.pool.setMaxThreadCount(1)
        self.live = {}; self.assets = []; self.selected_tag = None; self.last_epoch = None
        self.documents = {}; self.pending_scene_refresh = False; self.inspector_signature = None
        self.pending_work = []; self.inspector_mode = 'project'; self.asset_selection = None
        self.compact_mode = False; self.workspace_auto = self.settings.value('workspaceAuto', True, type=bool)
        self.test_available = backend.GAME_BIN.is_file() and backend.modkit_ready()
        self.close_when_idle = False
        self.preview_pending = False; self.pending_source = None
        self.asset_store = None; self.hover_asset = None
        self.bundle = mod_projects.catalog(backend.UI_DIR / 'modkit-templates.json')
        self.templates = self.bundle['templates']
        self.setWindowTitle('The Simpsons Game Modkit [*]'); self.resize(1280, 850)
        self.setMinimumSize(560, 480)
        self.setWindowIcon(QIcon(str(backend.UI_DIR / 'modkit-icon.svg')))
        self.build_workspace(); self.build_actions(); self.build_help()
        self.hover_timer=QTimer(self);self.hover_timer.setSingleShot(True);self.hover_timer.setInterval(450)
        self.hover_timer.timeout.connect(self.open_hover_preview)
        from PySide6.QtCore import QStringListModel
        self.code.completer.setModel(QStringListModel(['game.' + operation for operation in self.bundle['operations']], self.code.completer))
        self.default_layout = self.saveState()
        saved_layout = self.settings.value('wideLayout') if self.settings.value('compactMode', False, type=bool) else self.settings.value('layout')
        if saved_layout is not None: self.restoreState(saved_layout)
        self.wide_layout = self.saveState()
        if self.settings.value('geometry') is not None:
            self.restoreGeometry(self.settings.value('geometry'))
        self.timer = QTimer(self); self.timer.timeout.connect(self.poll); self.timer.start(1200)
        self.library_retry = QTimer(self); self.library_retry.setSingleShot(True); self.library_retry.setInterval(150)
        self.library_retry.timeout.connect(self.search_game_library)
        self.adaptive_timer = QTimer(self); self.adaptive_timer.setSingleShot(True); self.adaptive_timer.setInterval(120)
        self.adaptive_timer.timeout.connect(self.adapt_workspace)
        if self.settings.value('compactMode', False, type=bool) and (not self.workspace_auto or self.width() < 1000):
            self.set_compact_workspace(True)
            if self.settings.value('layout') is not None: self.restoreState(self.settings.value('layout'))
        self.adaptive_timer.start()
        self.refresh_levels()
        self.refresh_projects(project)

    def dock(self, title, name, widget, area):
        dock = QDockWidget(title, self); dock.setObjectName(name); dock.setWidget(widget)
        self.addDockWidget(area, dock); return dock

    def build_workspace(self):
        self.tabs = QTabWidget(); self.tabs.setDocumentMode(True)
        self.tabs.setTabsClosable(True); self.tabs.setMovable(True)
        self.viewport = GameViewport(); self.viewport.assetDropped.connect(self.place_asset)
        self.viewport.libraryAssetDropped.connect(self.drop_library_asset)
        self.viewport.testRequested.connect(self.test_project)
        self.viewport.helpRequested.connect(lambda: self.show_help('EDITOR_WORKFLOW.md'))
        self.code = self.make_source_editor()
        self.tabs.addTab(self.viewport, 'Scene / game'); self.tabs.addTab(self.code, 'Source')
        self.model_preview=ModelPreview();self.tabs.addTab(self.model_preview,'Asset preview')
        self.model_preview.helpRequested.connect(lambda:self.show_help('ASSET_PREVIEWS.md'))
        self.tabs.tabBar().setTabButton(2,self.tabs.tabBar().ButtonPosition.RightSide,None)
        self.tabs.tabBar().setTabButton(2,self.tabs.tabBar().ButtonPosition.LeftSide,None)
        self.hover_popup=HoverModel(self)
        self.tabs.tabBar().setTabButton(0, self.tabs.tabBar().ButtonPosition.RightSide, None)
        self.tabs.tabBar().setTabButton(0, self.tabs.tabBar().ButtonPosition.LeftSide, None)
        self.tabs.currentChanged.connect(self.source_tab_changed); self.tabs.tabCloseRequested.connect(self.close_source)
        central = QWidget(); center_layout = QVBoxLayout(central); center_layout.setContentsMargins(0, 0, 0, 0)
        center_layout.setSpacing(0)
        self.viewport_tools = ViewportTools(); center_layout.addWidget(self.viewport_tools)
        self.viewport_tools.launch.connect(self.load_level)
        self.viewport_tools.cameraChanged.connect(self.set_camera)
        self.viewport_tools.layersChanged.connect(self.set_layers)
        self.viewport_tools.focusRequested.connect(self.frame_selection)
        center_layout.addWidget(self.tabs)
        self.find_bar = FindBar(lambda: self.code, self); center_layout.addWidget(self.find_bar)
        self.setCentralWidget(central)
        project_widget = QWidget(); layout = QVBoxLayout(project_widget)
        self.project_filter = QLineEdit(); self.project_filter.setPlaceholderText('Search projects')
        self.project_filter.textChanged.connect(self.filter_projects); layout.addWidget(self.project_filter)
        self.projects = QListWidget(); self.projects.setObjectName('projectList')
        self.projects.itemActivated.connect(lambda item: self.open_project(item.data(Qt.UserRole)))
        self.projects.itemClicked.connect(lambda item: self.open_project(item.data(Qt.UserRole)))
        layout.addWidget(self.projects)
        self.project_dock = self.dock('Projects', 'projectsDock', project_widget, Qt.LeftDockWidgetArea)
        file_widget = QWidget(); files_layout = QVBoxLayout(file_widget)
        self.file_filter = QLineEdit(); self.file_filter.setPlaceholderText('Filter project files · Ctrl+P')
        self.file_filter.setClearButtonEnabled(True); self.file_filter.textChanged.connect(self.filter_files)
        files_layout.addWidget(self.file_filter)
        self.files = QTreeWidget(); self.files.setHeaderHidden(True); self.files.setObjectName('projectFiles')
        self.files.itemActivated.connect(self.file_chosen); self.files.itemClicked.connect(self.file_chosen)
        files_layout.addWidget(self.files)
        self.file_dock = self.dock('Project files', 'filesDock', file_widget, Qt.LeftDockWidgetArea)
        self.splitDockWidget(self.project_dock, self.file_dock, Qt.Vertical)
        scene_widget = QWidget(); scene_layout = QVBoxLayout(scene_widget)
        self.scene_filter = QLineEdit(); self.scene_filter.setPlaceholderText('Find scene objects…')
        self.scene_filter.setClearButtonEnabled(True); self.scene_filter.textChanged.connect(self.filter_scene)
        scene_layout.addWidget(self.scene_filter)
        self.scene_tree = QTreeWidget(); self.scene_tree.setObjectName('sceneOutliner')
        self.scene_tree.setHeaderLabels(['Object', 'Type']); self.scene_tree.setRootIsDecorated(False)
        self.scene_tree.setColumnHidden(1, True)
        self.scene_tree.setAlternatingRowColors(True); self.scene_tree.setUniformRowHeights(True)
        self.scene_tree.currentItemChanged.connect(self.select_scene_object); scene_layout.addWidget(self.scene_tree, 1)
        self.scene_hint = QLabel('Open a Scene or Mission project to inspect authored objects.')
        self.scene_hint.setWordWrap(True); scene_layout.addWidget(self.scene_hint)
        self.scene_dock = self.dock('Scene outliner', 'sceneDock', scene_widget, Qt.LeftDockWidgetArea)
        self.tabifyDockWidget(self.file_dock, self.scene_dock); self.file_dock.raise_()
        self.properties = QWidget(); self.property_layout = QVBoxLayout(self.properties)
        self.property_layout.addWidget(QLabel('Open a project to inspect its properties.'))
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setWidget(self.properties)
        self.property_dock = self.dock('Inspector', 'inspectorDock', scroll, Qt.RightDockWidgetArea)
        self.output = QPlainTextEdit(); self.output.setReadOnly(True); self.output.setMaximumBlockCount(1500)
        self.output.setObjectName('outputLog'); self.output.setFont(QFont('monospace', 9))
        self.output_dock = self.dock('Output', 'outputDock', self.output, Qt.BottomDockWidgetArea)
        self.library = AssetLibrary(self.settings); self.asset_filter = self.library.search
        self.asset_list = self.library.view; self.library.refresh.connect(self.refresh_assets)
        self.library.place.connect(self.place_asset)
        self.library.selected.connect(self.inspect_asset)
        self.library.view.setMouseTracking(True)
        self.library.view.entered.connect(lambda index:self.queue_hover(index.siblingAtColumn(0).data(Qt.UserRole)))
        self.library.view.viewport().installEventFilter(self)
        self.library.view.dragging.connect(lambda active:self.cancel_hover() if active else None)
        self.asset_list.dragging.connect(lambda visible: (self.tabs.setCurrentWidget(self.viewport) if visible else None,
                                                         self.viewport.show_drop(visible)))
        self.asset_hint = self.library.count
        self.game_library = GameLibrary()
        self.game_library.searchRequested.connect(self.search_game_library)
        self.game_library.selected.connect(self.inspect_library_asset)
        self.game_library.prepareRequested.connect(lambda asset: self.choose_library_definition(asset, False))
        self.game_library.previewRequested.connect(self.open_model_preview)
        self.game_library.nativePreviewRequested.connect(lambda asset: self.choose_library_definition(asset, True))
        self.game_library.hovered.connect(self.queue_hover);self.game_library.items.viewport().installEventFilter(self)
        self.game_library.items.dragging.connect(lambda active:(self.cancel_hover(),self.tabs.setCurrentWidget(self.viewport),self.viewport.show_drop(True))
                                                if active else self.viewport.show_drop(False))
        self.game_library.clearRequested.connect(self.clear_asset_preview)
        self.game_library.cancelRequested.connect(self.cancel_asset_source)
        self.asset_tabs = QTabWidget(); self.asset_tabs.setDocumentMode(True)
        self.asset_tabs.addTab(self.library, 'Loaded resources'); self.asset_tabs.addTab(self.game_library, 'Whole game')
        self.asset_tabs.currentChanged.connect(lambda i: self.search_game_library() if i == 1 else None)
        self.assets_dock = self.dock('Assets', 'assetsDock', self.asset_tabs, Qt.BottomDockWidgetArea)
        self.tabifyDockWidget(self.assets_dock, self.output_dock); self.assets_dock.raise_()
        self.connection = QLabel('No project open'); self.statusBar().addWidget(self.connection, 1)
        self.busy = QProgressBar(); self.busy.setRange(0, 0); self.busy.setMaximumWidth(100)
        self.busy.setMaximumHeight(12); self.busy.setTextVisible(False); self.busy.hide()
        self.statusBar().addPermanentWidget(self.busy)
        self.cursor_status = QLabel(''); self.statusBar().addPermanentWidget(self.cursor_status)
        self.resizeDocks([self.project_dock, self.property_dock], [210, 280], Qt.Horizontal)
        self.resizeDocks([self.assets_dock], [190], Qt.Vertical)

    def make_source_editor(self):
        editor = SourceEditor(); editor.textChanged.connect(lambda: self.source_changed(editor))
        editor.cursorPositionChanged.connect(self.update_cursor)
        editor.undoAvailable.connect(self.update_controls); editor.redoAvailable.connect(self.update_controls)
        if hasattr(self, 'bundle'):
            from PySide6.QtCore import QStringListModel
            editor.completer.setModel(QStringListModel(['game.' + name for name in self.bundle['operations']], editor.completer))
        return editor

    def build_actions(self):
        self.actions = {}
        def action(name, text, callback, shortcut=None):
            value = QAction(text, self); value.setObjectName(name)
            if shortcut:
                value.setShortcut(QKeySequence(shortcut))
            value.triggered.connect(callback); self.actions[name] = value; return value
        file_menu = self.menuBar().addMenu('&File')
        for value in [action('newProject', 'New project…', self.new_project, 'Ctrl+N'),
                      action('newTranslation', 'New language pack…', self.new_translation),
                      action('editTranslation', 'Edit translations…', self.edit_translation),
                      action('saveSource', 'Save source', self.save_source, 'Ctrl+S'),
                      action('saveAll', 'Save all files', self.save_all, 'Ctrl+Shift+S'),
                      action('reloadSource', 'Reload file from disk', self.reload_source),
                      action('closeSource', 'Close file', lambda: self.close_source(self.tabs.currentIndex()), 'Ctrl+W'),
                      action('quickOpen', 'Quick open file…', lambda: self.command_palette('files'), 'Ctrl+P'),
                      action('applyDrafts', 'Apply saved drafts', self.apply_drafts),
                      action('exportProject', 'Export package…', self.export_project, 'Ctrl+Shift+E')]:
            file_menu.addAction(value)
        file_menu.addSeparator(); file_menu.addAction(action('closeApp', 'Close Modkit', self.close, 'Ctrl+Q'))
        edit_menu = self.menuBar().addMenu('&Edit')
        edit_menu.addAction(action('findSource', 'Find in file…', lambda: self.find_source(False), 'Ctrl+F'))
        edit_menu.addAction(action('replaceSource', 'Find and replace…', lambda: self.find_source(True), 'Ctrl+H'))
        edit_menu.addAction(action('gotoLine', 'Go to line…', self.goto_line, 'Ctrl+G'))
        edit_menu.addAction(action('commandPalette', 'Command palette…', self.command_palette, 'Ctrl+Shift+P'))
        project_menu = self.menuBar().addMenu('&Project')
        for value in [action('validateProject', 'Validate project', self.validate_project, 'F7'),
                      action('testProject', 'Test in game', self.test_project, 'F5'),
                      action('quickLaunchLevel', 'Quick launch level', lambda: self.load_level(self.viewport_tools.level.currentData()), 'Ctrl+Shift+L'),
                      action('stopTest', 'Stop Test', self.stop_test, 'Shift+F5'),
                      action('saveDrafts', 'Save live drafts', lambda: self.scene_action('save'))]:
            project_menu.addAction(value)
        project_menu.addAction(action('openProjectFolder', 'Open project folder', self.open_project_folder))
        scene_menu = self.menuBar().addMenu('&Scene')
        for name, title, shortcut in [('undo', 'Undo scene edit', 'Alt+Z'), ('redo', 'Redo scene edit', 'Alt+Shift+Z'),
                                      ('load', 'Load scene preview', None), ('unload', 'Unload scene preview', None)]:
            scene_menu.addAction(action('scene_' + name, title, lambda checked=False, row=name: self.scene_action(row), shortcut))
        self.view_menu = self.menuBar().addMenu('&View')
        for dock in self.findChildren(QDockWidget):
            self.view_menu.addAction(dock.toggleViewAction())
        self.view_menu.addSeparator()
        compact = action('compactWorkspace', 'Compact workspace', lambda checked: self.set_compact_workspace(checked, manual=True))
        compact.setCheckable(True); self.view_menu.addAction(compact)
        self.view_menu.addAction('Reset workspace layout', self.reset_workspace)
        self.help_menu = self.menuBar().addMenu('&Help')
        toolbar = QToolBar('Project actions'); toolbar.setObjectName('projectToolbar'); toolbar.setMovable(False)
        self.addToolBar(toolbar)
        toolbar.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        for name, icon in [('newProject', QStyle.SP_FileIcon), ('saveSource', QStyle.SP_DialogSaveButton),
                           ('validateProject', QStyle.SP_DialogApplyButton), ('testProject', QStyle.SP_MediaPlay),
                           ('stopTest', QStyle.SP_MediaStop), ('saveDrafts', QStyle.SP_DialogSaveButton),
                           ('applyDrafts', QStyle.SP_ArrowDown), ('exportProject', QStyle.SP_DirLinkIcon)]:
            self.actions[name].setIcon(self.style().standardIcon(icon))
        self.actions['saveAll'].setIcon(self.style().standardIcon(QStyle.SP_DialogSaveButton))
        for name, color in [('testProject', '#b6d99b'), ('stopTest', '#efaaa2')]:
            pixmap = self.actions[name].icon().pixmap(36, 36); pixmap.setDevicePixelRatio(2)
            painter = QPainter(pixmap); painter.setCompositionMode(QPainter.CompositionMode_SourceIn)
            painter.fillRect(pixmap.rect(), QColor(color)); painter.end(); self.actions[name].setIcon(QIcon(pixmap))
        toolbar.setIconSize(QSize(18, 18))
        for name in ['newProject', 'saveAll', 'validateProject', 'testProject', 'stopTest', 'exportProject']:
            toolbar.addAction(self.actions[name])
        toolbar.addSeparator(); toolbar.addAction(self.actions['commandPalette'])
        self.update_controls()

    def build_help(self):
        help_widget = QWidget(); layout = QVBoxLayout(help_widget)
        self.help_search = QLineEdit(); self.help_search.setPlaceholderText('Search guides and named game operations')
        layout.addWidget(self.help_search); split = QSplitter()
        self.help_topics = QListWidget(); self.help_topics.setMinimumWidth(200); split.addWidget(self.help_topics)
        self.help_text = QTextBrowser(); self.help_text.setOpenLinks(False); split.addWidget(self.help_text)
        self.help_text.anchorClicked.connect(lambda url: QDesktopServices.openUrl(url) if url.scheme() in ('https', 'http')
                                            else self.show_help(Path(url.path()).name))
        split.setSizes([240, 800])
        layout.addWidget(split); self.help_dock = self.dock('Help & API reference', 'helpDock', help_widget, Qt.BottomDockWidgetArea)
        self.tabifyDockWidget(self.output_dock, self.help_dock); self.assets_dock.raise_()
        self.view_menu.addAction(self.help_dock.toggleViewAction())
        intro=QAction('Context help',self); intro.setShortcut(QKeySequence('F1')); intro.triggered.connect(self.context_help); self.help_menu.addAction(intro)
        self.help_menu.addAction('Getting started', lambda: self.show_help('EDITOR_WORKFLOW.md'))
        self.help_menu.addAction('API reference', lambda: self.show_help('api:world.status'))
        self.help_menu.addAction('Mission editor guide', lambda: self.show_help('MISSION_EDITOR.md'))
        self.help_search.textChanged.connect(self.filter_help)
        self.help_topics.currentItemChanged.connect(lambda item, old: self.render_help(item) if item else None)
        self.help_index = {}
        guides = self.bundle.get('guides', {})
        for name, text in guides.items():
            title = text.splitlines()[0].lstrip('# ').strip() if text else name
            self.help_index[name] = (title + ' ' + text).casefold()
            item = QListWidgetItem(title); item.setData(Qt.UserRole, name); self.help_topics.addItem(item)
        for operation in self.bundle.get('reference', {}).get('operations', []):
            key = 'api:' + operation['name']
            self.help_index[key] = json.dumps(operation, ensure_ascii=False).casefold()
            item = QListWidgetItem(operation['name']); item.setData(Qt.UserRole, key); self.help_topics.addItem(item)

    def context_help(self):
        if self.source and self.tabs.currentWidget() is self.code:
            cursor = self.code.textCursor(); offset = cursor.positionInBlock()
            for match in re.finditer(r'\b(?:game\.)?([a-z_]+\.[a-z_]+)\b', cursor.block().text()):
                if match.start() <= offset <= match.end() and match.group(1) in self.bundle['operations']:
                    self.show_help('api:' + match.group(1)); return
        self.show_help('EDITOR_WORKFLOW.md')

    def show_help(self, name):
        self.help_search.clear(); self.help_dock.show(); self.help_dock.raise_()
        for i in range(self.help_topics.count()):
            item = self.help_topics.item(i)
            if item.data(Qt.UserRole) == name:
                self.help_topics.setCurrentItem(item); return

    def filter_help(self, text):
        words = text.casefold().split()
        for i in range(self.help_topics.count()):
            item = self.help_topics.item(i); key = item.data(Qt.UserRole)
            item.setHidden(not all(word in self.help_index.get(key, item.text().casefold()) for word in words))

    def render_help(self, item):
        key = item.data(Qt.UserRole)
        if key.startswith('api:'):
            operation = next(o for o in self.bundle['reference']['operations'] if o['name'] == key[4:])
            text = '# game.' + operation['name'] + '\n\n' + operation['description'] + '\n\n'
            text += '\n'.join('- **' + p['name'] + '**: ' + p['type'] for p in operation['parameters'])
            text += '\n\nReturns: ' + operation['returns']
        else:
            text = self.bundle.get('guides', {}).get(key, 'Guide unavailable in this package.')
        self.help_text.setMarkdown(text)

    def log(self, text):
        self.output.appendPlainText(time.strftime('%H:%M:%S  ') + str(text))
        self.statusBar().showMessage(str(text), 10000)

    def work(self, label, function, done=None, quiet=False):
        if self.jobs:
            if not quiet and all(task.quiet for task in self.jobs) and not self.pending_work:
                self.pending_work.append((label, function, done, quiet)); self.update_controls()
            return
        session = self.session
        def invoke():
            try: return function()
            except Exception as error:
                if session and not quiet: raise mod_packages.ModError(session.failure_details(error)) from error
                raise
        task = Task(invoke); task.callback = done; task.quiet = quiet; task.label = label
        self.jobs.add(task); self.update_controls()
        if not quiet:
            self.connection.setText(label + '…')
        task.signals.completed.connect(self.task_completed, Qt.QueuedConnection)
        self.pool.start(task)

    @Slot(object, object, str)
    def task_completed(self, task, value, error):
        # Release the completed job before a callback opens another file or
        # refreshes the project. All widgets/timers stay on the GUI thread.
        self.jobs.discard(task)
        try:
            if error:
                if task.quiet: self.connection.setText(error)
                else:
                    self.log(error); self.output_dock.show(); self.output_dock.raise_()
            else:
                if isinstance(value, dict) and value.get('ok') is False:
                    raise ValueError(value.get('msg', value.get('error', 'The operation failed')))
                if task.callback: task.callback(value)
        except Exception as failure:
            self.log(failure)
            self.output_dock.show(); self.output_dock.raise_()
        self.update_controls()
        if not self.jobs and self.pending_work:
            next_task = self.pending_work.pop(0)
            # Keep the next foreground action busy through the handoff. An
            # empty timer gap could let another write or window close pass.
            self.work(*next_task)
        elif not self.jobs and self.close_when_idle:
            QTimer.singleShot(0, self.close)

    def update_controls(self):
        if not hasattr(self, 'actions'):
            return
        active = bool(self.session and self.session.ready())
        blocked = bool(self.pending_work or any(not task.quiet for task in self.jobs)); has = self.project is not None
        self.actions['newTranslation'].setEnabled(not blocked and not active)
        self.actions['editTranslation'].setEnabled(not blocked and not active and has and self.project.get('translation', False))
        for key in ['newProject', 'validateProject', 'exportProject', 'applyDrafts', 'testProject']:
            self.actions[key].setEnabled(not blocked and not active and (has or key == 'newProject'))
        self.actions['testProject'].setEnabled(not blocked and not active and has and self.test_available)
        self.actions['testProject'].setToolTip('Start this project with separate Test progress · F5' if self.test_available else
            'Install a game build with Modkit support before starting Test. Source editing and package tools are available.')
        current_dirty = bool(self.source and self.code.toPlainText() != self.source['text'])
        self.actions['saveSource'].setEnabled(not blocked and not active and current_dirty)
        self.actions['saveAll'].setEnabled(not blocked and not active and self.dirty)
        self.actions['reloadSource'].setEnabled(not blocked and not active and bool(self.source))
        self.actions['closeSource'].setEnabled(not blocked and isinstance(self.tabs.currentWidget(), SourceEditor))
        self.actions['quickOpen'].setEnabled(has and not blocked)
        self.actions['openProjectFolder'].setEnabled(has)
        self.actions['findSource'].setEnabled(bool(self.source))
        self.actions['gotoLine'].setEnabled(bool(self.source))
        self.actions['replaceSource'].setEnabled(bool(self.source) and not active and not blocked)
        self.viewport.set_context(self.project, self.actions['testProject'].isEnabled())
        self.viewport.start.setToolTip(self.actions['testProject'].toolTip())
        self.viewport_tools.update_state(self.live, blocked, self.project)
        self.game_library.connected = active
        self.game_library.ready = bool(self.live.get('level_ready'))
        self.game_library.blocked = blocked or bool(self.pending_source)
        self.game_library.update_controls()
        self.actions['stopTest'].setEnabled(not blocked and active)
        self.actions['saveDrafts'].setEnabled(not blocked and active and 'scene' in (self.project or {}).get('documents', []))
        for key in ['undo', 'redo', 'load', 'unload']:
            source_editor = self.documents.get('mod.json')
            available = active and self.live.get('level_ready') and 'scene' in (self.project or {}).get('documents', [])
            if not active and source_editor and self.scene_document() and key in ('undo', 'redo'):
                available = source_editor.document().isUndoAvailable() if key == 'undo' else source_editor.document().isRedoAvailable()
            self.actions['scene_' + key].setEnabled(not blocked and bool(available))
        for editor in self.source_editors(): editor.setReadOnly(active or blocked)
        self.busy.setVisible(blocked)
        self.properties.setEnabled(not blocked)
        self.asset_list.setDragEnabled(not blocked and active and 'scene' in (self.project or {}).get('documents', []))
        self.project_filter.setEnabled(not active and not blocked); self.projects.setEnabled(not active and not blocked)

    def source_editors(self):
        return [self.tabs.widget(i) for i in range(self.tabs.count()) if isinstance(self.tabs.widget(i), SourceEditor)]

    def changed_sources(self):
        return [editor for editor in self.source_editors() if editor.source and editor.toPlainText() != editor.source['text']]

    def discard(self, after=None, editors=None):
        changed = self.changed_sources() if editors is None else [e for e in editors if e.source and e.toPlainText() != e.source['text']]
        if not changed: return True
        names = '\n'.join('• ' + editor.source['path'] for editor in changed)
        answer = QMessageBox.question(self, 'Unsaved files', 'Save changes before continuing?\n\n' + names,
            QMessageBox.Save | QMessageBox.Discard | QMessageBox.Cancel, QMessageBox.Save)
        if answer == QMessageBox.Save:
            self.save_sources(changed, after); return False
        return answer == QMessageBox.Discard

    def command_palette(self, mode='commands'):
        entries = []
        if mode != 'files':
            for name, action in self.actions.items():
                if name == 'commandPalette': continue
                entries.append({'label': action.text().replace('&', ''), 'category': 'Action',
                                'shortcut': action.shortcut().toString(QKeySequence.NativeText),
                                'enabled': action.isEnabled(), 'callback': action.trigger})
        if self.project:
            for name in self.project['files']:
                entries.append({'label': name, 'category': 'File', 'callback': lambda path=name: self.open_source(path)})
        if mode != 'files':
            for name, text in self.bundle.get('guides', {}).items():
                entries.append({'label': text.splitlines()[0].lstrip('# ').strip(), 'category': 'Guide',
                                'callback': lambda key=name: self.show_help(key)})
        palette = CommandPalette(entries, self, 'Quick open file' if mode == 'files' else 'Command palette')
        if palette.exec() == QDialog.Accepted and palette.selected:
            palette.selected['callback']()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if hasattr(self, 'adaptive_timer'):
            # Adapt before a tall wide-layout minimum makes Qt expand a
            # narrow window again. The timer also settles dock rearrangement.
            self.adapt_workspace(); self.adaptive_timer.start()

    def adapt_workspace(self):
        if self.workspace_auto and not getattr(self, '_adapting_workspace', False):
            self._adapting_workspace = True
            try:
                if self.width() < 1000 and not self.compact_mode: self.set_compact_workspace(True)
                elif self.width() >= 1120 and self.compact_mode: self.set_compact_workspace(False)
            finally:
                self._adapting_workspace = False

    def set_compact_workspace(self, compact, manual=False):
        if manual: self.workspace_auto = False
        if self.compact_mode == compact: return
        if compact:
            self.wide_layout = self.saveState()
            for dock in [self.file_dock, self.scene_dock, self.property_dock]:
                self.addDockWidget(Qt.LeftDockWidgetArea, dock); self.tabifyDockWidget(self.project_dock, dock)
            self.project_dock.raise_(); self.resizeDocks([self.project_dock], [230], Qt.Horizontal)
        else:
            self.restoreState(self.wide_layout)
        self.compact_mode = compact; self.actions['compactWorkspace'].setChecked(compact)

    def reset_workspace(self):
        if self.compact_mode: self.set_compact_workspace(False)
        self.restoreState(self.default_layout); self.workspace_auto = True; self.adapt_workspace()

    def find_source(self, replace=False):
        if self.source:
            self.tabs.setCurrentWidget(self.code); self.find_bar.open(replace)

    def goto_line(self):
        if not self.source: return
        line, accepted = QInputDialog.getInt(self, 'Go to line', self.source['path'],
            self.code.textCursor().blockNumber() + 1, 1, self.code.blockCount())
        if accepted:
            self.tabs.setCurrentWidget(self.code)
            self.code.setTextCursor(QTextCursor(self.code.document().findBlockByNumber(line - 1)))
            self.code.centerCursor(); self.code.setFocus()

    def open_project_folder(self):
        if self.project: QDesktopServices.openUrl(QUrl.fromLocalFile(self.project['folder']))

    def refresh_projects(self, select=None):
        def done(items):
            self.projects.clear()
            for value in items:
                if value.get('editable'):
                    item = QListWidgetItem(value['name'] + '\n' + value['id']); item.setData(Qt.UserRole, value['id']); self.projects.addItem(item)
            self.filter_projects(self.project_filter.text())
            if select:
                QTimer.singleShot(0, lambda: self.open_project(select))
        self.work('Reading projects', lambda: mod_projects.list_projects(self.backend.MODS_DIR), done)

    def filter_projects(self, text):
        for i in range(self.projects.count()):
            item = self.projects.item(i); item.setHidden(text.casefold() not in item.text().casefold())

    def new_project(self):
        if not self.discard(self.new_project):
            return
        dialog = NewProject(self.templates, self)
        if dialog.exec() == QDialog.Accepted:
            self.create_project(dialog.values())

    def create_project(self, values):
        self.work('Creating project', lambda: self.backend.change_project({'action': 'create', **values}),
                  lambda result: (self.log(result.get('msg', 'Project created')), QTimer.singleShot(0, lambda: self.refresh_projects(result['project']['id']))))

    def new_translation(self):
        if self.jobs or not self.discard(self.new_translation): return
        dialog = NewTranslation(self.backend.MODS_DIR, self)
        if dialog.exec() != QDialog.Accepted: return
        values = dialog.values()
        def create():
            from argparse import Namespace
            suffix = '.exe' if sys.platform == 'win32' else ''
            candidates = [self.backend.BUILD_DIR / ('modkit_streams' + suffix),
                          self.backend.BUILD_DIR / 'modkit' / ('modkit_streams' + suffix),
                          self.backend.ROOT / ('modkit_streams' + suffix),
                          Path(sys.executable).parent / ('modkit_streams' + suffix)]
            tool = next((str(path) for path in candidates if path.is_file()), None)
            args = Namespace(**values, folder=values['id'], stream_tool=tool)
            folder = mod_translations.new(args, self.backend.GAMEDATA, self.backend.MODS_DIR)
            mod_translations.build(Namespace(name=str(folder)), None, self.backend.MODS_DIR)
            return mod_projects.describe(folder)
        def ready(project):
            self.refresh_projects(project['id'])
            TranslationEditor(project['folder'], self).exec()
        self.work('Preparing editable text catalogs from your installed files', create, ready)

    def edit_translation(self):
        if self.jobs or not self.project or not self.project.get('translation'):
            return
        if not self.discard(self.edit_translation): return
        TranslationEditor(self.project['folder'], self).exec()

    def open_project(self, identifier):
        if self.project and self.project['id'] == identifier or self.jobs or not self.discard(lambda: self.open_project(identifier)):
            return
        def done(result):
            self.project = result['project']; self.source = None; self.dirty = False
            self.setWindowTitle(self.project['name'] + ' — The Simpsons Game Modkit [*]'); self.setWindowModified(False)
            self.populate_files()
            self.loading_source = True
            for editor in self.source_editors():
                self.tabs.removeTab(self.tabs.indexOf(editor)); editor.deleteLater()
            self.documents.clear(); self.code = self.make_source_editor(); self.tabs.addTab(self.code, 'Source')
            self.source = None; self.loading_source = False; self.live = {}; self.selected_tag = None
            self.inspector_mode = 'project'; self.asset_selection = None
            self.refresh_levels()
            self.configure_asset_store()
            self.assets = []; self.library.set_records([]); self.refresh_outliner()
            self.show_properties(); self.log('Opened ' + self.project['folder'])
            first = 'mod.json' if self.project.get('documents') else 'main.lua' if 'main.lua' in self.project['files'] else self.project['files'][0] if self.project['files'] else None
            if first:
                QTimer.singleShot(0, lambda: self.open_source(first))
        self.work('Opening project', lambda: self.backend.change_project({'action': 'open', 'id': identifier}), done)

    def file_chosen(self, item, column=0):
        if item.data(0, Qt.UserRole): self.open_source(item.data(0, Qt.UserRole))

    def populate_files(self):
        self.files.clear(); folders = {}
        for name in sorted(self.project['files']):
            parent = self.files.invisibleRootItem(); parts = name.split('/')
            for i, part in enumerate(parts[:-1]):
                key = '/'.join(parts[:i + 1])
                if key not in folders:
                    folders[key] = QTreeWidgetItem(parent, [part]); folders[key].setExpanded(True)
                    folders[key].setIcon(0, self.style().standardIcon(QStyle.SP_DirIcon))
                parent = folders[key]
            item = QTreeWidgetItem(parent, [parts[-1]]); item.setData(0, Qt.UserRole, name)
            item.setToolTip(0, name); item.setIcon(0, self.style().standardIcon(QStyle.SP_FileIcon))
        self.filter_files()

    def filter_files(self, *_):
        query = self.file_filter.text().casefold()
        def visit(item):
            own = item.data(0, Qt.UserRole)
            matches = query in str(own or item.text(0)).casefold()
            children = [visit(item.child(i)) for i in range(item.childCount())]
            visible = matches or any(children); item.setHidden(not visible); return visible
        for i in range(self.files.topLevelItemCount()): visit(self.files.topLevelItem(i))

    def open_source(self, path):
        if not self.project or self.pending_work or any(not task.quiet for task in self.jobs):
            return
        if path in self.documents:
            self.tabs.setCurrentWidget(self.documents[path]); return
        def done(value):
            editor = next((e for e in self.source_editors() if e.source is None), None)
            if editor is None:
                editor = self.make_source_editor(); self.tabs.addTab(editor, path)
            self.loading_source = True; editor.source = value; editor.setPlainText(value['text']); self.loading_source = False
            self.documents[path] = editor; self.code = editor; self.source = value
            self.tabs.setTabText(self.tabs.indexOf(editor), path); self.tabs.setCurrentWidget(editor)
            self.source_changed(editor); self.refresh_outliner(); self.show_properties(); self.update_controls()
        self.work('Opening source', lambda: self.backend.change_project({'action': 'read', 'id': self.project['id'], 'path': path}), done)

    def source_tab_changed(self, index):
        if hasattr(self, 'viewport_tools'):
            self.viewport_tools.setVisible(self.tabs.currentWidget() == self.viewport)
        editor = self.tabs.widget(index)
        if isinstance(editor, SourceEditor):
            self.code = editor; self.source = editor.source
        if hasattr(self, 'property_layout') and not self.loading_source:
            self.show_properties(); self.update_controls(); self.update_cursor()

    def source_changed(self, editor=None):
        if self.loading_source:
            return
        editor = editor or self.code
        if editor.source:
            changed = editor.toPlainText() != editor.source['text']
            self.tabs.setTabText(self.tabs.indexOf(editor), editor.source['path'] + (' *' if changed else ''))
        self.dirty = bool(self.changed_sources())
        self.setWindowModified(self.dirty); self.update_controls(); self.update_cursor()
        if editor.source and editor.source['path'] == 'mod.json':
            self.refresh_outliner()
            if not self.inspector_loading and not self.editing_properties(): self.show_properties()

    def update_cursor(self):
        cursor = self.code.textCursor(); self.cursor_status.setText('Ln ' + str(cursor.blockNumber() + 1) + ', Col ' + str(cursor.positionInBlock() + 1))

    def save_source(self):
        self.save_sources([self.code])

    def save_all(self):
        self.save_sources(self.changed_sources())

    def save_sources(self, editors, after=None):
        if self.jobs or not self.project or self.session and self.session.ready(): return
        editors = [editor for editor in editors if editor.source and editor.toPlainText() != editor.source['text']]
        if not editors:
            if after: QTimer.singleShot(0, after)
            return
        requests = [(editor, {'action': 'write', 'id': self.project['id'], 'path': editor.source['path'],
                    'text': editor.toPlainText(), 'revision': editor.source['revision']}) for editor in editors]
        def write():
            results = []
            for editor, request in requests:
                try:
                    result = self.backend.change_project(request)
                    if result.get('ok') is False: raise ValueError(result.get('msg', 'Save failed'))
                    results.append((editor, result, ''))
                except Exception as error:
                    results.append((editor, None, str(error))); break
            return results
        def done(results):
            failed = False
            for editor, result, error in results:
                if error:
                    failed = True; self.log(editor.source['path'] + ': ' + error)
                    self.output_dock.show(); self.output_dock.raise_(); continue
                editor.source = result
                if editor is self.code: self.source = result
                self.source_changed(editor)
            self.show_properties()
            if not failed:
                self.log(f'Saved {len(results)} file' + ('' if len(results) == 1 else 's'))
                if after: QTimer.singleShot(0, after)
        self.work('Saving files', write, done)

    def reload_source(self):
        editor = self.code
        if not editor.source or self.jobs or not self.discard(self.reload_source, [editor]): return
        identifier, path = self.project['id'], editor.source['path']
        def done(value):
            self.loading_source = True; editor.source = value; editor.setPlainText(value['text']); self.loading_source = False
            if editor is self.code: self.source = value
            self.source_changed(editor); self.show_properties(); self.log('Reloaded ' + path)
        self.work('Reloading source', lambda: self.backend.change_project({'action': 'read', 'id': identifier, 'path': path}), done)

    def close_source(self, index):
        editor = self.tabs.widget(index)
        if not isinstance(editor, SourceEditor) or self.jobs: return
        if not self.discard(lambda: self.close_source(self.tabs.indexOf(editor)), [editor]): return
        if editor.source: self.documents.pop(editor.source['path'], None)
        self.loading_source = True; self.tabs.removeTab(index); editor.deleteLater()
        if not self.source_editors():
            self.code = self.make_source_editor(); self.tabs.addTab(self.code, 'Source'); self.source = None
        else:
            selected = self.tabs.currentWidget()
            self.code = selected if isinstance(selected, SourceEditor) else self.source_editors()[0]
            self.source = self.code.source
        self.loading_source = False; self.source_changed(); self.show_properties(); self.refresh_outliner()

    def project_action(self, action, done=None):
        if not self.project:
            return
        if self.dirty:
            self.save_sources(self.changed_sources(), lambda: self.project_action(action, done)); return
        self.work(action.capitalize(), lambda: self.backend.change_project({'action': action, 'id': self.project['id']}),
                  done or (lambda result: self.log(result.get('msg', 'Done'))))

    def validate_project(self):
        self.project_action('validate')

    def export_project(self):
        if not self.project: return
        if self.dirty:
            self.save_sources(self.changed_sources(), self.export_project); return
        output, _ = QFileDialog.getSaveFileName(self, 'Export mod package', self.project['id'] + '-' + self.project['version'] + '.zip', 'Mod package (*.zip)')
        if not output:
            return
        self.work('Exporting package', lambda: (mod_packages.pack(Path(self.project['folder']), Path(output)), {'msg': 'Exported ' + output})[1], lambda result: self.log(result['msg']))

    def apply_drafts(self):
        identifier = self.project['id'] if self.project else None
        def done(result):
            self.log(result['msg']); self.source = self.project = None
            QTimer.singleShot(0, lambda: self.open_project(identifier))
        self.project_action('apply-drafts', done)

    def refresh_levels(self):
        try:
            levels = modkit_levels.level_options(self.backend.GAMEDATA, self.backend.EPISODE_NAMES,
                                                self.project['folder'] if self.project else None)
            descriptor = mod_packages.read_json(self.backend.BUILD_DIR / 'simpsons-modkit.json')
            supported = 'editor-viewport-1' in descriptor.get('features', [])
            self.viewport_tools.set_levels(levels, supported, '' if supported else 'Update the game build to use direct level previews')
        except (OSError, ValueError, KeyError) as error:
            self.viewport_tools.set_levels([], False, str(error))

    def load_level(self, identity):
        if not identity: return
        if not self.project:
            if self.backend.game_running_pids():
                choice = QMessageBox.question(self, 'Switch quick-launch level?',
                    'Restart the game in the selected map?', QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
                if choice != QMessageBox.Yes: return
                self.work('Stopping game', self.backend.stop_game,
                          lambda _: QTimer.singleShot(0, lambda: self.load_level(identity)))
                return
            self.work('Quick launching level',
                      lambda: self.backend.launch_game(quick_launch=identity),
                      lambda res: self.log(res[1] if isinstance(res, tuple) else str(res)))
            return
        if self.session and self.session.ready():
            choice = QMessageBox.question(self, 'Switch preview level?',
                'Restart Test in the selected map? Save live drafts and apply them first to keep live edits. '
                'Unsaved preview changes will be discarded.', QMessageBox.Yes | QMessageBox.No, QMessageBox.No)
            if choice != QMessageBox.Yes: return
            self.stop_test(lambda: self.test_project(identity)); return
        self.test_project(identity)

    def test_project(self, preview=None):
        if isinstance(preview, bool): preview = None
        if not self.project: return
        if preview:
            selected = self.viewport_tools.level.findData(preview)
            if selected >= 0: self.viewport_tools.level.setCurrentIndex(selected)
        if self.dirty:
            self.save_sources(self.changed_sources(), lambda: self.test_project(preview)); return
        identifier, started = self.project['id'], time.time_ns()
        def done(result):
            self.session = EditorSession(self.backend, identifier, started)
            self.preview_pending = preview is not None
            self.pending_source = None
            self.last_epoch = None; self.assets = []; self.live = {}; self.tabs.setCurrentWidget(self.viewport)
            self.log(result['msg']); self.poll()
        self.work('Loading preview level' if preview else 'Starting Test',
                  lambda: self.backend.change_project({'action':'test', 'id':identifier, 'preview':preview}), done)

    def stop_test(self, after=None):
        if not self.session:
            return
        self.viewport.detach()
        def done(result):
            ok, message = result
            if not ok:
                raise ValueError(message)
            self.session = None; self.live = {}; self.assets = []; self.library.set_records([])
            self.asset_selection = None; self.inspector_mode = 'project'; self.selected_tag = None
            self.preview_pending = False; self.pending_source = None
            self.refresh_outliner(); self.show_properties(); self.log('Test stopped. Apply saved drafts to keep authored documents.')
            if callable(after): QTimer.singleShot(0, after)
        self.work('Stopping Test', self.backend.stop_game, done)

    def poll(self):
        if self.jobs or self.pending_work:
            return
        self.test_available = self.backend.GAME_BIN.is_file() and self.backend.modkit_ready()
        if not self.session or not self.session.ready():
            if self.session:
                self.viewport.detach(); self.session = None; self.live = {}; self.assets = []
                self.library.set_records([]); self.asset_selection = None; self.inspector_mode = 'project'
                self.refresh_outliner(); self.show_properties()
            self.connection.setText(('Project ready' if self.test_available else 'Source tools ready · Install a Modkit-enabled game build to Test')
                                    if self.project else 'Create or open a project · F1 opens help')
            self.update_controls(); return
        self.viewport.attach(self.backend.game_proc.pid)
        self.viewport.set_loading(self.preview_pending and not self.live.get('level_ready'))
        self.work('Connecting to game', lambda: self.session.snapshot(assets=self.last_epoch is None, epoch=self.last_epoch), self.accept_snapshot, quiet=True)

    def accept_snapshot(self, value):
        previous = self.live.get('state', {}).get('scene')
        self.live = value
        if value.get('assets') is not None:
            self.assets = value['assets']; self.library.set_records(self.assets)
            if self.asset_selection:
                key = asset_key(self.asset_selection)
                self.asset_selection = next((asset for asset in self.assets if asset_key(asset) == key), None)
                if self.asset_selection is None: self.inspector_mode = 'project'
        elif not value.get('level_ready'):
            self.assets = []; self.library.set_records([])
        world = value.get('world', {})
        if world.get('epoch') != self.last_epoch:
            self.last_epoch = world.get('epoch')
        if previous != value.get('state', {}).get('scene'):
            self.refresh_outliner(); self.pending_scene_refresh = True
        if self.pending_scene_refresh and not self.editing_properties():
            self.pending_scene_refresh = False; self.show_properties()
        status = next((row['label'] for page in value.get('pages', []) if page['id'].endswith(':scene')
                       for row in page['rows'] if row['id'] == 'status'), None)
        if status and status != getattr(self, 'scene_status', None):
            self.scene_status = status; self.log(status)
        self.connection.setText(('Map workspace · ' if value.get('viewport',{}).get('preview') else 'Test connected · ')
                                + world.get('level_root', 'Load an episode to edit'))
        if value.get('level_ready') and self.preview_pending:
            self.preview_pending = False
            self.viewport.set_loading(False)
            QTimer.singleShot(0, lambda: self.set_camera('paused'))
        self.check_source_load(value)

    def set_camera(self, mode):
        if not self.session: return
        self.work('Changing editor camera', lambda: self.session.camera(mode), self.accept_snapshot)

    def set_layers(self, mode):
        if not self.session: return
        tag = self.selected_tag
        self.work('Changing viewport layers', lambda: self.session.view(mode, tag), self.accept_snapshot)

    def frame_selection(self):
        if not self.session or not self.selected_tag:
            self.log('Select a live scene object in the outliner first'); return
        distance = self.viewport_tools.distance.value()
        self.work('Framing scene selection', lambda: self.session.focus(self.selected_tag, distance), self.accept_snapshot)

    def search_game_library(self):
        if not self.session or not self.session.ready():
            self.configure_asset_store()
            query=self.game_library.query();value=self.asset_store.query(**query)
            if value:
                self.game_library.offline_ready=True;self.game_library.accept(value)
            return
        if self.jobs or self.pending_work:
            self.library_retry.start(); return
        query = self.game_library.query()
        def done(value):
            self.game_library.accept(value)
            if not value.get('ready') and self.asset_tabs.currentIndex() == 1:
                QTimer.singleShot(1500, self.search_game_library)
        self.work('Searching installed archives', lambda: self.session.call('assets.search', **query), done)

    def configure_asset_store(self):
        tool=getattr(self.backend,'MODKIT_ASSET_TOOL',self.backend.BUILD_DIR/('modkit_streams.exe' if sys.platform=='win32' else 'modkit_streams'))
        if not Path(tool).is_file():
            tool=self.backend.ROOT/('modkit_streams.exe' if sys.platform=='win32' else 'modkit_streams')
        overlays=[Path(self.project['folder'])/'gamedata'] if self.project else []
        key=(str(tool),str(self.backend.GAMEDATA),tuple(map(str,overlays)))
        if self.asset_store and self.asset_store.key==key:return
        self.cancel_hover()
        if self.asset_store:self.asset_store.close()
        store=AssetStore(tool,self.backend.GAMEDATA,overlays,self,environment=self.backend.external_environment())
        store.key=key;self.asset_store=store
        store.changed.connect(lambda:self.offline_index_ready() if self.asset_store is store else None)
        store.progress.connect(lambda text:self.game_library.count.setText(text) if self.asset_store is store else None)
        store.meshReady.connect(lambda identity,value:self.accept_model_preview(identity,value) if self.asset_store is store else None)
        store.meshFailed.connect(lambda identity,error:self.model_preview_failed(identity,error) if self.asset_store is store else None)

    def offline_index_ready(self):
        self.game_library.offline_ready=True
        if self.asset_tabs.currentIndex()==1 and not self.session:self.search_game_library()

    def queue_hover(self,asset):
        if not asset or not (asset.get('model_resource') or asset.get('native_class') in ('MetaModel','EARS_MESH','CHA')):
            self.cancel_hover();return
        if self.hover_asset and self.hover_asset.get('id')==asset.get('id'):return
        self.hover_popup.hide();self.hover_asset=asset
        if hasattr(self,'hover_timer'):self.hover_timer.start()

    def cancel_hover(self):
        self.hover_asset=None
        if hasattr(self,'hover_timer'):self.hover_timer.stop()
        if hasattr(self,'hover_popup'):self.hover_popup.hide()
        if self.asset_store:self.asset_store.wanted=None

    def eventFilter(self,watched,event):
        if event.type() in (QEvent.Leave,QEvent.Hide):self.cancel_hover()
        return super().eventFilter(watched,event)

    def open_hover_preview(self):
        asset=self.hover_asset
        if not asset:return
        self.configure_asset_store()
        # Creating the first store clears stale hover requests; retain this
        # exact current row only after the new source identity is installed.
        self.hover_asset=asset;self.hover_popup.open_for(asset);self.asset_store.request_mesh(asset)

    def open_model_preview(self,asset):
        if not asset:return
        self.cancel_hover();self.configure_asset_store()
        identity=self.asset_store.identity(asset)
        self.model_preview.loading(asset,identity);self.tabs.setCurrentWidget(self.model_preview)
        self.asset_store.request_mesh(asset)

    def accept_model_preview(self,identity,document):
        if self.model_preview.identity==identity:self.model_preview.accept(document)
        if self.hover_asset and self.hover_popup.isVisible() and self.asset_store.identity(self.hover_asset)==identity:
            self.hover_popup.view.set_mesh(document)

    def model_preview_failed(self,identity,error):
        if self.model_preview.identity==identity or identity=='index':
            self.model_preview.view.error=error;self.model_preview.view.update()
        if self.hover_popup.isVisible() and (identity=='index' or self.hover_asset and self.asset_store.identity(self.hover_asset)==identity):
            self.hover_popup.view.error=error;self.hover_popup.view.update()
        if identity=='index':self.game_library.count.setText(error)

    def inspect_library_asset(self, asset):
        if not asset: return
        self.game_library.describe(asset)
        self.inspect_asset({**asset, 'class':asset.get('native_class'), 'source_stream':asset.get('source'),
                            'model_resource':asset.get('native_name'), 'library':True})

    def choose_library_definition(self, asset, preview, place=False):
        if not asset or not self.session: return
        def done(value):
            nonlocal asset
            asset={**asset,'id':value.pop('asset_id'),'_offline':False}
            definitions = value['items']
            if not definitions:
                self.log('This resource has no indexed entity definitions. It can be inspected, but not placed as a prop.'); return
            if value['total'] == 1: definition = definitions[0]
            else:
                picker = DefinitionPicker(value, self)
                picker.pageRequested.connect(lambda offset: self.work('Reading source variants',
                    lambda: self.session.call('assets.templates',asset=asset['id'],offset=offset,limit=100),
                    lambda result: picker.set_page(result,offset)))
                if picker.exec() != QDialog.Accepted: return
                definition = picker.selected
            self.prepare_library_source(asset, definition, preview, place)
        def query():
            identity=self.session.resolve_library_asset(asset)
            return {**self.session.call('assets.templates',asset=identity,limit=100),'asset_id':identity}
        self.work('Reading native definitions', query, done)

    def prepare_library_source(self, asset, definition, preview=False, place=False):
        if not definition.get('supported'):
            self.log('This factory has no qualified native placement adapter. Inspect the resource details instead.'); return
        def done(_):
            self.pending_source = {'asset':asset, 'definition':definition, 'preview':preview, 'place':place}
            self.game_library.status.setText('Loading native source and dependencies: ' + definition['stream'])
        self.work('Preparing original source', lambda: self.session.call('assets.load_source', stream=definition['stream']), done)

    def check_source_load(self, value):
        self.game_library.source_progress(value.get('source', {}), bool(self.pending_source))
        if not self.pending_source: return
        source = value.get('source', {}); pending = self.pending_source
        if source.get('phase') == 'failed':
            self.pending_source = None; self.log(source.get('message', 'Source loading failed')); return
        if not source.get('ready') or source.get('stream') != pending['definition']['stream']: return
        self.pending_source = None
        def done(result):
            definition = next((item for item in result['items'] if item['source_guid'] == pending['definition']['source_guid']
                               and item['class_hash'] == pending['definition']['class_hash']), None)
            if not definition or not definition.get('resident_template'):
                self.log('The source loaded, but this definition has no retained placement identity'); return
            self.game_library.status.setText('Source ready: ' + definition['stream'])
            if pending.get('place'):
                QTimer.singleShot(0,lambda:self.place_asset(definition['resident_template']))
            elif pending['preview']:
                distance = self.viewport_tools.distance.value()
                QTimer.singleShot(0, lambda: self.work('Previewing native prop', lambda: self.session.preview_prop(definition,distance),
                    lambda result: (self.accept_snapshot(result['snapshot']), self.tabs.setCurrentWidget(self.viewport),
                                    self.log('Temporary native prop preview. Use Loaded resources to add it to your scene.'))))
            else:
                QTimer.singleShot(0, self.refresh_assets)
        QTimer.singleShot(0, lambda: self.work('Resolving prepared prop',
            lambda: self.session.call('assets.templates', asset=pending['asset']['id'],
                                      offset=pending['definition'].get('_page_offset',0),limit=100), done))

    def cancel_asset_source(self):
        if not self.session or not self.pending_source: return
        def done(_):
            self.pending_source = None
            self.game_library.source_progress({},False)
            self.game_library.status.setText('Source cancellation requested. Existing owned objects retain their resources.')
        self.work('Cancelling source preparation', lambda: self.session.call('assets.cancel_source'), done)

    def clear_asset_preview(self):
        if not self.session: return
        self.work('Clearing temporary preview', lambda: (self.session.clear_preview(), self.session.snapshot())[1], self.accept_snapshot)

    def drop_library_asset(self,asset):
        self.viewport.show_drop(False)
        if not self.session or not self.live.get('level_ready'):
            self.log('Load a map workspace before placing library resources. Offline 3D previews do not require Play.');return
        if 'scene' not in (self.project or {}).get('documents',[]):
            self.log('Use a Scene or Mission project to retain placed objects.');return
        self.choose_library_definition(asset,False,place=True)

    def refresh_assets(self):
        if not self.session:
            self.log('Start Test and load an episode to inspect native assets'); return
        self.work('Reading native assets', lambda: self.session.snapshot(assets=True), self.accept_snapshot)

    def filter_assets(self, text=None):
        self.library.filter()

    def place_asset(self, identity):
        if not self.session or 'scene' not in (self.project or {}).get('documents', []):
            self.log('Use a Scene editor or Mission editor project and load a level in Test'); return
        def done(value):
            self.selected_tag = value.get('selected_tag'); self.inspector_mode = 'object'; self.accept_snapshot(value)
            self.tabs.setCurrentWidget(self.viewport); self.log('Prop placed through the native Scene editor. Save live drafts to keep it.')
        self.work('Placing prop', lambda: self.session.place(identity), done)

    def scene_action(self, row):
        if not self.session:
            if row == 'undo': self.scene_source_undo(False)
            elif row == 'redo': self.scene_source_undo(True)
            return
        def done(value):
            self.accept_snapshot(value)
            self.log('Drafts saved. Stop Test, then Apply saved drafts.' if row == 'save' else 'Scene: ' + row)
        self.work('Updating scene', lambda: self.session.action(row), done)

    def edit_object(self, row, value=None):
        if not self.selected_tag or self.inspector_loading: return
        if not self.session:
            self.edit_offline_object(row, value); return
        tag = self.selected_tag
        def done(snapshot):
            if row == 'duplicate':
                old = {obj['id'] for obj in self.live.get('state', {}).get('scene', {}).get('objects', [])}
                added = [obj['id'] for obj in snapshot.get('state', {}).get('scene', {}).get('objects', []) if obj['id'] not in old]
                if len(added) == 1: self.selected_tag = added[0]
            self.accept_snapshot(snapshot)
        self.work('Editing prop', lambda: self.session.edit(tag, row, value), done)

    def editing_properties(self):
        focus = QApplication.focusWidget()
        return bool(focus and self.properties.isAncestorOf(focus))

    def scene_document(self):
        if self.session and self.session.ready():
            value = self.live.get('state', {}).get('scene')
            return value if isinstance(value, dict) else None
        editor = self.documents.get('mod.json')
        if editor:
            try:
                value = json.loads(editor.toPlainText()).get('scene')
                if value is not None: return mod_projects.scenes.validate(value)
            except (ValueError, TypeError): pass
        return None

    def refresh_outliner(self):
        if not hasattr(self, 'scene_tree'): return
        scene = self.scene_document(); objects = scene.get('objects', []) if scene else []
        self.scene_tree.blockSignals(True); self.scene_tree.clear()
        if not any(obj['id'] == self.selected_tag for obj in objects):
            self.selected_tag = objects[0]['id'] if objects else None
        selected = None
        for obj in objects:
            item = QTreeWidgetItem([obj['name'], obj['source']['class']]); item.setData(0, Qt.UserRole, obj['id'])
            item.setToolTip(0, obj['id'] + '\n' + obj['source']['stream']); self.scene_tree.addTopLevelItem(item)
            if obj['id'] == self.selected_tag: selected = item
        if selected: self.scene_tree.setCurrentItem(selected)
        self.scene_tree.blockSignals(False); self.filter_scene()
        active = bool(self.session and self.session.ready())
        self.scene_hint.setText(f'{len(objects)}/64 objects · ' + ('Live preview' if active else 'Project source · Save all to keep edits')
                               if scene else 'Open mod.json in a Scene or Mission project. Place props in Test to start a scene.')

    def filter_scene(self, *_):
        if not hasattr(self, 'scene_tree'): return
        words = self.scene_filter.text().casefold().split()
        for index in range(self.scene_tree.topLevelItemCount()):
            item = self.scene_tree.topLevelItem(index)
            text = (item.text(0) + ' ' + item.text(1) + ' ' + item.toolTip(0)).casefold()
            item.setHidden(not all(word in text for word in words))

    def select_scene_object(self, item, previous=None):
        if item is None: return
        self.selected_tag = item.data(0, Qt.UserRole); self.inspector_mode = 'object'; self.show_properties()
        if self.compact_mode: self.property_dock.show(); self.property_dock.raise_()

    def inspect_asset(self, asset):
        if not asset: return
        self.asset_selection = asset; self.inspector_mode = 'asset'; self.show_properties()
        if self.compact_mode: self.property_dock.show(); self.property_dock.raise_()

    def replace_document(self, editor, text):
        position = editor.textCursor().position()
        cursor = QTextCursor(editor.document()); cursor.beginEditBlock(); cursor.select(QTextCursor.Document)
        cursor.insertText(text); cursor.endEditBlock()
        cursor.setPosition(min(position, editor.document().characterCount() - 1)); editor.setTextCursor(cursor)

    def edit_offline_object(self, row, number=None):
        import copy, math
        editor = self.documents.get('mod.json')
        if not editor or editor.isReadOnly(): return
        try:
            manifest = json.loads(editor.toPlainText()); scene = mod_projects.scenes.validate(manifest['scene'])
            obj = next(item for item in scene['objects'] if item['id'] == self.selected_tag)
            if row.startswith('position-'):
                obj['transform']['position'][row[-1]] = number
            elif row == 'rename':
                if obj['name'] == number: return
                obj['name'] = number
            elif row == 'remove': scene['objects'].remove(obj)
            elif row == 'duplicate':
                if len(scene['objects']) >= 64: raise ValueError('A scene supports up to 64 objects')
                duplicate = copy.deepcopy(obj); tags = {item['id'] for item in scene['objects']}
                index = 1
                while f'scene-{index}' in tags: index += 1
                duplicate['id'] = f'scene-{index}'; duplicate['transform']['position']['x'] += 1
                scene['objects'].append(duplicate); self.selected_tag = duplicate['id']
            elif row in ('turn-left', 'turn-right', 'grow', 'shrink'):
                angle = math.radians(-15 if row == 'turn-left' else 15); c, s = math.cos(angle), math.sin(angle)
                for axis in ('right', 'up', 'forward'):
                    vector = obj['transform'][axis]
                    if row in ('grow', 'shrink'):
                        factor = 1.1 if row == 'grow' else 1 / 1.1
                        obj['transform'][axis] = {k: v * factor for k, v in vector.items()}
                    else:
                        obj['transform'][axis] = {'x': vector['x'] * c + vector['z'] * s,
                            'y': vector['y'], 'z': vector['z'] * c - vector['x'] * s}
            else: return
            manifest['scene'] = mod_projects.scenes.validate(scene)
            self.replace_document(editor, json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
            if editor is self.code: self.source = editor.source
            self.refresh_outliner()
            if row in ('remove', 'duplicate') or not self.editing_properties(): self.show_properties()
            self.log('Scene source updated · Save all to keep edits')
        except (ValueError, KeyError, StopIteration, TypeError) as error: self.log(error)

    def scene_source_undo(self, redo):
        editor = self.documents.get('mod.json')
        if editor and not editor.isReadOnly():
            editor.redo() if redo else editor.undo()
            self.refresh_outliner(); self.show_properties()

    def clear_properties(self):
        while self.property_layout.count():
            item = self.property_layout.takeAt(0)
            if item.widget():
                item.widget().deleteLater()
            elif item.layout():
                while item.layout().count():
                    child = item.layout().takeAt(0)
                    if child.widget(): child.widget().deleteLater()
        self.inspector_loading = True

    def spin(self, value, low, high, changed):
        widget = QDoubleSpinBox(); widget.setRange(low, high); widget.setDecimals(6 if 0 < low < .001 else 3); widget.setSingleStep(.1); widget.setValue(float(value))
        widget.setKeyboardTracking(False); widget.valueChanged.connect(changed); return widget

    def show_properties(self):
        self.clear_properties()
        if not self.project:
            self.property_layout.addWidget(QLabel('Create a project to get started.\n\nF1 opens built-in guides and API help.'))
        else:
            title = QLabel(self.project['name']); title.setWordWrap(True); title.setObjectName('inspectorTitle'); self.property_layout.addWidget(title)
            info = QLabel(self.project['id'] + '\n' + self.project.get('author', '') + '\nVersion ' + self.project['version']); info.setWordWrap(True); info.setTextInteractionFlags(Qt.TextSelectableByMouse); self.property_layout.addWidget(info)
            scene = self.scene_document()
            if self.inspector_mode == 'asset' and self.asset_selection:
                asset = self.asset_selection
                title = QLabel(asset['name']); title.setTextFormat(Qt.PlainText); title.setWordWrap(True)
                title.setObjectName('inspectorTitle'); self.property_layout.addWidget(title)
                info = QLabel('Placeable prop' if placeable(asset) else 'Resource inspection')
                info.setObjectName('mutedLabel'); self.property_layout.addWidget(info)
                for label, key in [('Type', 'class'), ('Category', 'category'), ('Archive', 'source_stream'),
                                    ('Definition', 'source_guid'), ('Model', 'model_resource')]:
                    text = QLabel(label + '\n' + str(asset.get(key) or 'Not supplied'))
                    text.setTextFormat(Qt.PlainText); text.setWordWrap(True); text.setTextInteractionFlags(Qt.TextSelectableByMouse)
                    self.property_layout.addWidget(text)
                if placeable(asset):
                    button = QPushButton('Place beside character'); button.setObjectName('placeInspectedAsset')
                    button.setEnabled(bool(self.session and self.session.ready() and self.live.get('level_ready')
                                           and 'scene' in self.project.get('documents', [])))
                    button.clicked.connect(lambda: self.place_asset(asset['id'])); self.property_layout.addWidget(button)
                else:
                    hint = QLabel('This type has no qualified prop placement path. Its source identity can be copied from the asset context menu.')
                    hint.setWordWrap(True); self.property_layout.addWidget(hint)
            elif scene and isinstance(scene.get('objects'), list) and scene['objects']:
                if not any(o['id'] == self.selected_tag for o in scene['objects']):
                    self.selected_tag = scene['objects'][0]['id']
                selected = next(o for o in scene['objects'] if o['id'] == self.selected_tag)
                active = bool(self.session and self.session.ready())
                title = QLabel(selected['name']); title.setTextFormat(Qt.PlainText); title.setObjectName('inspectorTitle')
                title.setWordWrap(True); self.property_layout.addWidget(title)
                state = QLabel('Live preview · native undo history' if active else 'Project source · source undo history')
                state.setObjectName('mutedLabel'); self.property_layout.addWidget(state)
                form = QFormLayout()
                name = QLineEdit(selected['name']); name.setObjectName('sceneObjectName'); name.setMaxLength(160)
                name.setReadOnly(active)
                name.setToolTip('Stop Test to rename the portable source object.' if active else 'The object tag stays unchanged when its name changes.')
                name.editingFinished.connect(lambda: self.edit_object('rename', name.text()) if not active else None)
                form.addRow('Name', name)
                for axis in ['x', 'y', 'z']:
                    field = self.spin(selected['transform']['position'][axis], -1000000, 1000000,
                        lambda number, key=axis: self.edit_object('position-' + key, number))
                    field.setObjectName('scenePosition' + axis.upper()); form.addRow(axis.upper(), field)
                self.property_layout.addLayout(form)
                for options in [[('Rotate −15°', 'turn-left'), ('Rotate +15°', 'turn-right')],
                                [('Scale ÷1.1', 'shrink'), ('Scale ×1.1', 'grow')],
                                [('Duplicate', 'duplicate'), ('Remove', 'remove')]]:
                    row_layout = QHBoxLayout()
                    for text, row in options:
                        button = QPushButton(text); button.setObjectName('scene_' + row)
                        button.clicked.connect(lambda checked=False, key=row: self.edit_object(key)); row_layout.addWidget(button)
                    self.property_layout.addLayout(row_layout)
                identity = QLabel('Tag: ' + selected['id'] + '\n' + selected['source']['class'] + '\n' + selected['source']['stream'])
                identity.setTextFormat(Qt.PlainText); identity.setWordWrap(True); identity.setTextInteractionFlags(Qt.TextSelectableByMouse)
                self.property_layout.addWidget(identity)
            elif self.source and self.source['path'] == 'mod.json' and not (self.session and self.session.ready()):
                try:
                    value = json.loads(self.code.toPlainText())
                    if 'mission' in value:
                        self.mission_properties(value)
                except (ValueError, KeyError, TypeError):
                    self.property_layout.addWidget(QLabel('Fix the JSON source before editing properties.'))
            else:
                hint = QLabel('Scene and Mission projects expose their native tools through Options → Extras → Mods during Test.\n\nOpen mod.json for mission properties. Use the built-in guides for each template.'); hint.setWordWrap(True); self.property_layout.addWidget(hint)
        self.property_layout.addStretch(); self.inspector_loading = False

    def mission_properties(self, source):
        doc = source['mission']; form = QFormLayout(); title = QLineEdit(doc['name']); title.setMaxLength(160)
        title.setObjectName('missionTitle'); title.textEdited.connect(lambda text: self.mission_edit(lambda d: d.update(name=text), False)); form.addRow('Mission title', title)
        description = QLineEdit(doc.get('description', '')); description.setMaxLength(1024)
        description.textEdited.connect(lambda text: self.mission_edit(lambda d: d.update(description=text), False)); form.addRow('Description', description)
        form.addRow('Time limit', self.spin(doc.get('time_limit', 0), 0, 86400, lambda number: self.mission_edit(lambda d: d.update(time_limit=number))))
        self.property_layout.addLayout(form)
        for text, key, default in [('Start on episode entry', 'auto_start', False), ('Fail when all players fall', 'fail_on_defeat', True)]:
            widget = QCheckBox(text); widget.setChecked(doc.get(key, default)); widget.toggled.connect(lambda flag, field=key: self.mission_edit(lambda d: d.update({field: flag}))); self.property_layout.addWidget(widget)
        if not doc['objectives']:
            hint = QLabel('Place the first objective through the native Mission editor in Test.'); hint.setWordWrap(True); self.property_layout.addWidget(hint); return
        selector = QComboBox()
        for item in doc['objectives']: selector.addItem(item['title'], item['id'])
        selected_id = getattr(self, 'mission_selection', doc['objectives'][0]['id'])
        selector.setCurrentIndex(max(0, selector.findData(selected_id))); self.property_layout.addWidget(selector)
        selector.currentIndexChanged.connect(lambda: (setattr(self, 'mission_selection', selector.currentData()), self.show_properties()))
        point_id = selector.currentData(); point = next(o for o in doc['objectives'] if o['id'] == point_id)
        def change(field, value, contract=True):
            self.mission_edit(lambda d: next(o for o in d['objectives'] if o['id'] == point_id).update({field: value}), contract)
        form = QFormLayout(); title = QLineEdit(point['title']); title.setMaxLength(240); title.textEdited.connect(lambda text: change('title', text, False)); form.addRow('Objective', title)
        kind = QComboBox(); kind.addItems(['reach', 'stay', 'wait']); kind.setCurrentText(point['kind']); kind.currentTextChanged.connect(lambda text: change('kind', text)); form.addRow('Rule', kind)
        for axis in ['x', 'y', 'z']:
            form.addRow(axis.upper(), self.spin(point['position'][axis], -1000000, 1000000,
                lambda number, key=axis: self.mission_edit(lambda d: next(o for o in d['objectives'] if o['id'] == point_id)['position'].update({key: number}))))
        form.addRow('Radius', self.spin(point['radius'], .000001, 1000, lambda number: change('radius', number)))
        form.addRow('Seconds', self.spin(point['seconds'], .1, 86400, lambda number: change('seconds', number)))
        self.property_layout.addLayout(form)
        for text, field in [('Optional', 'optional'), ('Retry point', 'checkpoint')]:
            widget = QCheckBox(text); widget.setChecked(point[field]); widget.toggled.connect(lambda flag, key=field: change(key, flag)); self.property_layout.addWidget(widget)
        self.property_layout.addWidget(QLabel('Prerequisites'))
        for other in doc['objectives']:
            if other['id'] == point_id: continue
            widget = QCheckBox(other['title']); widget.setChecked(other['id'] in point['requires'])
            def link(flag, parent=other['id']):
                def update(d):
                    current = next(o for o in d['objectives'] if o['id'] == point_id)
                    values = set(current['requires']); values.add(parent) if flag else values.discard(parent); current['requires'] = sorted(values)
                self.mission_edit(update)
            widget.toggled.connect(link); self.property_layout.addWidget(widget)

    def mission_edit(self, change, contract=True):
        if self.inspector_loading or self.code.isReadOnly():
            return
        try:
            value = json.loads(self.code.toPlainText()); change(value['mission'])
            if contract:
                value['mission']['revision'] = min(4294967295, json.loads(self.source['text'])['mission'].get('revision', 1) + 1)
            self.replace_document(self.code, json.dumps(value, indent=2, ensure_ascii=False) + '\n')
        except (ValueError, KeyError, StopIteration) as error:
            self.log(error)

    def closeEvent(self, event):
        if self.jobs or self.pending_work:
            if self.jobs and all(task.quiet for task in self.jobs) and not self.pending_work:
                self.close_when_idle = True; self.timer.stop()
            else:
                self.log('Wait for the current operation to finish before closing')
            event.ignore(); return
        if self.close_when_idle:
            self.close_when_idle = False; self.timer.start(1200)
        if not self.discard(self.close):
            event.ignore(); return
        if self.session and self.session.ready():
            if QMessageBox.question(self, 'Test running', 'Stop the Test game and close Modkit?',
                    QMessageBox.Yes | QMessageBox.Cancel, QMessageBox.Cancel) != QMessageBox.Yes:
                event.ignore(); return
            self.viewport.detach()
            ok, message = self.backend.stop_game()
            if not ok:
                self.log(message); event.ignore(); return
        self.settings.setValue('geometry', self.saveGeometry()); self.settings.setValue('layout', self.saveState())
        self.settings.setValue('compactMode', self.compact_mode)
        self.settings.setValue('workspaceAuto', self.workspace_auto)
        self.settings.setValue('wideLayout', self.wide_layout if self.compact_mode else self.saveState())
        self.timer.stop(); self.viewport.detach(); self.cancel_hover()
        if self.asset_store:self.asset_store.close()
        super().closeEvent(event)


def apply_theme(app):
    app.setStyleSheet('''
        QWidget { background:#20242b; color:#dce1e8; font-family:Sans; font-size:12px; }
        QMainWindow::separator { background:#11151b; width:4px; height:4px; }
        QDockWidget::title { background:#2a3039; padding:7px; font-weight:600; }
        QPlainTextEdit,QTextBrowser,QTreeView,QTreeWidget,QListWidget,QLineEdit,QComboBox,QDoubleSpinBox { background:#181c22; border:1px solid #3a424f; selection-background-color:#3b546d; padding:5px; }
        QTreeView { alternate-background-color:#1d222a; } QHeaderView::section { background:#242b35; padding:6px; border:0; border-bottom:1px solid #414c5b; text-align:left; }
        QLineEdit:focus,QPlainTextEdit:focus { border:1px solid #809fc7; }
        QPlainTextEdit#sourceEditor { padding:12px; font-family:monospace; } QPlainTextEdit#outputLog { font-family:monospace; }
        QPushButton { background:#303844; border:1px solid #465263; border-radius:3px; padding:6px 10px; }
        QPushButton:hover { background:#3a4859; } QPushButton:disabled { color:#727d8c; }
        QToolBar { border:0; spacing:4px; padding:5px; background:#262c35; }
        QToolButton { padding:7px; } QToolButton:hover { background:#3a4859; }
        QToolButton:disabled { color:#748296; }
        QTabBar::tab { padding:8px 16px; background:#282f39; } QTabBar::tab:selected { background:#394452; border-top:2px solid #d9b55b; }
        QMenu { border:1px solid #465263; } QMenu::item { padding:7px 18px; } QMenu::item:selected { background:#3b546d; }
        QMenu::item:disabled { color:#748296; }
        QLabel#inspectorTitle { font-size:15px; font-weight:600; padding:4px 0; }
        QLabel#mutedLabel { color:#9baabd; }
        QWidget#viewportWelcome { background:#121820; }
        QLabel#workspaceHeading { background:transparent; font-size:25px; font-weight:600; color:#f0f4fa; }
        QLabel#workspaceEyebrow { background:transparent; font-size:11px; font-weight:600; color:#e4bd70; }
        QLabel#workspaceDescription { background:transparent; font-size:13px; color:#acb9ca; }
        QLabel#workspaceSteps { background:transparent; color:#92a2b7; padding-top:16px; }
        QPushButton#primaryAction { background:#bd9850; color:#111820; border-color:#d6b773; font-weight:600; padding:9px 14px; }
        QPushButton#primaryAction:disabled { background:#28313d; color:#8493a6; border-color:#3e4c5e; }
        QStatusBar { border-top:1px solid #39414c; } QCheckBox { spacing:6px; }
    ''')


def configure_test_runtime(backend):
    """An optional local editor build never changes the game's Play target."""
    path = backend.LAUNCHER_DIR / 'modkit-runtime.json'
    if not path.is_file(): return
    config = mod_packages.read_json(path)
    executable = backend._resolve(config['engine'][backend.PLAT])
    descriptor = executable.parent / 'simpsons-modkit.json'
    if not executable.is_file() or not descriptor.is_file():
        raise ValueError('The configured Modkit Test runtime is missing: ' + str(executable))
    backend.GAME_BIN = executable
    backend.BUILD_DIR = executable.parent
    backend.GAME_TOML = executable.parent / 'simpsons.toml'
    if backend.PLAT in config.get('asset_tool',{}):
        backend.MODKIT_ASSET_TOOL=backend._resolve(config['asset_tool'][backend.PLAT])
    backend.CONFIG = {**backend.CONFIG, 'lib_dirs': {**backend.CONFIG['lib_dirs'],
        backend.PLAT: config.get('lib_dirs', {}).get(backend.PLAT, backend.CONFIG['lib_dirs'].get(backend.PLAT, []))}}


def run(backend, project=None):
    app = QApplication(sys.argv[:1]); app.setApplicationName('simpsons-modkit')
    app.setDesktopFileName('simpsons-modkit'); app.setOrganizationName('YesterMester'); app.setStyle('Fusion')
    if sys.platform == 'win32':
        import ctypes
        ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('YesterMester.Simpsons.Modkit')
    apply_theme(app)
    backend.LOG_FILE = backend.LAUNCHER_DIR / 'modkit.log'
    backend.startup_log('Modkit ' + backend.VERSION + ', frozen=' + str(backend.FROZEN))
    try:
        configure_test_runtime(backend)
    except (OSError, ValueError, KeyError) as error:
        backend.startup_log('Modkit Test runtime: ' + str(error))
        QMessageBox.warning(None, 'Modkit Test runtime', str(error))
    try:
        backend.install_desktop_entry()
    except OSError as error:
        backend.startup_log('desktop entry: ' + str(error))
    window = ModkitWindow(backend, project)
    available = app.primaryScreen().availableGeometry()
    if window.settings.value('geometry') is None and available.width() <= 1440:
        window.showMaximized()
    else:
        window.show()
    backend.startup_log('native Modkit window opened')
    app.exec()
