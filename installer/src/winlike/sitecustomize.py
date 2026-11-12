"""Makes the port's Python tools write what they write on Windows, where installer/generated.lock
was made. infamous-installer puts this folder on PYTHONPATH for the pipeline steps on Linux/macOS
(pipeline.rs); it is never used on Windows.

  * Text files opened for writing without newline= get CRLF line endings (Windows' os.linesep).
    newline='' / newline='\\n' are left alone, exactly as on Windows.
  * Directory listings come back in NTFS order (case-insensitive), not the file system's hash
    order, so tools that walk a folder without sorting it produce the same output.
"""
import builtins
import io
import os

_open = io.open


def _open_winlike(file, mode='r', buffering=-1, encoding=None, errors=None, newline=None,
                  closefd=True, opener=None):
    if newline is None and 'b' not in mode and any(c in mode for c in 'wax+'):
        newline = '\r\n'
    return _open(file, mode, buffering, encoding, errors, newline, closefd, opener)


builtins.open = io.open = _open_winlike


def _ntfs_key(name):
    return name.upper()


_listdir = os.listdir
_scandir = os.scandir


def _listdir_winlike(path='.'):
    names = _listdir(path)
    names.sort(key=_ntfs_key)
    return names


class _Entries:
    """os.scandir's iterator protocol (iterate, with-block, close) over a sorted list."""

    def __init__(self, entries):
        self._it = iter(entries)

    def __iter__(self):
        return self

    def __next__(self):
        return next(self._it)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def close(self):
        self._it = iter(())


def _scandir_winlike(path='.'):
    with _scandir(path) as it:
        entries = list(it)
    entries.sort(key=lambda e: _ntfs_key(e.name))
    return _Entries(entries)


os.listdir = _listdir_winlike
os.scandir = _scandir_winlike
