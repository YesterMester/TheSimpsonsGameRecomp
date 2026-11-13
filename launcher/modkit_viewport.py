"""Real owned-game window hosting; no frame capture or renderer replacement."""
import ctypes
import ctypes.util
import json
import os
import sys

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QGuiApplication, QWindow
from PySide6.QtWidgets import QHBoxLayout, QLabel, QProgressBar, QPushButton, QStackedLayout, QVBoxLayout, QWidget

ASSET_MIME = "application/x-simpsons-modkit-asset"
LIBRARY_MIME = 'application/x-simpsons-modkit-library-resource'


def game_window(pid):
    if sys.platform == "win32":
        from ctypes import wintypes as w
        user = ctypes.WinDLL("user32", use_last_error=True)
        user.GetWindowThreadProcessId.argtypes = [w.HWND, ctypes.POINTER(w.DWORD)]
        user.GetWindowThreadProcessId.restype = w.DWORD
        user.IsWindowVisible.argtypes = [w.HWND]; user.IsWindowVisible.restype = w.BOOL
        user.GetWindow.argtypes = [w.HWND, w.UINT]; user.GetWindow.restype = w.HWND
        result = []
        callback_type = ctypes.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
        @callback_type
        def visit(window, _):
            owner = w.DWORD(); user.GetWindowThreadProcessId(window, ctypes.byref(owner))
            if owner.value == pid and user.IsWindowVisible(window) and not user.GetWindow(window, 4):
                result.append(int(window))
            return True
        user.EnumWindows.argtypes = [callback_type, w.LPARAM]; user.EnumWindows.restype = w.BOOL
        user.EnumWindows(visit, 0)
        return result[0] if result else None
    if QGuiApplication.platformName() != "xcb":
        return None
    # Checked XCB replies keep a window closing between discovery and query
    # from invoking Xlib's process-wide fatal BadWindow handler.
    x = ctypes.CDLL(ctypes.util.find_library("xcb") or "libxcb.so.1")
    libc = ctypes.CDLL(None); pointer, uint = ctypes.c_void_p, ctypes.c_uint32
    libc.free.argtypes = [pointer]
    class Cookie(ctypes.Structure):
        _fields_ = [("sequence", ctypes.c_uint)]
    class Iterator(ctypes.Structure):
        _fields_ = [("data", pointer), ("remaining", ctypes.c_int), ("index", ctypes.c_int)]
    x.xcb_connect.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int)]; x.xcb_connect.restype = pointer
    x.xcb_connection_has_error.argtypes = [pointer]; x.xcb_connection_has_error.restype = ctypes.c_int
    x.xcb_get_setup.argtypes = [pointer]; x.xcb_get_setup.restype = pointer
    x.xcb_setup_roots_iterator.argtypes = [pointer]; x.xcb_setup_roots_iterator.restype = Iterator
    x.xcb_screen_next.argtypes = [ctypes.POINTER(Iterator)]
    x.xcb_intern_atom.argtypes = [pointer, ctypes.c_uint8, ctypes.c_uint16, ctypes.c_char_p]; x.xcb_intern_atom.restype = Cookie
    x.xcb_intern_atom_reply.argtypes = [pointer, Cookie, ctypes.POINTER(pointer)]; x.xcb_intern_atom_reply.restype = pointer
    x.xcb_get_property.argtypes = [pointer, ctypes.c_uint8, uint, uint, uint, uint, uint]; x.xcb_get_property.restype = Cookie
    x.xcb_get_property_reply.argtypes = [pointer, Cookie, ctypes.POINTER(pointer)]; x.xcb_get_property_reply.restype = pointer
    x.xcb_get_property_value.argtypes = [pointer]; x.xcb_get_property_value.restype = pointer
    x.xcb_get_property_value_length.argtypes = [pointer]; x.xcb_get_property_value_length.restype = ctypes.c_int
    x.xcb_disconnect.argtypes = [pointer]
    screen = ctypes.c_int(); connection = x.xcb_connect(None, ctypes.byref(screen))
    if not connection:
        return None
    def reply(function, cookie):
        error = pointer(); value = function(connection, cookie, ctypes.byref(error))
        if error:
            libc.free(error)
            if value: libc.free(value)
            return None
        return value
    def values(window, name, maximum):
        atom_reply = reply(x.xcb_intern_atom_reply, x.xcb_intern_atom(connection, 1, len(name), name))
        if not atom_reply: return []
        try: atom = ctypes.cast(atom_reply + 8, ctypes.POINTER(uint))[0]
        finally: libc.free(atom_reply)
        if not atom: return []
        value = reply(x.xcb_get_property_reply, x.xcb_get_property(connection, 0, window, atom, 0, 0, maximum))
        if not value: return []
        try:
            if ctypes.cast(value, ctypes.POINTER(ctypes.c_uint8))[1] != 32: return []
            count = x.xcb_get_property_value_length(value) // 4
            return list(ctypes.cast(x.xcb_get_property_value(value), ctypes.POINTER(uint))[:min(count, maximum)])
        finally: libc.free(value)
    try:
        if x.xcb_connection_has_error(connection): return None
        iterator = x.xcb_setup_roots_iterator(x.xcb_get_setup(connection))
        for _ in range(screen.value): x.xcb_screen_next(ctypes.byref(iterator))
        if not iterator.data or not iterator.remaining: return None
        root = ctypes.cast(iterator.data, ctypes.POINTER(uint))[0]
        for window in values(root, b"_NET_CLIENT_LIST", 4096):
            if values(window, b"_NET_WM_PID", 1) == [pid]: return window
    finally:
        x.xcb_disconnect(connection)
    return None



class DropTarget(QLabel):
    assetDropped = Signal(str)
    libraryAssetDropped = Signal(object)
    def __init__(self, parent=None):
        super().__init__("Drop a prop to place it beside your character", parent)
        self.setAlignment(Qt.AlignCenter); self.setWordWrap(True); self.setAcceptDrops(True)
        self.setAttribute(Qt.WA_NativeWindow)
        self.setStyleSheet("background:rgba(21,32,44,225);color:#f7cf67;border:2px dashed #f7cf67;padding:24px;font-size:15px")
    def dragEnterEvent(self, event):
        if event.mimeData().hasFormat(ASSET_MIME) or event.mimeData().hasFormat(LIBRARY_MIME):
            event.acceptProposedAction()
    def dropEvent(self, event):
        if event.mimeData().hasFormat(LIBRARY_MIME):
            try:
                data=bytes(event.mimeData().data(LIBRARY_MIME))
                if len(data)>64*1024:return
                asset=json.loads(data)
                if not isinstance(asset,dict) or not isinstance(asset.get('id'),str) or not isinstance(asset.get('locations'),list):return
                event.acceptProposedAction();self.libraryAssetDropped.emit(asset)
            except (ValueError,UnicodeError):pass
            return
        data = bytes(event.mimeData().data(ASSET_MIME))
        try:
            if len(data) > 2048:
                return
            asset = json.loads(data)
            if not isinstance(asset, str) or not 1 <= len(asset) <= 256:
                return
            event.acceptProposedAction(); self.assetDropped.emit(asset)
        except (ValueError, UnicodeError):
            pass


class GameViewport(QWidget):
    assetDropped = Signal(str)
    libraryAssetDropped = Signal(object)
    testRequested = Signal()
    helpRequested = Signal()
    def __init__(self, parent=None):
        super().__init__(parent)
        self.foreign = self.container = self.native_id = self.pid = None
        self.platform_limited = False
        self.loading=False
        self.setMinimumSize(280, 180)
        self.stack = QStackedLayout(self); self.stack.setStackingMode(QStackedLayout.StackAll)
        self.welcome = QWidget(); self.welcome.setObjectName('viewportWelcome')
        self.welcome.setAttribute(Qt.WA_NativeWindow)
        layout = QVBoxLayout(self.welcome); layout.setContentsMargins(32, 20, 32, 20); layout.setSpacing(14)
        layout.addStretch()
        eyebrow = QLabel('SCENE WORKSPACE'); eyebrow.setObjectName('workspaceEyebrow'); layout.addWidget(eyebrow)
        self.heading = QLabel('Create something for Springfield'); self.heading.setObjectName('workspaceHeading')
        self.heading.setWordWrap(True); layout.addWidget(self.heading)
        self.message = QLabel('Open a project or create one with Ctrl+N.\nThe live viewport uses the game’s own renderer and a separate Test save.')
        self.message.setWordWrap(True); self.message.setAlignment(Qt.AlignCenter)
        self.message.setAlignment(Qt.AlignLeft); self.message.setObjectName('workspaceDescription'); layout.addWidget(self.message)
        actions = QHBoxLayout(); self.start = QPushButton('▶  Start Test · F5'); self.start.setObjectName('primaryAction')
        self.start.setEnabled(False); self.start.clicked.connect(self.testRequested); actions.addWidget(self.start)
        help_button = QPushButton('Getting started · F1'); help_button.clicked.connect(self.helpRequested)
        actions.addWidget(help_button); actions.addStretch(); layout.addLayout(actions)
        tips = QLabel('01  Create or open a project\n02  Load an episode in Test and place props from Assets\n03  Save live drafts, stop Test, then apply drafts and export')
        tips.setObjectName('workspaceSteps'); tips.setWordWrap(True); layout.addWidget(tips)
        self.loading_bar=QProgressBar();self.loading_bar.setRange(0,0);self.loading_bar.setMaximumHeight(8)
        self.loading_bar.setTextVisible(False);self.loading_bar.hide();layout.addWidget(self.loading_bar)
        layout.addStretch(); self.stack.addWidget(self.welcome)
        self.drop_target = DropTarget(self); self.drop_target.hide()
        self.drop_target.assetDropped.connect(self.assetDropped)
        self.drop_target.libraryAssetDropped.connect(self.libraryAssetDropped)
    def attach(self, pid):
        if self.pid == pid and self.container:
            return True
        if sys.platform != "win32" and QGuiApplication.platformName() != "xcb":
            self.platform_limited = True
            self.message.setText("Live embedding is available on Windows and Linux X11.\n\nYour Test game runs in its own window on this display server.\nAsset and property tools still use the native creator connection.")
            return False
        identity = game_window(pid)
        if not identity:
            return False
        self.detach()
        self.foreign = QWindow.fromWinId(identity)
        if self.foreign is None:
            return False
        self.pid, self.native_id = pid, identity
        self.container = QWidget.createWindowContainer(self.foreign, self)
        self.container.setFocusPolicy(Qt.StrongFocus)
        self.stack.addWidget(self.container); self.stack.setCurrentWidget(self.container)
        self.container.show(); self.drop_target.raise_()
        return True
    def detach(self):
        if self.foreign is not None:
            self.foreign.setParent(None)
        if self.container is not None:
            self.stack.removeWidget(self.container); self.container.deleteLater()
        self.foreign = self.container = self.native_id = self.pid = None
        self.welcome.show(); self.stack.setCurrentWidget(self.welcome)
        self.set_loading(False)
    def set_context(self, project, can_test):
        if self.loading:return
        self.heading.setText(project['name'] if project else 'Create something for Springfield')
        self.heading.setTextFormat(Qt.PlainText)
        self.start.setEnabled(can_test)
        if not self.platform_limited:
            self.message.setText('Choose a map above and Load level to edit with a paused camera.\nTest / F5 opens Play with separate progress. Assets also have offline 3D previews.' if project else
                'Open a project or create one with Ctrl+N.\nThe live viewport uses the game’s own renderer and a separate Test save.')
    def set_loading(self,active):
        if self.loading==active:return
        self.loading=active;self.loading_bar.setVisible(active)
        self.start.setVisible(not active)
        if active:
            self.heading.setText('Opening map workspace')
            self.message.setText('Loading the original map and native renderer…\nThe editor will pause simulation when the map is ready. Your playthrough stays separate.')
            self.welcome.show();self.stack.setCurrentWidget(self.welcome);self.welcome.raise_()
        elif self.container:
            self.welcome.hide();self.stack.setCurrentWidget(self.container);self.container.raise_()
    def show_drop(self, visible):
        self.drop_target.setGeometry(self.rect()); self.drop_target.setVisible(visible)
        if visible:
            self.drop_target.raise_()
    def resizeEvent(self, event):
        self.drop_target.setGeometry(self.rect()); super().resizeEvent(event)
