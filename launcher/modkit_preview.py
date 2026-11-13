"""Native viewport controls and a paged browser for the installed game's library."""
from PySide6.QtCore import QMimeData, Qt, QTimer, Signal
from PySide6.QtGui import QDrag
from PySide6.QtWidgets import (QComboBox, QDialog, QDialogButtonBox, QDoubleSpinBox,
    QHBoxLayout, QLabel, QLineEdit, QSizePolicy, QListWidget, QListWidgetItem,
    QProgressBar, QPushButton, QVBoxLayout, QWidget)
import json
if __package__:
    from .modkit_viewport import LIBRARY_MIME
else:
    from modkit_viewport import LIBRARY_MIME


class LibraryAssetList(QListWidget):
    dragging=Signal(bool)
    def __init__(self,parent=None):
        super().__init__(parent);self.setDragEnabled(True)

    def startDrag(self,actions):
        item=self.currentItem();asset=item.data(Qt.UserRole) if item else None
        if not asset or not asset.get('object_count'):return
        # Transfer portable source metadata, not an arbitrary command or a
        # presumed session handle. The receiver resolves the active identity.
        value={k:asset[k] for k in ('id','name','native_name','native_class','category','source','object_count','locations') if k in asset}
        value['locations']=value.get('locations',[])[:64];value['_offline']=True
        data=json.dumps(value,separators=(',',':')).encode()
        if len(data)>64*1024:return
        mime=QMimeData();mime.setData(LIBRARY_MIME,data)
        drag=QDrag(self);drag.setMimeData(mime);self.dragging.emit(True)
        try:drag.exec(Qt.CopyAction)
        finally:self.dragging.emit(False)


class ViewportTools(QWidget):
    launch = Signal(str)
    cameraChanged = Signal(str)
    layersChanged = Signal(str)
    focusRequested = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self); layout.setContentsMargins(6, 4, 6, 4); layout.setSpacing(4)
        row = QHBoxLayout(); self.level = QComboBox(); self.level.setObjectName('previewLevel')
        self.level.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.level.setMinimumContentsLength(16); row.addWidget(self.level, 1)
        self.load = QPushButton('Quick launch level'); self.load.setObjectName('loadPreviewLevel')
        self.load.clicked.connect(lambda: self.launch.emit(self.level.currentData()))
        row.addWidget(self.load); layout.addLayout(row)
        row = QHBoxLayout()
        self.camera = QComboBox(); self.camera.setObjectName('viewportCamera')
        for label, value in [('Game camera', 'game'), ('Free camera', 'free'), ('Paused editor', 'paused')]:
            self.camera.addItem(label, value)
        self.camera.activated.connect(lambda _: self.cameraChanged.emit(self.camera.currentData()))
        row.addWidget(self.camera, 1)
        self.layers = QComboBox(); self.layers.setObjectName('viewportLayers')
        for label, value in [('Full scene', 'all'), ('Map geometry', 'map'), ('Isolate selected actor', 'isolate')]:
            self.layers.addItem(label, value)
        self.layers.activated.connect(lambda _: self.layersChanged.emit(self.layers.currentData()))
        self.layers.setToolTip('Preview-only native visibility. Map geometry hides supported factory actors. '
            'Static world geometry stays visible; unsupported effects are reported below. No game files are edited.')
        row.addWidget(self.layers, 1)
        for combo in (self.camera, self.layers):
            combo.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
            combo.setMinimumContentsLength(8); combo.setMinimumWidth(60)
            combo.setSizePolicy(QSizePolicy.Ignored, QSizePolicy.Fixed)
        self.focus = QPushButton('Frame selection'); self.focus.setObjectName('frameSelection')
        self.focus.clicked.connect(self.focusRequested); row.addWidget(self.focus)
        self.distance = QDoubleSpinBox(); self.distance.setRange(1, 500); self.distance.setValue(12)
        self.distance.setDecimals(0); self.distance.setSingleStep(2); self.distance.setSuffix(' m')
        self.distance.setMaximumWidth(76)
        self.distance.setToolTip('Camera framing distance for selected objects and temporary previews. Adjust for large models.')
        row.addWidget(self.distance)
        layout.addLayout(row)
        self.hint = QLabel('Load a map directly into private Test progress.'); self.hint.setWordWrap(True)
        layout.addWidget(self.hint)

    def set_levels(self, levels, available, error=''):
        current = self.level.currentData(); self.level.clear()
        for entry in levels:
            self.level.addItem(entry['label'], entry['id'])
            self.level.setItemData(self.level.count()-1, entry['folder'] + '/' + entry['stream'], Qt.ToolTipRole)
        if current is not None: self.level.setCurrentIndex(max(0, self.level.findData(current)))
        self.available = available and bool(levels)
        self.level.setToolTip(error or 'Choose an original standard-mode map entry. A new private preview starts on each load.')
        self.load.setToolTip(error or 'Load the selected map without editing the installed gameflow or using your saves.')
        self.load.setEnabled(self.available)

    def update_state(self, live, blocked, project):
        ready = bool(live.get('level_ready'))
        self.load.setEnabled(bool(getattr(self, 'available', False) and not blocked))
        self.level.setEnabled(not blocked)
        self.camera.setEnabled(ready and not blocked)
        self.layers.setEnabled(ready and 'viewport' in live and not blocked)
        self.focus.setEnabled(ready and not blocked)
        camera = live.get('camera', {})
        mode = 'paused' if camera.get('photo') else 'free' if camera.get('free') else 'game'
        self.camera.setCurrentIndex(max(0, self.camera.findData(mode)))
        view = live.get('viewport', {})
        self.layers.setCurrentIndex(max(0, self.layers.findData(view.get('mode', 'all'))))
        if ready:
            text = 'Click the viewport · WASD move · arrows look · Q/E rise · Shift/Ctrl change speed · 1/3 zoom'
            if view.get('mode', 'all') != 'all':
                text += f" · {view.get('hidden', 0):,} actors hidden; {view.get('unsupported', 0):,} unsupported layouts kept"
            self.hint.setText(text)
        elif view.get('booting'):
            self.hint.setText('Loading the selected map through the original game menus…')
        else:
            self.hint.setText('Load a map directly into private Test progress.')


class GameLibrary(QWidget):
    searchRequested = Signal()
    selected = Signal(object)
    prepareRequested = Signal(object)
    previewRequested = Signal(object)
    clearRequested = Signal()
    cancelRequested = Signal()
    nativePreviewRequested = Signal(object)
    hovered = Signal(object)

    def __init__(self, parent=None):
        super().__init__(parent); self.offset = 0; self.total = 0; self.current = None
        layout = QVBoxLayout(self); layout.setContentsMargins(8, 6, 8, 6)
        row = QHBoxLayout()
        self.search = QLineEdit(); self.search.setPlaceholderText('Search every installed archive…'); self.search.setClearButtonEnabled(True)
        self.search.setObjectName('gameLibrarySearch'); row.addWidget(self.search, 1)
        self.category = QComboBox(); self.category.addItem('All types', ''); row.addWidget(self.category)
        button = QPushButton('Search'); button.clicked.connect(self.start_search); row.addWidget(button)
        layout.addLayout(row)
        self.items = LibraryAssetList(); self.items.setObjectName('wholeGameAssets'); self.items.setUniformItemSizes(True)
        self.items.setMouseTracking(True);self.items.itemEntered.connect(lambda item:self.hovered.emit(item.data(Qt.UserRole)))
        self.items.currentItemChanged.connect(self.select); layout.addWidget(self.items, 1)
        row = QHBoxLayout()
        self.previous = QPushButton('Previous'); self.previous.clicked.connect(lambda: self.page(-1)); row.addWidget(self.previous)
        self.count = QLabel('Start Test to connect to the native archive index.'); self.count.setWordWrap(True); row.addWidget(self.count, 1)
        self.next = QPushButton('Next'); self.next.clicked.connect(lambda: self.page(1)); row.addWidget(self.next)
        layout.addLayout(row)
        row = QHBoxLayout()
        self.prepare = QPushButton('Prepare source'); self.prepare.setObjectName('prepareAssetSource')
        self.prepare.clicked.connect(lambda: self.prepareRequested.emit(self.current)); row.addWidget(self.prepare)
        self.preview = QPushButton('3D preview'); self.preview.setObjectName('previewLibraryModel')
        self.preview.clicked.connect(lambda: self.previewRequested.emit(self.current)); row.addWidget(self.preview)
        self.native_preview = QPushButton('Native prop preview'); self.native_preview.setObjectName('previewLibraryProp')
        self.native_preview.clicked.connect(lambda:self.nativePreviewRequested.emit(self.current));row.addWidget(self.native_preview)
        self.clear = QPushButton('Clear preview'); self.clear.clicked.connect(self.clearRequested); row.addWidget(self.clear)
        layout.addLayout(row)
        self.status = QLabel('The native loader resolves models and dependencies. Source actors are not activated.')
        self.status.setWordWrap(True); layout.addWidget(self.status)
        row = QHBoxLayout()
        self.progress = QProgressBar(); self.progress.hide(); row.addWidget(self.progress, 1)
        self.cancel = QPushButton('Cancel loading'); self.cancel.hide(); self.cancel.clicked.connect(self.cancelRequested)
        row.addWidget(self.cancel); layout.addLayout(row)
        self.debounce = QTimer(self); self.debounce.setSingleShot(True); self.debounce.setInterval(350)
        self.debounce.timeout.connect(self.start_search)
        self.search.textChanged.connect(lambda: self.debounce.start()); self.search.returnPressed.connect(self.start_search)
        self.category.activated.connect(self.start_search)
        self.connected = self.ready = self.blocked = self.offline_ready = False; self.update_controls()

    def query(self):
        return dict(text=self.search.text()[:160], category=self.category.currentData() or '', offset=self.offset, limit=80)

    def start_search(self, *_):
        self.debounce.stop(); self.offset = 0; self.searchRequested.emit()

    def page(self, direction):
        self.offset = max(0, self.offset + 80 * direction); self.searchRequested.emit()

    def accept(self, result):
        self.total = result['total']; self.items.clear(); self.current = None
        for asset in result['items']:
            item = QListWidgetItem(asset['name'] + '  ·  ' + asset['category']); item.setData(Qt.UserRole, asset)
            item.setToolTip(asset['source']); self.items.addItem(item)
        category = self.category.currentData()
        self.category.clear(); self.category.addItem('All types', '')
        for name in sorted(result.get('categories', [])):
            self.category.addItem(name, name)
        self.category.setCurrentIndex(max(0, self.category.findData(category)))
        self.count.setText((f"{self.offset + 1:,}–{self.offset + len(result['items']):,} of {self.total:,}" if self.total else 'No matching resources')
                           if result.get('ready') else f"Indexing archives: {result.get('files', 0):,} / {result.get('total_files', 0):,}")
        self.index_ready = result.get('ready', False); self.update_controls()

    def select(self, item, old=None):
        self.current = item.data(Qt.UserRole) if item else None
        self.selected.emit(self.current); self.update_controls()

    def update_controls(self):
        usable = self.connected and not self.blocked
        browse=(self.connected or self.offline_ready) and not self.blocked
        self.previous.setEnabled(browse and self.offset > 0)
        self.next.setEnabled(browse and self.offset + 80 < self.total)
        self.prepare.setEnabled(bool(usable and self.ready and self.current and self.current.get('object_count')))
        self.preview.setEnabled(bool(self.current and not self.blocked and (self.offline_ready or self.connected)))
        self.native_preview.setEnabled(bool(usable and self.ready and self.current and self.current.get('object_count')))
        self.clear.setEnabled(usable and self.ready)

    def describe(self, asset):
        if not asset: return
        self.status.setText(asset['category'] + ' · ' + asset.get('native_class', '') + '\n' + asset['source'] +
            ('\nPrepare the original archive, then preview a supported prop.' if asset.get('object_count') else
             '\nInspect this resource in the asset Inspector. It is not a placeable entity.'))

    def source_progress(self, source, pending):
        self.progress.setVisible(pending); self.cancel.setVisible(pending)
        if not pending: return
        loaded, total = source.get('loaded', 0), source.get('files', 0)
        if not isinstance(total, int) or not isinstance(loaded, int): total = loaded = 0
        self.progress.setRange(0, total); self.progress.setValue(loaded)
        self.status.setText(source.get('message') or 'Preparing original archive and dependencies…')


class DefinitionPicker(QDialog):
    pageRequested = Signal(int)

    def __init__(self, result, parent=None):
        super().__init__(parent); self.setWindowTitle('Choose original source definition'); self.resize(700, 440)
        self.selected = None; self.offset = 0
        layout = QVBoxLayout(self)
        label = QLabel('Choose the original variant to prepare. Unsupported factories remain inspection-only.')
        label.setWordWrap(True); layout.addWidget(label)
        self.items = QListWidget(); self.items.setAlternatingRowColors(True); layout.addWidget(self.items, 1)
        self.items.itemDoubleClicked.connect(lambda _: self.accept_selection())
        row = QHBoxLayout()
        self.previous = QPushButton('Previous'); self.previous.clicked.connect(lambda: self.pageRequested.emit(self.offset-100))
        self.next = QPushButton('Next'); self.next.clicked.connect(lambda: self.pageRequested.emit(self.offset+100))
        self.count = QLabel(); row.addWidget(self.previous); row.addWidget(self.count, 1); row.addWidget(self.next)
        layout.addLayout(row)
        self.buttons = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel)
        self.buttons.accepted.connect(self.accept_selection); self.buttons.rejected.connect(self.reject)
        layout.addWidget(self.buttons); self.set_page(result)

    def set_page(self, result, offset=0):
        self.offset=offset; self.items.clear()
        for value in result['items']:
            item=QListWidgetItem(value['name']+' · '+(value['class'] or 'Unsupported factory')+'\n'+value['stream'])
            item.setData(Qt.UserRole,{**value,'_page_offset':offset}); item.setToolTip(value['source_guid']); self.items.addItem(item)
        self.count.setText(f"{offset+1:,}–{offset+len(result['items']):,} of {result['total']:,}")
        self.previous.setEnabled(offset>0); self.next.setEnabled(offset+len(result['items'])<result['total'])
        if self.items.count(): self.items.setCurrentRow(0)

    def accept_selection(self):
        item=self.items.currentItem()
        if item:
            self.selected=item.data(Qt.UserRole); self.accept()
