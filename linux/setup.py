#!/usr/bin/env python3
"""inFamous Linux setup: builds the Windows port from the player's own game files with a private,
portable toolchain (clang-cl + lld-link + Microsoft's headers/libraries) and runs it with Proton.

Nothing is installed system-wide and nothing needs sudo. Everything (build tools, Python, Rust,
the Proton prefix) lives in <install folder>/.linux. Standard library only, Python 3.8+.
"""
import argparse
import fcntl
import hashlib
import json
import os
import platform
import queue
import re
import selectors
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from urllib.parse import quote

HERE = Path(__file__).resolve().parent
TITLE = 'inFamous'
DEFAULT_REPO = 'whompay/inFAMOUSreborn'       # set by package.sh --repo
DEFAULT_BRANCH = 'main'                        # set by package.sh --branch
LINK_NAME = 'Infamous (Europe) (Fr,De,Es,It,Nl)'   # [game] link_name in installer/infamous.toml
STATE = '.linux'
FIRST_INSTALL_GB = 40
UPDATE_GB = 8

MAMBA_URL = 'https://github.com/mamba-org/micromamba-releases/releases/download/2.9.0-0/micromamba-linux-64'
MAMBA_SHA256 = '366cd9cd8be14df1ab8ed50352a82111082a36686b2d389fdb79a92c3fafb3e3'
# Rust builds the installer (gcc is only its linker); Python is the base of the installer's venv
# and runs umu; curl/unzip are what `infamous-installer update` uses; 7-Zip opens the SDK cab.
ENV_VERSION = 'py312-rust-gcc14-curl-unzip-7z-v1'
PACKAGES = ['python=3.12', 'rust>=1.80', 'gcc_linux-64=14', 'curl', 'unzip', 'p7zip',
            'ca-certificates']
# LLVM 19's lld-link and llvm-mt need the libxml2.so.2 ABI, which newer distributions dropped.
LLVM_RUNTIME = ['libxml2=2.13.*']

UMU_URL = 'https://github.com/Open-Wine-Components/umu-launcher/releases/download/1.4.4/umu-launcher-1.4.4-zipapp.tar'
UMU_SHA256 = 'eb590691841f7fad3fc3ad8fd5db4ccb87849fe7948e62b28ece7a4ee48cc851'

LLVM_VERSION = '19.1.5'   # must read installer/pgo/infamous.profdata (written by LLVM 19)
TOOLS = [
    # name, url, sha256, size, folder
    ('LLVM (clang-cl, lld-link) ' + LLVM_VERSION,
     'https://github.com/llvm/llvm-project/releases/download/llvmorg-19.1.5/LLVM-19.1.5-Linux-X64.tar.xz',
     '13e9975b026d431c945927960e5f8c0a47a155a2f600f57e85f4d1482620c65f', 1652713376, 'llvm'),
    ('CMake 3.31.6',
     'https://github.com/Kitware/CMake/releases/download/v3.31.6/cmake-3.31.6-linux-x86_64.tar.gz',
     '5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf', 55010959, 'cmake'),
    ('Ninja 1.13.2',
     'https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-linux.zip',
     '5749cbc4e668273514150a80e387a957f933c6ed3f5f11e03fb30955e2bbead6', 134040, 'ninja'),
    ('xwin 0.10.0',
     'https://github.com/Jake-Shadle/xwin/releases/download/0.10.0/xwin-0.10.0-x86_64-unknown-linux-musl.tar.gz',
     'd870eb4b2f390878af6da1ccd3cf321d22fcb72720984853b4be732ae597fc88', 3416863, 'xwin'),
]
XWIN_CRT = '14.44.17.14'
XWIN_SDK = '10.0.26100'
MS_LICENSE_URL = 'https://visualstudio.microsoft.com/license-terms/'
# The game compiles its shaders at run time with D3DCompile. Wine's own d3dcompiler_47 is a partial
# reimplementation; Microsoft's (from the Windows SDK, the file winetricks installs) goes next to the game.
D3DC_CAB_URL = ('https://download.microsoft.com/download/B/0/C/B0C80BA3-8AD6-4958-810B-6882485230B5/'
                'standalonesdk/Installers/61d57a7a82309cd161a854a6f4619e52.cab')
D3DC_CAB_SHA256 = 'f736e161547095bb8d98c636b85fdfeb4070fefeee3c3745db3ce88f6eb1d9de'
D3DC_MEMBER = 'fil3585cb2ea5db13cc0838f8d06b5c9679'
D3DC_SHA256 = 'e994847e01a6f1e4cbdc5a864616ac262f67ee4f14db194984661a8d927ab7f4'
LLVM_KEEP = {'clang', 'clang-cl', 'clang++', 'lld', 'lld-link', 'ld.lld', 'llvm-rc', 'llvm-ar',
             'llvm-lib', 'llvm-ranlib', 'llvm-mt', 'llvm-profdata', 'llvm-cvtres', 'llvm-nm',
             'llvm-objdump', 'llvm-readobj', 'llvm-symbolizer'}
FONT_DIRS = ('/usr/share/fonts', '/usr/local/share/fonts', '/run/host/fonts',
             str(Path.home() / '.local/share/fonts'), str(Path.home() / '.fonts'))


class SetupError(Exception):
    pass


class Cancelled(SetupError):
    pass


# ------------------------------------------------------------------------------------------------
# The system
# ------------------------------------------------------------------------------------------------

def os_release():
    for p in ('/etc/os-release', '/usr/lib/os-release'):
        try:
            text = Path(p).read_text()
        except OSError:
            continue
        return {k: v.strip().strip('"\'') for k, v in
                (line.split('=', 1) for line in text.splitlines() if '=' in line and not line.startswith('#'))}
    return {}


def is_steamos():
    return os_release().get('ID') == 'steamos'


def is_wsl():
    try:
        return 'microsoft' in Path('/proc/sys/kernel/osrelease').read_text().lower()
    except OSError:
        return False


def mount_fstype(path):
    """File system type of the mount holding `path` (from /proc/self/mountinfo)."""
    path = Path(path)
    while not path.exists():
        path = path.parent
    real = os.path.realpath(path)
    best, kind = '', ''
    try:
        lines = Path('/proc/self/mountinfo').read_text().splitlines()
    except OSError:
        return ''
    for line in lines:
        left, _, right = line.partition(' - ')
        fields = left.split()
        if len(fields) < 5 or not right:
            continue
        point = fields[4].replace('\\040', ' ').replace('\\011', '\t').replace('\\134', '\\')
        if (real == point or real.startswith(point.rstrip('/') + '/')) and len(point) >= len(best):
            best, kind = point, right.split()[0]
    return kind


def cpu_has_v3():
    """The feature set of the default (AVX2) build; else the installer builds the compatible one."""
    try:
        flags = next((line.split(':', 1)[1].split() for line in Path('/proc/cpuinfo').read_text().splitlines()
                      if line.startswith('flags')), [])
    except OSError:
        return True
    return {'ssse3', 'sse4_1', 'avx', 'avx2', 'fma', 'bmi1', 'bmi2', 'abm', 'movbe', 'popcnt'} <= set(flags)


def ram_gb():
    try:
        for line in Path('/proc/meminfo').read_text().splitlines():
            if line.startswith('MemTotal:'):
                return int(line.split()[1]) // (1 << 20)
    except (OSError, ValueError):
        pass
    return 0


def has_nvidia():
    return Path('/proc/driver/nvidia/version').exists()


def preflight(warn):
    """Refuse what cannot work and warn about what might not. Returns the warnings."""
    if platform.system() != 'Linux':
        raise SetupError('This setup is for Linux. On Windows, use the inFamous Setup.exe of the original port.')
    if platform.machine() not in ('x86_64', 'amd64'):
        raise SetupError('inFamous needs an x86-64 PC (the recompiled code is x86-64). ARM Linux is not supported.')
    libc, version = platform.libc_ver()
    if libc != 'glibc':
        raise SetupError('This setup needs a glibc-based Linux. On Alpine, Void (musl) or Chimera, run it inside '
                         'a glibc container, for example: distrobox create -i archlinux && distrobox enter archlinux')
    try:
        if tuple(int(x) for x in version.split('.')[:2]) < (2, 28):
            raise SetupError('The portable LLVM toolchain needs glibc 2.28 or newer (this system has %s). '
                             'Use a newer distribution or a distrobox container.' % version)
    except ValueError:
        pass
    if not Path('/lib64/ld-linux-x86-64.so.2').exists():
        if Path('/etc/NIXOS').exists():
            raise SetupError('NixOS: enable nix-ld (programs.nix-ld.enable = true;) and rebuild, or run this setup '
                             'inside steam-run or a distrobox container. The portable build tools need the standard loader.')
        raise SetupError('The standard x86-64 program loader (/lib64/ld-linux-x86-64.so.2) is missing.')
    if not cpu_has_v3():
        warn('This CPU has no AVX2/FMA/BMI2: the slower compatibility version of the game is built.')
    gb = ram_gb()
    if gb and gb < 12:
        warn('This PC has %d GB of RAM; 16 GB is recommended. The build uses fewer parallel jobs and takes longer.' % gb)
    if is_wsl():
        warn('This is WSL. The build works here, but play the game with the Windows version of the port.')
    try:
        if Path('/proc/sys/kernel/apparmor_restrict_unprivileged_userns').read_text().strip() == '1':
            warn('AppArmor restricts user namespaces (Ubuntu 24.04+). If the game does not start, '
                 'see "Ubuntu 24.04" in linux/README.md (Steam\'s own bwrap profile fixes it).')
    except OSError:
        pass


# ------------------------------------------------------------------------------------------------
# Small helpers
# ------------------------------------------------------------------------------------------------

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def atomic_text(path, text, mode=None):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', dir=str(path.parent), delete=False, encoding='utf-8') as f:
        f.write(text)
        tmp = Path(f.name)
    if mode is not None:
        tmp.chmod(mode)
    tmp.replace(path)


def desktop_value(text):
    return str(text).replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')


def desktop_arg(text):
    """One Exec= argument: the Desktop Entry quoting, then the string escaping."""
    text = str(text).replace('%', '%%')
    for char in ('\\', '"', '`', '$'):
        text = text.replace(char, '\\' + char)
    return desktop_value('"' + text + '"')


def cmake_str(text):
    return '"' + str(text).replace('\\', '/').replace('"', '\\"') + '"'


def private_alias(target):
    """A short path without spaces for `target` (conda prefixes must not contain spaces). Only a
    symlink lives in /tmp; everything stays on the chosen drive."""
    base = Path('/tmp') / ('infamous-setup-%d' % os.getuid())
    try:
        base.mkdir(mode=0o700)
    except FileExistsError:
        pass
    info = base.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise SetupError('The temporary setup folder is not private to your user: %s' % base)
    target = Path(target)
    link = base / hashlib.sha256(os.fsencode(str(target))).hexdigest()[:20]
    if link.is_symlink():
        if os.readlink(str(link)) != str(target):
            raise SetupError('The temporary alias %s points elsewhere.' % link)
    elif link.exists():
        raise SetupError('The temporary alias %s is occupied.' % link)
    else:
        link.symlink_to(target, target_is_directory=True)
    return link


def is_install(path):
    return (Path(path) / 'installer' / 'infamous.toml').is_file()


def validate_destination(path, update=False, free_gb=FIRST_INSTALL_GB):
    path = Path(path).expanduser()
    if not path.is_absolute() or path == Path('/'):
        raise SetupError('Choose a full path to a folder for inFamous.')
    if any(c in str(path) for c in '\n\r\t;\\"$`'):
        raise SetupError('Choose a folder without quotes, $, `, ;, backslashes or control characters '
                         '(limits of CMake and the build tools).')
    for base in ('/run/media/', '/media/', '/mnt/'):
        if str(path).startswith(base):
            probe = path
            while not probe.exists():
                probe = probe.parent
            if not any(p.is_mount() for p in [probe] + list(probe.parents)
                       if str(p) not in ('/', '/run', '/run/media', '/media', '/mnt')):
                raise SetupError('That drive is not mounted. Mount it in the file manager first.')
    if path.exists() and not path.is_dir():
        raise SetupError('The install path is a file. Choose a folder.')
    if update and not is_install(path):
        raise SetupError('Choose the existing inFamous folder (the one with installer/ and infamous-recomp/).')
    if path.exists() and not is_install(path):
        allowed = {STATE, LINK_NAME}
        if any(p.name not in allowed for p in path.iterdir()):
            raise SetupError('Choose an empty folder, or an existing inFamous installation.')
    kind = mount_fstype(path)
    if kind in ('vfat', 'exfat', 'msdos', 'ntfs', 'ntfs3', 'fuseblk'):
        raise SetupError('That drive is %s. The build tools and Proton need a Linux file system '
                         '(ext4, Btrfs, XFS, ...). Choose another drive.' % kind)
    path.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(str(path)).free < free_gb * 1024 ** 3:
        raise SetupError('That drive needs at least %d GB free.' % free_gb)
    with tempfile.TemporaryDirectory(prefix='.infamous-check-', dir=str(path)) as tmp:
        probe = Path(tmp)
        exe = probe / 'run'
        exe.write_text('#!/bin/sh\nexit 0\n')
        exe.chmod(0o700)
        try:
            subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)
            (probe / 'link').symlink_to('run')
        except (OSError, subprocess.SubprocessError) as exc:
            raise SetupError('This drive must allow running programs and symbolic links (it may be mounted '
                             'noexec). Use a Linux file system and check its mount options.') from exc
        (probe / 'CaseProbe').write_text('x')
        if (probe / 'caseprobe').exists():
            raise SetupError('This drive ignores upper/lower case. Choose a Linux file system (ext4, Btrfs, ...).')
    return path.resolve()


class KeepAwake:
    """Keeps the PC from sleeping while setup works: the first install takes an hour or more, and a PC
    (a Steam Deck in particular) that suspends meanwhile stops it. A logind inhibitor (systemd-inhibit,
    or elogind-inhibit) is held by a small process that ends by itself when setup does, even if setup
    is killed, so the PC is never left unable to sleep."""

    def __init__(self, say, who):
        self.say = say
        self.who = who
        self.proc = None

    def start(self):
        tool = shutil.which('systemd-inhibit') or shutil.which('elogind-inhibit')
        if not tool:
            self.say('Note: this system cannot be kept awake automatically; keep it from sleeping until setup finishes.')
            return self
        watch = ('import os, sys, time\n'
                 'while True:\n'
                 '    try:\n'
                 '        os.kill(%d, 0)\n'
                 '    except OSError:\n'
                 '        sys.exit(0)\n'
                 '    time.sleep(5)\n' % os.getpid())
        try:
            self.proc = subprocess.Popen([tool, '--what=sleep:idle', '--mode=block', '--who=' + self.who,
                                          '--why=Building the game', sys.executable, '-c', watch],
                                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
            time.sleep(0.5)
        except OSError:
            self.proc = None
        if self.proc is None or self.proc.poll() is not None:
            self.proc = None
            self.say('Note: the PC could not be kept awake automatically; keep it from sleeping until setup finishes.')
        else:
            self.say('Keeping the PC awake until setup finishes.')
        return self

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.proc = None


def tree_pids(pid):
    out = []
    try:
        for child in map(int, Path('/proc/%d/task/%d/children' % (pid, pid)).read_text().split()):
            out.extend(tree_pids(child))
    except (OSError, ValueError):
        pass
    return out + [pid]


def safe_extract_tar(archive, dest, strip=1, keep=None):
    """Extract regular files and folders only, refusing anything that leaves `dest`."""
    dest = Path(dest)
    with tarfile.open(str(archive)) as tar:
        for m in tar:
            parts = Path(m.name).parts[strip:]
            if not parts or m.name.startswith('/') or '..' in Path(m.name).parts:
                continue
            rel = Path(*parts)
            if keep is not None and not keep(rel, m):
                continue
            out = dest / rel
            if m.isdir():
                out.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                out.parent.mkdir(parents=True, exist_ok=True)
                with tar.extractfile(m) as src, open(str(out), 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                out.chmod(0o755 if m.mode & 0o111 else 0o644)
            elif m.issym() and keep is not None:
                # LLVM: clang-cl and friends are links to clang-19 in the same folder.
                target = m.linkname
                if '/' in target or target in ('.', '..'):
                    continue
                out.parent.mkdir(parents=True, exist_ok=True)
                if out.is_symlink() or out.exists():
                    out.unlink()
                out.symlink_to(target)


def find_font():
    """A DejaVu Sans Bold for the keyboard/mouse prompt glyphs (paths differ per distribution)."""
    if shutil.which('fc-match'):
        try:
            out = subprocess.run(['fc-match', '-f', '%{file}', 'DejaVu Sans:bold'], capture_output=True,
                                 text=True, timeout=10).stdout.strip()
            if out.endswith('DejaVuSans-Bold.ttf') and Path(out).is_file():
                return out
        except (OSError, subprocess.SubprocessError):
            pass
    for base in FONT_DIRS:
        for root, _dirs, files in os.walk(base):
            if 'DejaVuSans-Bold.ttf' in files:
                return os.path.join(root, 'DejaVuSans-Bold.ttf')
    return None


def find_rpcs3(explicit=None):
    """RPCS3 decrypts EBOOT.BIN when the dump has no EBOOT.elf. Returns a command list or None."""
    if explicit:
        p = Path(explicit).expanduser()
        if not p.is_file():
            raise SetupError('RPCS3 was not found at %s' % p)
        return [str(p.resolve())]
    found = shutil.which('rpcs3')
    if found:
        return [found]
    flatpak = shutil.which('flatpak')
    if flatpak:
        try:
            listed = subprocess.run([flatpak, 'list', '--app', '--columns=application'], capture_output=True,
                                    text=True, timeout=20).stdout.split()
        except (OSError, subprocess.SubprocessError):
            listed = []
        if 'net.rpcs3.RPCS3' in listed:
            return [flatpak, 'run', 'net.rpcs3.RPCS3']
    home = Path.home()
    for d in (home / 'Applications', home / '.local/bin', home / 'AppImages', home / 'Downloads',
              home / 'Games', home / 'Desktop', home / 'bin', Path('/opt')):
        try:
            hits = sorted(p for p in d.iterdir() if p.is_file() and p.name.lower().startswith('rpcs3')
                          and p.name.lower().endswith('.appimage'))
        except OSError:
            continue
        if hits:
            return [str(hits[-1])]
    return None


# ------------------------------------------------------------------------------------------------
# Installing and updating
# ------------------------------------------------------------------------------------------------

class Engine:
    def __init__(self, destination, game=None, dkey=None, update=False, rpcs3=None,
                 emit=lambda text: print(text, flush=True),
                 cancel=None, repository=DEFAULT_REPO, branch=DEFAULT_BRANCH, port_source=None,
                 accept_license=False, build_jobs=None):
        self.destination = Path(destination).expanduser()
        self.game = Path(game).expanduser().resolve() if game else None
        self.dkey = Path(dkey).expanduser().resolve() if dkey else None
        self.update = update
        self.rpcs3 = rpcs3
        self.emit = emit
        self.cancel = cancel or threading.Event()
        self.repository = repository
        self.branch = branch
        self.port_source = port_source
        self.accept_license = accept_license
        self.build_jobs = build_jobs
        self.log = None
        self.state = None
        self.alias = None
        self.env = os.environ.copy()

    # ---- output and processes ----

    def say(self, text):
        text = str(text)
        if self.log:
            self.log.write(text + '\n')
            self.log.flush()
        self.emit(text)

    def check_cancel(self):
        if self.cancel.is_set():
            raise Cancelled('Setup stopped. Run it again to continue; finished downloads and build steps are kept.')

    def run(self, command, cwd=None, env=None, what='A setup step'):
        self.check_cancel()
        command = [str(c) for c in command]
        self.say('$ ' + ' '.join(shlex.quote(c) for c in command))
        process = subprocess.Popen(command, cwd=None if cwd is None else str(cwd), env=env or self.env,
                                   stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, start_new_session=True)
        pending = b''
        try:
            with selectors.DefaultSelector() as sel:
                sel.register(process.stdout, selectors.EVENT_READ)
                while sel.get_map():
                    self.check_cancel()
                    for key, _ in sel.select(timeout=0.2):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        if not chunk:
                            sel.unregister(key.fileobj)
                            continue
                        pending += chunk.replace(b'\r', b'\n')
                        lines = pending.split(b'\n')
                        pending = lines.pop()
                        for line in lines:
                            if line.strip():
                                self.say(line.decode('utf-8', 'replace'))
            if pending.strip():
                self.say(pending.decode('utf-8', 'replace'))
            code = process.wait()
            if code:
                raise SetupError('%s failed (exit code %d). The lines above and the log (%s) say why.'
                                 % (what, code, self.state / 'setup.log' if self.state else 'setup.log'))
        finally:
            if process.poll() is None:
                pids = tree_pids(process.pid)
                for sig in (signal.SIGTERM, signal.SIGKILL):
                    for pid in pids:
                        try:
                            os.kill(pid, sig)
                        except ProcessLookupError:
                            pass
                    try:
                        process.wait(timeout=3)
                        break
                    except subprocess.TimeoutExpired:
                        pass
            process.stdout.close()

    def download(self, url, dest, sha256, size=None, label=None):
        dest = Path(dest)
        if dest.is_file() and sha256_file(dest) == sha256:
            return dest
        dest.parent.mkdir(parents=True, exist_ok=True)
        partial = dest.with_name(dest.name + '.partial')
        label = label or dest.name
        error = None
        for attempt in range(3):
            self.check_cancel()
            digest = hashlib.sha256()
            try:
                self.say('Downloading %s%s' % (label, ' (%d MB)' % (size >> 20) if size else ''))
                req = urllib.request.Request(url, headers={'User-Agent': 'inFamous-Linux-Setup/1'})
                with urllib.request.urlopen(req, timeout=60) as resp, open(str(partial), 'wb') as out:
                    total = int(resp.headers.get('Content-Length') or size or 0)
                    done, shown = 0, -1
                    while True:
                        self.check_cancel()
                        chunk = resp.read(1 << 20)
                        if not chunk:
                            break
                        out.write(chunk)
                        digest.update(chunk)
                        done += len(chunk)
                        if total:
                            pct = done * 100 // total
                            if pct >= shown + 10:
                                shown = pct
                                self.say('    %d%% of %d MB' % (pct, total >> 20))
                if digest.hexdigest() != sha256:
                    raise SetupError('%s failed its SHA-256 check and was not used.' % label)
                partial.replace(dest)
                return dest
            except (OSError, urllib.error.URLError) as exc:
                error = exc
                self.say('Download failed: %s' % exc)
                time.sleep(2)
            finally:
                if partial.exists():
                    partial.unlink()
        raise SetupError('Could not download %s. Check the internet connection (and proxy settings), '
                         'then press Install again. (%s)' % (label, error))

    # ---- the private environment ----

    def mamba(self, *args):
        return [self.alias / 'micromamba', '--no-rc', '--root-prefix', self.alias / 'mamba-pkgs'] + list(args)

    def prepare_environment(self):
        self.download(MAMBA_URL, self.state / 'micromamba', MAMBA_SHA256, label='the private package manager')
        (self.state / 'micromamba').chmod(0o755)
        marker = self.state / 'env-version'
        if not marker.is_file() or marker.read_text() != ENV_VERSION:
            self.say('Downloading Python, Rust and small helper programs into %s (no administrator rights needed).'
                     % (self.state / 'env'))
            action = 'install' if (self.state / 'env/conda-meta/history').is_file() else 'create'
            self.run(self.mamba(action, '--yes', '--prefix', self.alias / 'env', '--channel', 'conda-forge',
                                '--strict-channel-priority', *PACKAGES), what='Downloading the private environment')
            atomic_text(marker, ENV_VERSION)
        if not (self.state / 'llvm-runtime/lib/libxml2.so.2').is_file():
            self.say('Downloading the LLVM linker runtime (libxml2.so.2).')
            action = 'install' if (self.state / 'llvm-runtime/conda-meta/history').is_file() else 'create'
            self.run(self.mamba(action, '--yes', '--prefix', self.alias / 'llvm-runtime', '--channel',
                                'conda-forge', '--strict-channel-priority', *LLVM_RUNTIME),
                     what='Downloading the LLVM runtime')
            # The package downloads (about 500 MB) are not needed once both environments exist.
            try:
                self.run(self.mamba('clean', '--all', '--yes'), what='Removing package downloads')
            except SetupError as exc:
                self.say('Note: %s' % exc)
        prefix = self.alias / 'env'
        self.python = prefix / 'bin/python3'
        if not self.python.is_file():
            raise SetupError('The private environment is incomplete. Delete %s and press Install again.' % marker)
        tc = self.alias / 'toolchain'
        self.env.update({
            'PATH': os.pathsep.join([str(tc / 'llvm/bin'), str(tc / 'cmake/bin'), str(tc / 'ninja'),
                                     str(prefix / 'bin'), self.env.get('PATH', '/usr/bin:/bin')]),
            'LD_LIBRARY_PATH': os.pathsep.join(filter(None, [str(self.alias / 'llvm-runtime/lib'),
                                                             self.env.get('LD_LIBRARY_PATH')])),
            'CARGO_HOME': str(self.alias / 'cargo'),
            'CARGO_TARGET_DIR': str(self.alias / 'cargo-target'),
            'CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER': str(prefix / 'bin/x86_64-conda-linux-gnu-cc'),
            'SSL_CERT_FILE': str(prefix / 'ssl/cacert.pem'),
            'TMPDIR': str(self.alias / 'tmp'),
            'PIP_CACHE_DIR': str(self.alias / 'pip-cache'),
            'PYTHONNOUSERSITE': '1',
        })
        (self.state / 'tmp').mkdir(exist_ok=True)

    # ---- the port's files ----

    def get_port(self):
        if is_install(self.destination):
            return
        src = self.port_source
        if src and Path(src).is_dir():
            self.say('Copying the port from %s' % src)
            ignore = shutil.ignore_patterns('.git', '.installer', STATE, 'target', 'build-*', '__pycache__')
            shutil.copytree(str(src), str(self.destination), ignore=ignore, dirs_exist_ok=True)
        else:
            url = src or 'https://codeload.github.com/%s/tar.gz/refs/heads/%s' % (self.repository, self.branch)
            archive = self.state / 'port.tar.gz'
            self.say('Downloading the port from %s' % url)
            req = urllib.request.Request(url, headers={'User-Agent': 'inFamous-Linux-Setup/1'})
            try:
                with urllib.request.urlopen(req, timeout=60) as resp, open(str(archive), 'wb') as out:
                    shutil.copyfileobj(resp, out, 1 << 20)
            except (OSError, urllib.error.URLError) as exc:
                raise SetupError('Could not download the port (%s): %s' % (url, exc)) from exc
            safe_extract_tar(archive, self.destination, strip=1)
            archive.unlink()
        if not is_install(self.destination):
            raise SetupError('The downloaded port has no installer/infamous.toml.')

    def build_installer(self):
        src = self.destination / 'installer'
        digest = hashlib.sha256(ENV_VERSION.encode())
        for p in sorted(list((src / 'src').rglob('*')) + [src / 'Cargo.toml', src / 'Cargo.lock']):
            if p.is_file():
                digest.update(str(p.relative_to(src)).encode())
                digest.update(p.read_bytes())
        key = digest.hexdigest()
        marker = self.state / 'installer-version'
        self.installer = src / 'infamous-installer'
        if self.installer.is_file() and marker.is_file() and marker.read_text() == key:
            return
        self.say('Building the installer (Rust, about a minute the first time).')
        # Only the command-line installer: the setup window and the launcher are Windows programs.
        self.run([self.alias / 'env/bin/cargo', 'build', '--release', '--locked', '--no-default-features',
                  '--bin', 'infamous-installer'], cwd=src, what='Building the installer')
        shutil.copy2(str(self.alias / 'cargo-target/release/infamous-installer'), str(self.installer))
        atomic_text(marker, key)

    # ---- the Windows toolchain ----

    def prepare_toolchain(self):
        tc = self.state / 'toolchain'
        for name, url, sha, size, folder in TOOLS:
            done = tc / folder / '.complete'
            if done.is_file() and done.read_text() == sha:
                continue
            archive = self.download(url, self.state / 'downloads' / url.rsplit('/', 1)[1], sha, size, name)
            self.say('Unpacking %s' % name)
            target = tc / folder
            if target.exists():
                shutil.rmtree(str(target))
            target.mkdir(parents=True)
            if url.endswith('.zip'):
                with zipfile.ZipFile(str(archive)) as z:
                    for info in z.infolist():
                        if info.is_dir() or '..' in Path(info.filename).parts or info.filename.startswith('/'):
                            continue
                        out = target / Path(info.filename).name
                        with z.open(info) as s, open(str(out), 'wb') as d:
                            shutil.copyfileobj(s, d)
                        out.chmod(0o755)
            elif folder == 'llvm':
                def keep(rel, m):
                    if rel.parts[0] == 'bin' and len(rel.parts) == 2:
                        n = rel.name
                        return n in LLVM_KEEP or re.fullmatch(r'clang-\d+', n) is not None
                    return rel.parts[:2] == ('lib', 'clang')
                safe_extract_tar(archive, target, strip=1, keep=keep)
            else:
                safe_extract_tar(archive, target, strip=1)
            atomic_text(done, sha)
            archive.unlink()
        ws = tc / ('winsysroot-%s-%s' % (XWIN_CRT, XWIN_SDK))
        if not (ws / '.complete').is_file():
            if not self.accept_license:
                raise SetupError('The Microsoft C++ headers and libraries come under the Visual Studio license '
                                 '(%s). Accept it and press Install again.' % MS_LICENSE_URL)
            self.say('Downloading the Microsoft C++ headers and libraries (about 600 MB, once).')
            if ws.exists():
                shutil.rmtree(str(ws))
            self.run([self.alias / 'toolchain/xwin/xwin', '--accept-license', '--log-level', 'warn',
                      '--http-retry', '3', '--cache-dir', self.alias / 'xwin-cache',
                      '--crt-version', XWIN_CRT, '--sdk-version', XWIN_SDK,
                      'splat', '--use-winsysroot-style', '--preserve-ms-arch-notation',
                      '--output', self.alias / 'toolchain' / ws.name], what='Downloading the Microsoft headers')
            shutil.rmtree(str(self.state / 'xwin-cache'), ignore_errors=True)
            atomic_text(ws / '.complete', XWIN_CRT + ' ' + XWIN_SDK)
        self.cross_file = write_cross_files(self.alias / 'toolchain', self.alias / 'toolchain' / ws.name)
        version = subprocess.run([str(self.alias / 'toolchain/llvm/bin/lld-link'), '--version'],
                                 capture_output=True, text=True, env=self.env).stdout
        if LLVM_VERSION not in version:
            raise SetupError('The downloaded LLVM linker does not start (lld-link --version: %r).' % version.strip())

    def prepare_d3dcompiler(self):
        dll = self.state / 'd3dcompiler_47.dll'
        if dll.is_file() and sha256_file(dll) == D3DC_SHA256:
            return dll
        cab = self.download(D3DC_CAB_URL, self.state / 'downloads/d3dcompiler_47.cab', D3DC_CAB_SHA256,
                            label="Microsoft's shader compiler (d3dcompiler_47)")
        out = self.state / 'tmp/d3dc'
        shutil.rmtree(str(out), ignore_errors=True)
        self.run([self.alias / 'env/bin/7z', 'e', '-y', '-o' + str(out), cab, D3DC_MEMBER],
                 what='Unpacking d3dcompiler_47')
        got = out / D3DC_MEMBER
        if not got.is_file() or sha256_file(got) != D3DC_SHA256:
            raise SetupError('d3dcompiler_47.dll from the Windows SDK did not match its expected SHA-256.')
        shutil.move(str(got), str(dll))
        shutil.rmtree(str(out), ignore_errors=True)
        return dll

    # ---- the game ----

    def game_folder(self):
        """The dump to build from: a folder the player chose, or the disc image copied out next to the port."""
        linked = self.destination / LINK_NAME
        if self.game is None:
            if (linked / 'PS3_GAME').is_dir():
                return linked
            if self.update:
                return None
            raise SetupError('Choose your inFamous disc image (.iso) or game folder (the one with PS3_GAME).')
        if self.game.is_dir():
            for c in (self.game, self.game.parent, self.game.parent.parent):
                if (c / 'PS3_GAME/PARAM.SFO').is_file():
                    return c
            raise SetupError('%s is not a PS3 game folder: PS3_GAME/PARAM.SFO is missing.' % self.game)
        if not self.game.is_file():
            raise SetupError('%s does not exist.' % self.game)
        meta = self.game.stat()
        stamp = '%s|%d' % (self.game, meta.st_size)
        marker = linked / '.copied-from-disc-image'
        if marker.is_file() and marker.read_text() == stamp:
            self.say('The game files were already copied from this disc image.')
            return linked
        if linked.is_symlink():
            linked.unlink()
        self.say('Copying the game out of the disc image (decrypting it with the disc key if there is one).')
        cmd = [self.installer, 'extract-iso', '--iso', self.game, '--out', linked]
        dkey = self.dkey or next((p for p in (self.game.with_suffix('.dkey'), Path(str(self.game) + '.dkey'))
                                  if p.is_file()), None)
        if dkey:
            cmd += ['--dkey', dkey]
        self.run(cmd, what='Copying the game from the disc image')
        atomic_text(marker, stamp)
        return linked

    def rpcs3_wrapper(self, game):
        usrdir = game / 'PS3_GAME/USRDIR' if game else None
        if usrdir and (usrdir / 'EBOOT.elf').is_file():
            return None
        found = find_rpcs3(self.rpcs3)
        if not found:
            self.say('RPCS3 was not found. It is needed only if your dump has an encrypted EBOOT.BIN and no '
                     'EBOOT.elf; install it (Flatpak net.rpcs3.RPCS3 or the AppImage from rpcs3.net) if the '
                     'next step asks for it.')
            return None
        wrapper = self.state / 'rpcs3'
        lines = ['#!/bin/sh', '# Written by the inFamous Linux setup: runs RPCS3 for "rpcs3 --decrypt".',
                 'export APPIMAGE_EXTRACT_AND_RUN=1   # AppImages without FUSE',
                 'if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then export QT_QPA_PLATFORM=offscreen; fi']
        if found[:2] == [shutil.which('flatpak'), 'run']:
            access = ['--filesystem=' + str(p) for p in {str(self.destination), str(game)}]
            found = found[:2] + access + found[2:]
        lines.append('exec ' + ' '.join(shlex.quote(c) for c in found) + ' "$@"')
        atomic_text(wrapper, '\n'.join(lines) + '\n', 0o755)
        self.say('RPCS3: %s' % ' '.join(found))
        return wrapper

    # ---- the steps ----

    def installer_env(self):
        env = dict(self.env)
        font = find_font()
        if font:
            env['KBM_FONT'] = font
        env['INFAMOUS_CROSS_FILE'] = str(self.cross_file)
        return env

    def common_args(self):
        args = ['--root', self.destination, '--python', self.python, '--cross', self.cross_file, '-y']
        if self.build_jobs:
            args += ['--build-jobs', str(self.build_jobs)]
        elif ram_gb() and ram_gb() < 12:
            args += ['--build-jobs', '1']
        return args

    def finish(self, d3dcompiler):
        exe_dir = self.destination / 'infamous-recomp/build-install'
        shutil.copy2(str(d3dcompiler), str(exe_dir / 'd3dcompiler_47.dll'))
        launcher = self.destination / 'installer/bin/infamous-launch.exe'
        if launcher.is_file():
            shutil.copy2(str(launcher), str(self.destination / (TITLE + '.exe')))
        else:
            self.say('Note: installer/bin/infamous-launch.exe is missing; the game starts without the options window.')
        # inFamous.exe passes launch.txt's env= lines to the game, so Wine uses Microsoft's d3dcompiler_47.dll
        # however it is started (no Steam launch options). The installer writes this line itself; an update
        # run by an older installer binary did not, so make sure.
        launch_txt = self.destination / 'installer/launch.txt'
        if launch_txt.is_file():
            text = launch_txt.read_text()
            if 'env=WINEDLLOVERRIDES=' not in text:
                atomic_text(launch_txt, text.rstrip('\n') + '\nenv=WINEDLLOVERRIDES=d3dcompiler_47=n,b\n')
        self.integrate()

    def execute(self):
        preflight(lambda w: self.say('Note: ' + w))
        self.destination = validate_destination(self.destination, self.update,
                                                UPDATE_GB if self.update else FIRST_INSTALL_GB)
        self.state = self.destination / STATE
        self.state.mkdir(exist_ok=True)
        self.alias = private_alias(self.state)
        with open(str(self.state / 'setup.lock'), 'a') as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError as exc:
                raise SetupError('Another setup is already working on this folder.') from exc
            with open(str(self.state / 'setup.log'), 'a', buffering=1, encoding='utf-8') as log:
                self.log = log
                awake = KeepAwake(self.say, TITLE + ' Setup')
                try:
                    self.say('\n%s Linux setup  %s' % (TITLE, time.strftime('%Y-%m-%d %H:%M:%S')))
                    self.say('Install folder: %s' % self.destination)
                    self.say('System: %s, %s' % (os_release().get('PRETTY_NAME', 'Linux'), platform.release()))
                    awake.start()
                    self.prepare_environment()
                    self.get_port()
                    self.build_installer()
                    self.prepare_toolchain()
                    d3dc = self.prepare_d3dcompiler()
                    game = self.game_folder()
                    wrapper = self.rpcs3_wrapper(game)
                    env = self.installer_env()
                    if self.update and (self.destination / '.installer/state').is_file():
                        self.say('Checking for a new version of the port.')
                        cmd = [self.installer, 'update', '--repo', self.repository, '--branch', self.branch]
                    else:
                        cmd = [self.installer, 'install']
                        if game:
                            cmd += ['--game', game]
                    if wrapper:
                        cmd += ['--rpcs3', wrapper]
                    self.say('Regenerating the game code from your files and building it. The first build '
                             'takes a long time (an hour or more); later updates only rebuild what changed.')
                    self.run(cmd + self.common_args(), cwd=self.destination, env=env, what='The installer')
                    self.build_installer()   # an update may have brought new installer sources
                    self.prepare_runner()
                    self.finish(d3dc)
                    self.say('Finished. %s' % ('Use "Add to Steam", then start inFamous from Steam.' if is_steamos()
                                                else 'Start inFamous from your application menu (Games), '
                                                     'or press Play. Proton downloads itself on the first start.'))
                except Exception as exc:
                    self.say(str(exc))
                    raise
                finally:
                    awake.stop()
                    self.log = None

    # ---- running the game ----

    def prepare_runner(self):
        if is_steamos():
            self.say('SteamOS: the game runs through Steam (Add to Steam), with the Proton you choose there.')
            return
        archive = self.download(UMU_URL, self.state / 'downloads/umu-launcher-1.4.4.tar', UMU_SHA256,
                                label='the Proton launcher (umu)')
        runner = self.state / 'umu/umu-run'
        with tarfile.open(str(archive)) as tar:
            members = [m for m in tar.getmembers() if m.isfile() and Path(m.name).name == 'umu-run']
            if len(members) != 1 or members[0].size > 5 * 1024 ** 2:
                raise SetupError('The umu-launcher archive has an unexpected layout.')
            data = tar.extractfile(members[0]).read()
        runner.parent.mkdir(exist_ok=True)
        runner.write_bytes(data)
        runner.chmod(0o755)

    def integrate(self):
        """Keep a copy of this setup in the install folder and add menu entries that use it."""
        bundle = self.state / 'setup'
        bundle.mkdir(exist_ok=True)
        if HERE.resolve() != bundle.resolve():
            for name in ('setup.py', 'launch.sh', 'README.md'):
                if (HERE / name).is_file():
                    shutil.copy2(str(HERE / name), str(bundle / name))
        (bundle / 'launch.sh').chmod(0o755)
        atomic_text(self.state / 'installation.json', json.dumps(
            {'directory': str(self.destination), 'repository': self.repository, 'branch': self.branch}, indent=2))
        apps = Path(os.environ.get('XDG_DATA_HOME') or Path.home() / '.local/share') / 'applications'
        ident = hashlib.sha256(os.fsencode(str(self.destination))).hexdigest()[:10]
        icon = self.destination / 'ps3recomp/packaging/macos/icon_1024.png'
        icon_line = 'Icon=%s\n' % desktop_value(icon) if icon.is_file() else ''
        run = lambda *a: ' '.join(desktop_arg(x) for x in (bundle / 'launch.sh',) + a)
        entries = {
            'infamous-update-%s.desktop' % ident:
                ('Update %s' % TITLE, run('--update', '--dir', self.destination), 'false'),
        }
        if is_steamos():
            entries['infamous-%s.desktop' % ident] = (
                'Add %s to Steam' % TITLE, run('--add-to-steam', '--dir', self.destination), 'false')
        else:
            entries['infamous-%s.desktop' % ident] = (TITLE, run('--play', '--dir', self.destination), 'false')
        for name, (title, command, term) in entries.items():
            atomic_text(apps / name, '[Desktop Entry]\nType=Application\nName=%s\nExec=%s\n%sCategories=Game;\n'
                                     'Terminal=%s\n' % (title, command, icon_line, term))
        self.say('Menu entries added: %s, Update %s.' % (TITLE if not is_steamos() else 'Add to Steam', TITLE))


def write_cross_files(tc, winsysroot):
    """The CMake toolchain file for the clang-cl build, plus the case-insensitive file overlay that
    Microsoft's headers and libraries need on a case-sensitive file system (the SDK includes
    <WinDef.h> while the file is windef.h, and so on)."""
    def walk(d):
        items = []
        for e in sorted(os.scandir(str(d)), key=lambda e: e.name):
            if e.is_symlink():
                continue   # xwin's case links; the overlay replaces them
            if e.is_dir():
                items.append({'name': e.name, 'type': 'directory', 'contents': walk(e.path)})
            else:
                items.append({'name': e.name, 'type': 'file', 'external-contents': e.path})
        return items
    overlay = Path(tc) / 'winsysroot.vfs.json'
    atomic_text(overlay, json.dumps({'version': 0, 'case-sensitive': 'false', 'roots': [
        {'name': str(winsysroot), 'type': 'directory', 'contents': walk(winsysroot)}]}))
    llvm = Path(tc) / 'llvm/bin'
    compile_flags = '/winsysroot %s /clang:-ivfsoverlay /clang:%s' % (winsysroot, overlay)
    link_flags = '/winsysroot:%s /vfsoverlay:%s' % (winsysroot, overlay)
    text = '\n'.join([
        '# Written by the inFamous Linux setup: the Windows x64 game built with clang-cl + lld-link and',
        "# Microsoft's headers/libraries (xwin), like the Windows build with Visual Studio's clang-cl.",
        'set(CMAKE_SYSTEM_NAME Windows)',
        'set(CMAKE_SYSTEM_VERSION 10.0)',
        'set(CMAKE_SYSTEM_PROCESSOR AMD64)',
        'set(CMAKE_C_COMPILER %s)' % cmake_str(llvm / 'clang-cl'),
        'set(CMAKE_CXX_COMPILER %s)' % cmake_str(llvm / 'clang-cl'),
        'set(CMAKE_LINKER %s)' % cmake_str(llvm / 'lld-link'),
        'set(CMAKE_AR %s)' % cmake_str(llvm / 'llvm-lib'),
        'set(CMAKE_RC_COMPILER %s)' % cmake_str(llvm / 'llvm-rc'),
        'set(CMAKE_MT %s)' % cmake_str(llvm / 'llvm-mt'),
        'set(CMAKE_C_FLAGS_INIT %s)' % cmake_str(compile_flags),
        'set(CMAKE_CXX_FLAGS_INIT %s)' % cmake_str(compile_flags),
        'set(CMAKE_RC_FLAGS_INIT %s)' % cmake_str('/winsysroot %s' % winsysroot),
        'set(CMAKE_EXE_LINKER_FLAGS_INIT %s)' % cmake_str(link_flags),
        'set(CMAKE_SHARED_LINKER_FLAGS_INIT %s)' % cmake_str(link_flags),
        'set(CMAKE_MODULE_LINKER_FLAGS_INIT %s)' % cmake_str(link_flags),
        '# Only the release C runtime is downloaded.',
        'set(CMAKE_TRY_COMPILE_CONFIGURATION Release)',
        "# The C/C++ runtime linked in (/MT): Proton's own vcruntime140/msvcp140 then never matter.",
        'set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)',
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)',
        '',
    ])
    path = Path(tc) / 'windows-x64.cmake'
    atomic_text(path, text)
    return path


# ------------------------------------------------------------------------------------------------
# Playing
# ------------------------------------------------------------------------------------------------

def installed_state(directory):
    directory = Path(directory).expanduser().resolve()
    if not is_install(directory):
        raise SetupError('%s is not an inFamous installation.' % directory)
    return directory, directory / STATE


def game_exe(directory):
    for p in (directory / (TITLE + '.exe'), directory / 'infamous-recomp/build-install/inFamousRecomp.exe'):
        if p.is_file():
            return p
    raise SetupError('The game is not built yet. Run Install or Update first.')


def play(directory):
    if is_steamos():
        raise SetupError('On SteamOS, use "Add %s to Steam" and start it from your Steam library.' % TITLE)
    directory, state = installed_state(directory)
    exe = game_exe(directory)
    alias = private_alias(state)
    runner = state / 'umu/umu-run'
    python = alias / 'env/bin/python3'
    if not runner.is_file() or not python.is_file():
        raise SetupError('Setup has not finished. Run Install or Update first.')
    env = os.environ.copy()
    for d in ('prefix', 'umu', 'tmp', 'cache'):
        (state / d).mkdir(exist_ok=True)
    env.update({
        'WINEPREFIX': str(state / 'prefix'),
        'GAMEID': 'umu-default',
        'STORE': 'none',
        'PROTONPATH': env.get('PROTONPATH', 'GE-Proton'),   # the latest GE-Proton, downloaded on first use
        'UMU_ZENITY': '1',
        'XDG_CACHE_HOME': str(state / 'cache'),
        'WINEDLLOVERRIDES': ';'.join(filter(None, [env.get('WINEDLLOVERRIDES'), 'd3dcompiler_47=n,b'])),
    })
    if has_nvidia():
        env.setdefault('PROTON_ENABLE_NVAPI', '1')   # DLSS
    env.pop('PYTHONHOME', None)
    env.pop('PYTHONPATH', None)
    with open(str(state / 'play.log'), 'a') as log:
        log.write('\n--- %s\n' % time.strftime('%Y-%m-%d %H:%M:%S'))
        log.flush()
        code = subprocess.run([str(python), str(runner), str(exe)], cwd=str(exe.parent), env=env,
                              stdout=log, stderr=subprocess.STDOUT).returncode
    if code:
        text = Path(state / 'play.log').read_text(errors='replace')[-20000:]
        hint = ''
        if 'setting up uid map' in text or 'bwrap' in text and 'Permission denied' in text:
            hint = ('\n\nThe Steam Linux Runtime could not create its sandbox (user namespaces are restricted). '
                    'See "Ubuntu 24.04" in linux/README.md.')
        raise SetupError('The game exited with code %d. See %s%s' % (code, state / 'play.log', hint))
    return 0


def steam_command():
    native = shutil.which('steam')
    if native:
        return [native], False
    flatpak = shutil.which('flatpak')
    if flatpak and (Path.home() / '.var/app/com.valvesoftware.Steam').is_dir():
        return [flatpak, 'run', 'com.valvesoftware.Steam'], True
    raise SetupError('Steam was not found. Install or open Steam, then try again.')


def add_to_steam(directory):
    directory, state = installed_state(directory)
    exe = game_exe(directory)
    command, is_flatpak = steam_command()
    manual = ('In Steam: Games > Add a Non-Steam Game to My Library > Browse > %s\n'
              'Then Properties > Compatibility: tick "Force the use of a specific Steam Play compatibility tool" '
              'and choose Proton 9 or newer (or GE-Proton).' % exe)
    if is_flatpak:
        if not str(directory).startswith(str(Path.home()) + '/'):
            subprocess.run([shutil.which('flatpak'), 'override', '--user', '--filesystem=%s' % directory,
                            'com.valvesoftware.Steam'], check=False)
        return 'Flatpak Steam cannot be asked to add games directly.\n\n' + manual
    # Steam adds the file it is given as the shortcut's target (a .desktop file would itself become the
    # target, which Proton cannot run), with the file's folder as its start folder.
    sentinel = Path('/tmp/addnonsteamgamefile')
    fd = os.open(str(sentinel), os.O_CREAT | os.O_WRONLY | os.O_NOFOLLOW | os.O_NONBLOCK, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            raise SetupError('The Steam import marker %s is not a file owned by you.' % sentinel)
    finally:
        os.close(fd)
    try:
        subprocess.run(command + ['steam://addnonsteamgame/' + quote(str(exe), safe='')], check=True,
                       timeout=30, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return 'Steam did not take the request.\n\n' + manual
    return ('Sent to Steam: find %s in your library.\n\nThen, in its Properties > Compatibility, tick "Force the '
            'use of a specific Steam Play compatibility tool" and choose Proton 9 or newer (or GE-Proton). '
            'No launch options are needed.' % TITLE)


def doctor(directory=None):
    lines = []
    ok = True
    try:
        preflight(lambda w: lines.append('note   ' + w))
        lines.insert(0, 'ok     x86-64, glibc %s, standard loader' % platform.libc_ver()[1])
    except SetupError as exc:
        ok = False
        lines.insert(0, 'FAIL   ' + str(exc))
    rel = os_release()
    lines.append('info   %s (%s), kernel %s, %d GB RAM, %s'
                 % (rel.get('PRETTY_NAME', '?'), rel.get('ID', '?'), platform.release(), ram_gb(),
                    'AVX2 build' if cpu_has_v3() else 'compatibility build (no AVX2/FMA/BMI2)'))
    icds = [p for d in ('/usr/share/vulkan/icd.d', '/etc/vulkan/icd.d', '/run/opengl-driver/share/vulkan/icd.d')
            for p in Path(d).glob('*.json')]
    lines.append(('ok     Vulkan drivers: %s' % ', '.join(sorted({p.name for p in icds}))) if icds else
                 'WARN   no Vulkan driver found (Proton needs one: mesa-vulkan-drivers / vulkan-radeon / '
                 'vulkan-intel / the NVIDIA driver)')
    lines.append('ok     DejaVu Sans Bold: %s' % find_font() if find_font() else
                 'note   DejaVu Sans Bold not found: keyboard/mouse prompt glyphs are skipped (install fonts-dejavu)')
    rp = find_rpcs3()
    lines.append('ok     RPCS3: %s' % ' '.join(rp) if rp else
                 'note   RPCS3 not found (only needed for a dump with an encrypted EBOOT.BIN and no EBOOT.elf)')
    try:
        cmd, flat = steam_command()
        lines.append('ok     Steam: %s%s' % (' '.join(cmd), ' (Flatpak)' if flat else ''))
    except SetupError:
        lines.append('note   Steam not found (not needed: the game runs with umu + GE-Proton)')
    if directory:
        try:
            d = Path(directory).expanduser()
            lines.append('info   %s: %s file system, %d GB free' % (d, mount_fstype(d) or '?',
                                                                    shutil.disk_usage(str(d if d.exists() else d.parent)).free >> 30))
        except OSError:
            pass
    return ok, '\n'.join(lines)


# ------------------------------------------------------------------------------------------------
# The window
# ------------------------------------------------------------------------------------------------

def gui(args):
    import tkinter as tk
    from tkinter import filedialog, messagebox, scrolledtext, ttk
    root = tk.Tk()
    root.title('%s — Linux Setup' % TITLE)
    width = min(920, max(620, root.winfo_screenwidth() - 80))
    height = min(760, max(520, root.winfo_screenheight() - 100))
    root.geometry('%dx%d' % (width, height))
    root.columnconfigure(0, weight=1)
    root.rowconfigure(0, weight=1)
    frame = ttk.Frame(root, padding=(18, 14, 18, 0))
    frame.grid(row=0, column=0, sticky='nsew')
    ttk.Label(frame, text=TITLE, font=('', 22, 'bold')).pack(anchor='w')
    ttk.Label(frame, text='Builds the PC port from your own European disc (BCES00220) and runs it with Proton.\n'
                          'The build tools download into the install folder; nothing is installed system-wide.'
              ).pack(anchor='w', pady=(4, 12))
    remembered = None
    config = HERE.parent / 'installation.json'
    if config.is_file():
        remembered = json.loads(config.read_text()).get('directory')
    directory = tk.StringVar(value=args.dir or remembered or str(Path.home() / 'Games/inFamous'))
    game = tk.StringVar(value=args.iso or args.game_dir or '')
    rpcs3 = tk.StringVar(value=args.rpcs3 or '')
    accepted = tk.BooleanVar(value=args.accept_license or
                             (Path(directory.get()).expanduser() / STATE / 'toolchain').is_dir())

    def row(title, var, buttons):
        ttk.Label(frame, text=title).pack(anchor='w')
        r = ttk.Frame(frame)
        r.pack(fill='x', pady=(3, 9))
        ttk.Entry(r, textvariable=var).pack(side='left', fill='x', expand=True)
        for label, chooser in buttons:
            def pick(c=chooser):
                v = c()
                if v:
                    var.set(v)
            ttk.Button(r, text=label, command=pick).pack(side='left', padx=(8, 0))

    row('Install folder (any Linux drive, about 40 GB)', directory,
        [('Browse…', lambda: filedialog.askdirectory(title='Install folder', mustexist=False))])
    row('Your game: the disc image, or the folder that contains PS3_GAME', game,
        [('Disc image…', lambda: filedialog.askopenfilename(title='Your inFamous disc image',
                                                          filetypes=[('PS3 disc image', '*.iso *.ISO'), ('All files', '*')])),
         ('Game folder…', lambda: filedialog.askdirectory(title='The folder that contains PS3_GAME'))])
    row('RPCS3 (optional: only for an encrypted EBOOT.BIN; found automatically when installed)', rpcs3,
        [('Browse…', lambda: filedialog.askopenfilename(title='RPCS3 (AppImage or program)'))])
    lic = ttk.Frame(frame)
    lic.pack(anchor='w', fill='x')
    ttk.Checkbutton(lic, text="I accept Microsoft's license for the C++ headers and libraries the build downloads",
                    variable=accepted).pack(side='left')
    ttk.Button(lic, text='Read', command=lambda: __import__('webbrowser').open(MS_LICENSE_URL)).pack(side='left', padx=8)
    status = tk.StringVar(value='Choose the folder and your game, then press Install / Update. '
                                'The first build takes an hour or more.')
    ttk.Label(frame, textvariable=status, wraplength=width - 60).pack(anchor='w', pady=(10, 0))
    progress = ttk.Progressbar(frame, mode='indeterminate')
    progress.pack(fill='x', pady=8)
    output = scrolledtext.ScrolledText(frame, height=10, wrap='word', state='disabled')
    output.pack(fill='both', expand=True)
    controls = ttk.Frame(root, padding=(18, 10, 18, 14))
    controls.grid(row=1, column=0, sticky='ew')
    for c in range(3):
        controls.columnconfigure(c, weight=1)
    events = queue.Queue()
    cancel = threading.Event()
    busy = {'on': False}

    def append(text):
        output.configure(state='normal')
        output.insert('end', text + '\n')
        if int(output.index('end-1c').split('.')[0]) > 3000:
            output.delete('1.0', '800.0')
        output.see('end')
        output.configure(state='disabled')

    def set_busy(on):
        busy['on'] = on
        for b in (install_b, play_b, steam_b, doctor_b):
            b.configure(state='disabled' if on else 'normal')
        stop_b.configure(state='normal' if on else 'disabled')
        (progress.start if on else progress.stop)()

    def background(job, done_text):
        def work():
            try:
                events.put(('done', job() or done_text))
            except Exception as exc:   # shown to the player; the log has the details
                events.put(('error', str(exc)))
        set_busy(True)
        threading.Thread(target=work, daemon=True).start()

    def start():
        if not accepted.get():
            messagebox.showinfo('License', "Accept Microsoft's license for the C++ headers and libraries first.")
            return
        chosen = Path(directory.get().strip()).expanduser()
        update = is_install(chosen)
        if update and not messagebox.askokcancel('Update', 'Update the installation in\n%s?' % chosen):
            return
        cancel.clear()
        engine = Engine(chosen, game.get().strip() or None, update=update, rpcs3=rpcs3.get().strip() or None,
                        emit=lambda t: events.put(('log', t)), cancel=cancel, repository=args.repository,
                        branch=args.branch, port_source=args.port_source, accept_license=True)
        status.set('Working… (you can leave this running)')
        background(engine.execute, 'Done. Start %s from the application menu or press Play.' % TITLE)

    def do_play():
        background(lambda: play(directory.get()) and None, 'The game was closed.')
        status.set('The game is running. The first start downloads Proton (about 500 MB).')

    def do_steam():
        background(lambda: add_to_steam(directory.get()), '')

    def do_doctor():
        ok, text = doctor(directory.get())
        append(text)
        status.set('System check: %s' % ('OK' if ok else 'problems found, see above'))

    install_b = ttk.Button(controls, text='Install / Update', command=start)
    install_b.grid(row=0, column=0, columnspan=3, sticky='ew', ipady=6, pady=(0, 8))
    play_b = ttk.Button(controls, text='Play', command=do_play)
    play_b.grid(row=1, column=0, sticky='ew', padx=(0, 4))
    steam_b = ttk.Button(controls, text='Add to Steam', command=do_steam)
    steam_b.grid(row=1, column=1, sticky='ew', padx=4)
    doctor_b = ttk.Button(controls, text='Check system', command=do_doctor)
    doctor_b.grid(row=1, column=2, sticky='ew', padx=(4, 0))
    stop_b = ttk.Button(controls, text='Stop', state='disabled',
                        command=lambda: (cancel.set(), status.set('Stopping…')))
    stop_b.grid(row=2, column=0, sticky='ew', padx=(0, 4), pady=(8, 0))
    ttk.Button(controls, text='Open log', command=lambda: subprocess.Popen(
        ['xdg-open', str(Path(directory.get()).expanduser() / STATE / 'setup.log')])).grid(
        row=2, column=1, columnspan=2, sticky='ew', padx=(4, 0), pady=(8, 0))

    def poll():
        for _ in range(300):
            try:
                kind, text = events.get_nowait()
            except queue.Empty:
                break
            if kind == 'log':
                append(text)
                continue
            set_busy(False)
            status.set('Stopped: you can fix the problem and press the button again.' if kind == 'error' else
                       text.splitlines()[0] if text else 'Done.')
            if text:
                append(text)
                (messagebox.showerror if kind == 'error' else messagebox.showinfo)(TITLE, text)
        root.after(100, poll)

    def close():
        if busy['on']:
            messagebox.showinfo(TITLE, 'Press Stop and wait for it to finish first.')
        else:
            root.destroy()

    root.protocol('WM_DELETE_WINDOW', close)
    poll()
    root.mainloop()


def main():
    parser = argparse.ArgumentParser(description='%s Linux setup: build the port from your own game and play '
                                                 'it with Proton.' % TITLE)
    parser.add_argument('--dir', help='install folder (or the existing installation)')
    parser.add_argument('--iso', help='your disc image (.iso; a .dkey next to it is used)')
    parser.add_argument('--dkey', help='the disc key file of an encrypted image')
    parser.add_argument('--game-dir', help='your game folder (the one with PS3_GAME)')
    parser.add_argument('--rpcs3', help='RPCS3 (AppImage or program), to decrypt EBOOT.BIN')
    parser.add_argument('--accept-license', action='store_true',
                        help="accept Microsoft's license for the C++ headers and libraries (%s)" % MS_LICENSE_URL)
    parser.add_argument('--build-jobs', type=int, help='parallel compiler jobs (default: from RAM)')
    parser.add_argument('--cli', action='store_true', help='no window: install or update in the terminal')
    parser.add_argument('--update', action='store_true', help='update an installation (no window)')
    parser.add_argument('--play', action='store_true', help='start the game')
    parser.add_argument('--add-to-steam', action='store_true', help='add the game to Steam')
    parser.add_argument('--doctor', action='store_true', help='check this system')
    parser.add_argument('--repository', default=DEFAULT_REPO, help='GitHub owner/name of the port')
    parser.add_argument('--branch', default=DEFAULT_BRANCH)
    parser.add_argument('--port-source', help='(testing) a local checkout or tarball URL instead of GitHub')
    args = parser.parse_args()
    config = HERE.parent / 'installation.json'
    if config.is_file():
        saved = json.loads(config.read_text())
        args.repository = saved.get('repository', args.repository)
        args.branch = saved.get('branch', args.branch)
        args.dir = args.dir or saved.get('directory')

    def report(fn, *a):
        try:
            result = fn(*a)
        except (SetupError, OSError) as exc:
            print(str(exc), file=sys.stderr)
            if not sys.stdin.isatty() and shutil.which('zenity'):
                subprocess.run(['zenity', '--error', '--title', TITLE, '--text', str(exc)], check=False)
            return 1
        if isinstance(result, str):
            print(result)
            if not sys.stdin.isatty() and shutil.which('zenity'):
                subprocess.run(['zenity', '--info', '--title', TITLE, '--text', result], check=False)
        return 0

    if args.doctor:
        ok, text = doctor(args.dir)
        print(text)
        return 0 if ok else 1
    if args.play or args.add_to_steam:
        if not args.dir:
            parser.error('--play / --add-to-steam need --dir')
        return report(play if args.play else add_to_steam, args.dir)
    if args.cli or (args.update and not os.environ.get('DISPLAY') and not os.environ.get('WAYLAND_DISPLAY')):
        if not args.dir:
            parser.error('--cli needs --dir')
        if not args.accept_license and not (Path(args.dir) / STATE / 'toolchain').is_dir():
            parser.error('--cli needs --accept-license (Microsoft C++ headers and libraries: %s)' % MS_LICENSE_URL)
        engine = Engine(args.dir, args.iso or args.game_dir, dkey=args.dkey,
                        update=args.update or is_install(Path(args.dir).expanduser()), rpcs3=args.rpcs3,
                        repository=args.repository, branch=args.branch, port_source=args.port_source,
                        accept_license=True, build_jobs=args.build_jobs)
        return report(engine.execute)
    try:
        gui(args)
    except Exception as exc:   # no display, or Tk cannot open one
        if 'display' not in str(exc).lower():
            raise
        print('No window could be opened (%s). Run it in a terminal with --cli, for example:\n'
              '  %s --cli --accept-license --dir ~/Games/inFamous --iso ~/inFamous.iso' % (exc, sys.argv[0]),
              file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
