"""Tests for linux/setup.py that need no network, toolchain or game files.

    python3 -m unittest discover -s linux/tests
"""
import io
import json
import os
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import setup  # noqa: E402


class DesktopQuoting(unittest.TestCase):
    def test_exec_argument_escapes_both_layers(self):
        self.assertEqual(setup.desktop_arg('/home/a b/x'), '"/home/a b/x"')
        self.assertEqual(setup.desktop_arg('/g/$HOME/"q"/100%'), r'"/g/\\$HOME/\\"q\\"/100%%"')
        self.assertEqual(setup.desktop_value('a\\b\nc'), 'a\\\\b\\nc')


class Destination(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.base = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_new_and_empty_folders_are_accepted(self):
        self.assertEqual(setup.validate_destination(self.base / 'new', free_gb=0), (self.base / 'new').resolve())
        (self.base / 'empty' / setup.STATE).mkdir(parents=True)
        setup.validate_destination(self.base / 'empty', free_gb=0)

    def test_foreign_files_are_refused(self):
        (self.base / 'used').mkdir()
        (self.base / 'used' / 'photo.jpg').write_text('x')
        with self.assertRaises(setup.SetupError):
            setup.validate_destination(self.base / 'used', free_gb=0)

    def test_update_needs_an_installation(self):
        with self.assertRaises(setup.SetupError):
            setup.validate_destination(self.base, update=True, free_gb=0)
        (self.base / 'installer').mkdir()
        (self.base / 'installer' / 'infamous.toml').write_text('')
        setup.validate_destination(self.base, update=True, free_gb=0)

    def test_characters_the_build_cannot_take(self):
        for bad in ('a;b', 'a"b', 'a$b', 'a\\b'):
            with self.assertRaises(setup.SetupError):
                setup.validate_destination(self.base / bad, free_gb=0)

    def test_free_space(self):
        with self.assertRaises(setup.SetupError):
            setup.validate_destination(self.base / 'big', free_gb=10 ** 9)


class Archives(unittest.TestCase):
    def make_tar(self, members):
        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode='w:gz') as tar:
            for name, data, kind in members:
                info = tarfile.TarInfo(name)
                if kind == 'file':
                    info.size = len(data)
                    info.mode = 0o755 if name.endswith('.sh') else 0o644
                    tar.addfile(info, io.BytesIO(data))
                elif kind == 'link':
                    info.type = tarfile.SYMTYPE
                    info.linkname = data
                    tar.addfile(info)
        f = tempfile.NamedTemporaryFile(suffix='.tar.gz', delete=False)
        f.write(buf.getvalue())
        f.close()
        return Path(f.name)

    def test_strips_the_top_folder_and_refuses_escapes(self):
        archive = self.make_tar([
            ('repo-linux/installer/infamous.toml', b'[game]', 'file'),
            ('repo-linux/run.sh', b'#!/bin/sh', 'file'),
            ('repo-linux/../../evil', b'x', 'file'),
            ('repo-linux/link', '/etc/passwd', 'link'),
        ])
        with tempfile.TemporaryDirectory() as d:
            setup.safe_extract_tar(archive, d)
            self.assertTrue((Path(d) / 'installer/infamous.toml').is_file())
            self.assertTrue(os.access(str(Path(d) / 'run.sh'), os.X_OK))
            self.assertFalse((Path(d) / 'link').exists() or (Path(d) / 'link').is_symlink())
            self.assertFalse((Path(d).parent / 'evil').exists())
        archive.unlink()

    def test_llvm_keeps_only_local_tool_links(self):
        archive = self.make_tar([
            ('LLVM/bin/clang-19', b'elf', 'file'),
            ('LLVM/bin/clang-cl', 'clang-19', 'link'),
            ('LLVM/bin/escape', '../../x', 'link'),
        ])
        with tempfile.TemporaryDirectory() as d:
            setup.safe_extract_tar(archive, d, keep=lambda rel, m: True)
            self.assertEqual(os.readlink(str(Path(d) / 'bin/clang-cl')), 'clang-19')
            self.assertFalse((Path(d) / 'bin/escape').is_symlink())
        archive.unlink()


class CrossFiles(unittest.TestCase):
    def test_toolchain_file_and_case_overlay(self):
        with tempfile.TemporaryDirectory() as d:
            tc = Path(d)
            inc = tc / 'ws' / 'Windows Kits' / '10' / 'Include' / 'um'
            inc.mkdir(parents=True)
            (inc / 'windef.h').write_text('')
            (inc / 'WinDef.h').symlink_to('windef.h')
            path = setup.write_cross_files(tc, tc / 'ws')
            text = path.read_text()
            for needle in ('CMAKE_SYSTEM_NAME Windows', 'llvm/bin/clang-cl"', 'llvm/bin/lld-link"',
                           'CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded', '/winsysroot ', '/vfsoverlay:',
                           '/clang:-ivfsoverlay'):
                self.assertIn(needle, text)
            overlay = json.loads((tc / 'winsysroot.vfs.json').read_text())
            self.assertEqual(overlay['case-sensitive'], 'false')
            self.assertEqual(overlay['roots'][0]['name'], str(tc / 'ws'))
            flat = json.dumps(overlay)
            self.assertIn('windef.h', flat)
            self.assertNotIn('WinDef.h', flat)   # xwin's case links are left to the overlay


class GameFolder(unittest.TestCase):
    def test_accepts_the_dump_or_a_folder_inside_it(self):
        with tempfile.TemporaryDirectory() as d:
            dump = Path(d) / 'dump'
            (dump / 'PS3_GAME' / 'USRDIR').mkdir(parents=True)
            (dump / 'PS3_GAME' / 'PARAM.SFO').write_bytes(b'\0PSF')
            for pick in (dump, dump / 'PS3_GAME', dump / 'PS3_GAME' / 'USRDIR'):
                e = setup.Engine(Path(d) / 'dest', game=pick)
                self.assertEqual(e.game_folder(), dump.resolve())
            with self.assertRaises(setup.SetupError):
                setup.Engine(Path(d) / 'dest', game=Path(d)).game_folder()


class System(unittest.TestCase):
    def test_probes_do_not_fail(self):
        self.assertIsInstance(setup.cpu_has_v3(), bool)
        self.assertIsInstance(setup.ram_gb(), int)
        self.assertIsInstance(setup.mount_fstype('/'), str)
        self.assertIsInstance(setup.os_release(), dict)

    def test_private_alias_is_stable(self):
        with tempfile.TemporaryDirectory() as d:
            a = setup.private_alias(Path(d))
            self.assertEqual(setup.private_alias(Path(d)), a)
            self.assertEqual(os.readlink(str(a)), d)
            self.assertNotIn(' ', str(a))
            a.unlink()

    def test_explicit_rpcs3_must_exist(self):
        with self.assertRaises(setup.SetupError):
            setup.find_rpcs3('/nonexistent/rpcs3.AppImage')


if __name__ == '__main__':
    unittest.main()
