"""Small cross-platform process helpers shared by the two desktop apps."""
from contextlib import contextmanager
import os
from pathlib import Path


@contextmanager
def game_launch_lock(directory):
    # Lock preparation through Popen, then let exact process detection prevent
    # another launch. The file contains no settings or persistent session data.
    path = Path(directory) / ".game-launch.lock"
    with path.open("a+b") as lock:
        if os.name == "nt":
            import msvcrt
            lock.seek(0)
            if not lock.read(1):
                lock.write(b"\0"); lock.flush()
            lock.seek(0)
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise BlockingIOError("Another app is preparing the game") from error
            try:
                yield
            finally:
                lock.seek(0); msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                yield
            finally:
                fcntl.flock(lock, fcntl.LOCK_UN)


def windows_game_pids(executable):
    import ctypes
    from ctypes import wintypes as w
    k = ctypes.WinDLL("kernel32", use_last_error=True)
    class Entry(ctypes.Structure):
        _fields_ = [("size", w.DWORD), ("usage", w.DWORD), ("pid", w.DWORD),
                    ("heap", ctypes.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                    ("parent", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD),
                    ("name", w.WCHAR * 260)]
    k.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    k.CreateToolhelp32Snapshot.restype = w.HANDLE
    k.Process32FirstW.argtypes = k.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(Entry)]
    k.Process32FirstW.restype = k.Process32NextW.restype = w.BOOL
    k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]; k.OpenProcess.restype = w.HANDLE
    k.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
    k.QueryFullProcessImageNameW.restype = w.BOOL
    k.CloseHandle.argtypes = [w.HANDLE]; k.CloseHandle.restype = w.BOOL
    snapshot = k.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError(ctypes.get_last_error(), "Could not inspect running game processes")
    found, target = [], os.path.normcase(str(Path(executable).resolve()))
    try:
        entry = Entry(); entry.size = ctypes.sizeof(entry)
        more = k.Process32FirstW(snapshot, ctypes.byref(entry))
        while more:
            if entry.name.casefold() == Path(executable).name.casefold():
                process = k.OpenProcess(0x1000, False, entry.pid)
                if process:
                    try:
                        text = ctypes.create_unicode_buffer(32768); length = w.DWORD(len(text))
                        if k.QueryFullProcessImageNameW(process, 0, text, ctypes.byref(length)) and \
                                os.path.normcase(str(Path(text.value).resolve())) == target:
                            found.append(entry.pid)
                    finally:
                        k.CloseHandle(process)
            more = k.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        k.CloseHandle(snapshot)
    return found


def linux_game_pids(executable):
    # Native runtimes can rename the main thread. Match the executable's
    # identity, rather than its comm name or unrelated command-line text.
    try:
        target = Path(executable).stat()
    except OSError:
        return []
    found = []
    with os.scandir('/proc') as processes:
        for process in processes:
            if not process.name.isdigit():
                continue
            try:
                image = os.stat('/proc/' + process.name + '/exe')
                if (image.st_dev, image.st_ino) == (target.st_dev, target.st_ino):
                    found.append(int(process.name))
            except OSError:
                continue
    return found
