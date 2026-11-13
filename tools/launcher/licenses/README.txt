The Linux launcher includes Python, PySide6, Qt and the PyInstaller bootloader.
The versions used for this build are in versions.txt. Their upstream licenses
apply independently of the project's own code.

Python: Python Software Foundation License; see Python.txt.
Source and third-party notices: https://www.python.org/downloads/source/

PySide6, shiboken6 and Qt: GNU LGPL version 3; see LGPL-3.0-only.txt and
GPL-3.0-only.txt. Copyright The Qt Company Ltd. and other contributors.
Qt for Python 6.8.3 source:
https://download.qt.io/official_releases/QtForPython/pyside6/PySide6-6.8.3-src/
Qt 6.8.3 source:
https://download.qt.io/archive/qt/6.8/6.8.3/single/
Qt third-party notices: https://doc.qt.io/qt-6.8/licenses-used-in-qt.html
Qt WebEngine also carries Chromium's third-party notices in its bundled
resources, accessible at chrome://credits in a Qt WebEngine view.

PyInstaller: GNU GPL version 2 or later with the bootloader exception;
see PyInstaller.txt. Source: https://github.com/pyinstaller/pyinstaller

To modify or replace the bundled Qt libraries, install the versions in
tools/launcher/requirements-linux.txt, apply your changes and rebuild with
tools/launcher/build_linux.py, or run launcher/launcher.py with your own
Python and PySide6 installation. The launcher source is included beside
the executable and available at:
https://github.com/YesterMester/TheSimpsonsGameRecomp
