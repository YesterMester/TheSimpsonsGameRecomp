"""Extract an Xbox 360 XDVDFS disc image (ISO or XISO) without extract-xiso.

The launcher installs with the bundled extract-xiso program. When that program
cannot start at all, for example because Windows Smart App Control blocks
unsigned executables (WinError 4551), this module extracts the image itself.
It reads the same partition offsets and directory format as extract-xiso and
the Android app's reader (android/.../XboxIsoSource.java). Standard library only.
"""
from __future__ import annotations

import os
from pathlib import Path
import struct

SECTOR = 2048
MEDIA = b"MICROSOFT*XBOX*MEDIA"
# Where the XDVDFS partition starts: plain XISO, then the XGD2/XGD3 layouts.
PARTITIONS = (0, 0x0FD90000, 0x02080000, 0x18300000)
MAX_DIRECTORY_TABLE = 16 * 1024 * 1024
MAX_ENTRIES = 250000
INVALID_NAME_CHARACTERS = set('<>:"/\\|?*')


class XisoError(ValueError):
    pass


class Image:
    def __init__(self, path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        try:
            self.size = os.fstat(self.file.fileno()).st_size
            self.partition, self.root = self._find_root()
        except BaseException:
            self.file.close()
            raise

    def close(self):
        self.file.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _read(self, position, length):
        self.file.seek(position)
        data = self.file.read(length)
        if len(data) != length:
            raise XisoError("The ISO could not be read completely.")
        return data

    def _find_root(self):
        for partition in PARTITIONS:
            if partition + 0x10800 > self.size:
                continue
            header = self._read(partition + 0x10000, SECTOR)
            if header[:20] != MEDIA:
                continue
            if header[2028:2048] != MEDIA:
                raise XisoError("The ISO's disc header is damaged or incomplete.")
            sector, length = struct.unpack_from("<II", header, 20)
            if length == 0:
                raise XisoError("The disc image contains no game files.")
            root = (partition + sector * SECTOR, length)
            self._check_range(partition, *root)
            return partition, root
        raise XisoError("This file is not an Xbox 360 ISO/XISO disc image.")

    def _check_range(self, partition, position, length):
        if position < partition or length < 0 or position > self.size or length > self.size - position:
            raise XisoError("The ISO is truncated or contains a file outside the disc image.")

    def entries(self, position, length):
        """The entries of one directory table: (name, position, length, is_directory)."""
        if length == 0:
            return []
        if length > MAX_DIRECTORY_TABLE:
            raise XisoError("The ISO contains an invalid directory table.")
        table = self._read(position, length)
        found, pending, visited = [], [0], set()
        while pending:
            offset = pending.pop()
            if offset in visited:
                raise XisoError("The ISO contains a directory loop.")
            visited.add(offset)
            if offset < 0 or offset > len(table) - 14:
                raise XisoError("The ISO contains a damaged directory entry.")
            left, right, sector, size, attributes, name_length = struct.unpack_from("<HHIIBB", table, offset)
            if left == 0xFFFF:
                if offset == 0:
                    continue  # An empty directory's table is filled with 0xff.
                next_sector = (offset // SECTOR + 1) * SECTOR
                if next_sector < len(table):
                    pending.append(next_sector)
                    continue
                raise XisoError("The ISO contains an invalid directory link.")
            if name_length == 0 or name_length > len(table) - offset - 14:
                raise XisoError("The ISO contains a damaged filename.")
            name = table[offset + 14:offset + 14 + name_length].decode("latin-1")
            if (name in (".", "..") or any(c in INVALID_NAME_CHARACTERS or ord(c) < 32 for c in name)
                    or name.endswith((" ", "."))):
                raise XisoError(f"The ISO contains an unsupported file name: {name!r}")
            start = self.partition + sector * SECTOR
            self._check_range(self.partition, start, size)
            found.append((name, start, size, bool(attributes & 0x10)))
            if len(found) > MAX_ENTRIES:
                raise XisoError("The ISO contains too many directory entries.")
            if right:
                pending.append(right * 4)
            if left:
                pending.append(left * 4)
        return found

    def walk(self):
        """Every entry below the root, parents first: (relative parts, position, length, is_directory)."""
        stack, seen, count = [((), self.root)], set(), 0
        while stack:
            parts, (position, length) = stack.pop()
            if (position, length) in seen and length:
                raise XisoError("The ISO contains a directory loop.")
            seen.add((position, length))
            names = set()
            for name, start, size, directory in sorted(self.entries(position, length)):
                if name.lower() in names:
                    raise XisoError(f"The ISO contains the same name twice: {name!r}")
                names.add(name.lower())
                count += 1
                if count > MAX_ENTRIES:
                    raise XisoError("The ISO contains too many directory entries.")
                child = parts + (name,)
                yield child, start, size, directory
                if directory:
                    stack.append((child, (start, size)))

    def chunks(self, position, length, size=1 << 20):
        remaining = length
        while remaining:
            data = self._read(position, min(size, remaining))
            position += len(data)
            remaining -= len(data)
            yield data


def extract(iso, target, log=None, cancelled=None):
    """Extract every file and folder of the image into the existing, empty target folder.

    Returns (files, bytes). log receives short progress lines; cancelled() may
    return True to stop (raises XisoError)."""
    log = log or (lambda message: None)
    target = Path(target)
    files = written = 0
    next_report = 256 << 20
    with Image(iso) as image:
        log(f"Reading the disc image's file system (partition at {image.partition:#x})")
        for parts, start, size, directory in image.walk():
            if cancelled and cancelled():
                raise XisoError("Extraction was cancelled.")
            path = target.joinpath(*parts)
            if directory:
                path.mkdir(parents=True, exist_ok=False)
                continue
            path.parent.mkdir(parents=True, exist_ok=True)
            with open(path, "xb") as output:
                for data in image.chunks(start, size):
                    output.write(data)
                    written += len(data)
                    if written >= next_report:
                        log(f"... {written >> 20} MiB extracted ({files} files)")
                        next_report += 256 << 20
            files += 1
    log(f"Extracted {files} files, {written >> 20} MiB")
    return files, written
