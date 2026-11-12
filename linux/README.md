# inFamous on Linux

The Linux setup builds the inFamous PC port on Linux from **your own** European disc (BCES00220) and
runs it with Proton. It builds the same Windows game as the Windows setup does: the same
recompiled code, the same clang-cl 19 and the same PGO profile. Proton's vkd3d-proton then runs
its Direct3D 12 renderer on Vulkan. There is no separate Linux renderer to fall behind the Windows one.

Nothing is installed system-wide and the setup never asks for `sudo`. Everything it downloads
goes into the install folder (`<folder>/.linux`): a private Python, Rust, LLVM, CMake, Ninja,
Microsoft's C++ headers and libraries, the Proton launcher and the Proton prefix.

## Install

1. Download `inFamous-Linux-Setup.run` from the Releases page.
2. Make it executable (file manager: Properties > Permissions > "Allow executing", or
   `chmod +x inFamous-Linux-Setup.run`) and open it.
3. Choose an install folder, then your disc image (`.iso`; an encrypted image works with its
   `.dkey` next to it) or your game folder (the one with `PS3_GAME`). Tick the Microsoft license
   box and press **Install / Update**.

The first install downloads about 3 GB and builds for an hour or more. It needs about 40 GB of
free space and 16 GB of RAM (it works with less RAM, just more slowly). Afterwards, start the
game with **inFamous** in your application menu. On the first start Proton downloads itself
(GE-Proton, about 500 MB).

Without a desktop, or over SSH:

```sh
./inFamous-Linux-Setup.run --cli --accept-license --dir ~/Games/inFamous --iso ~/inFamous.iso
./inFamous-Linux-Setup.run --play --dir ~/Games/inFamous
./inFamous-Linux-Setup.run --doctor --dir ~/Games/inFamous      # checks this system
```

**Updating:** use **Update inFamous** in the application menu. It downloads only the files that
changed and recompiles only what they affect.

## What each kind of distribution needs

The setup checks all of this itself and says what to do. In short:

| Distribution | Notes |
|---|---|
| Ubuntu, Debian, Mint, Pop!_OS, elementary, Zorin | Works as is. With no `python3-tk`, the setup fetches its own Python with Tk; you don't need to install it. Ubuntu 24.04+: see below. |
| Fedora, Nobara, RHEL/Alma/Rocky 9+ | Works as is. SELinux needs no changes. RHEL/Alma/Rocky 8 work too (glibc 2.28). |
| Arch, Manjaro, EndeavourOS, CachyOS, Garuda | Works as is. |
| openSUSE Tumbleweed/Leap, Slackware, Gentoo, Solus, Mageia, PCLinuxOS | Works as is. |
| SteamOS / Steam Deck | Run the setup in Desktop Mode, then press **Add to Steam** and pick Proton in the game's Compatibility settings. It then works in Game Mode. |
| Bazzite, ChimeraOS, Fedora Silverblue/Kinoite, Aurora, Bluefin, openSUSE Aeon/Kalpa/MicroOS, Vanilla OS, blendOS | Works: nothing goes into the read-only system. Install into your home folder or another writable drive. |
| NixOS | Turn on `programs.nix-ld.enable = true;` (the portable LLVM needs the standard loader), or run the setup inside `steam-run` or a distrobox. |
| Alpine, Void (musl), Chimera, other musl systems | Not supported directly (the toolchain needs glibc). Use `distrobox create -i archlinux` and run the setup inside it. |
| Guix | Use a glibc FHS container (`guix shell --container --emulate-fhs`) or distrobox. |
| ARM (Asahi, Raspberry Pi, ...) | Not supported: the recompiled game is x86-64 code. |
| WSL | The build works, but play the Windows release of the port on Windows. |

Every distribution also needs:

* **A Vulkan driver.** Mesa's RADV for AMD, ANV for Intel Arc / Xe, or the NVIDIA driver (535 or
  newer). The package is called `mesa-vulkan-drivers` (Debian/Ubuntu), `mesa-vulkan-drivers` /
  `vulkan-loader` (Fedora), `vulkan-radeon` / `vulkan-intel` / `nvidia-utils` (Arch) or `libvulkan_radeon` (openSUSE).
  The GPU needs Direct3D 12 feature level 12_0 through vkd3d-proton: AMD Polaris (RX 400) or
  newer, NVIDIA Maxwell (GTX 900) or newer, or Intel Arc / Xe. `--doctor` lists the Vulkan
  drivers it finds.
* **A Linux file system** for the install folder: ext4, Btrfs, XFS, F2FS, ZFS, bcachefs and so on.
  NTFS, exFAT and FAT drives are refused, because the build tools and Proton need Unix
  permissions, symbolic links and case-sensitive names. A drive mounted `noexec` is refused too.
* **Optionally, DejaVu Sans** (`fonts-dejavu-core`, `dejavu-sans-fonts`, `ttf-dejavu`). It's used
  to draw the keyboard/mouse button prompts; without it the game uses the controller prompts.
* **Optionally, RPCS3.** It's only needed when your dump has an encrypted `EBOOT.BIN` and no
  `EBOOT.elf`. The setup finds the Flatpak (`net.rpcs3.RPCS3`), an AppImage in `~/Applications`,
  `~/Downloads`, `~/.local/bin` or `~/Games`, or `rpcs3` on your PATH. AppImages work without
  FUSE. A disc image's `.dkey` only decrypts the image itself: the `EBOOT.BIN` inside it still
  needs RPCS3 the first time.

### Ubuntu 24.04 and newer: AppArmor and user namespaces

Proton runs inside the Steam Linux Runtime, which needs unprivileged user namespaces. Ubuntu 24.04
restricts those unless an AppArmor profile allows them. If the game doesn't start and `play.log`
mentions `bwrap` / `setting up uid map`, install Steam from Ubuntu's repository (its package brings
the profile), or allow it once:

```sh
sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0   # until the next reboot
```

### Steam (any distribution)

**Add to Steam** in the setup window adds the game as a non-Steam game. Then, in its Properties >
Compatibility, choose Proton 9 or newer (or GE-Proton). No launch options are needed: the
launcher sets the shader-compiler override for the game itself.

Flatpak Steam can't be asked to add a game, so the setup shows the steps instead. If your install
folder is outside your home folder, it also gives Flatpak Steam access to that folder.

### DLSS, FSR

FSR works on every GPU. For DLSS on NVIDIA, the setup sets `PROTON_ENABLE_NVAPI=1`. To build
either one in, put the SDKs in the install folder as the main README describes, then run **Update
inFamous**.

## How it works

| Step | What happens |
|---|---|
| Private environment | micromamba (SHA-256 pinned) installs Python 3.12, Rust, a linker, curl, unzip and 7-Zip from conda-forge into `.linux/env`. |
| Installer | `installer/` is built with `cargo --no-default-features`: just the command-line installer, with no window library to download. |
| Toolchain | LLVM 19.1.5, CMake 3.31.6, Ninja 1.13.2 and xwin 0.10.0, each SHA-256 pinned. xwin downloads MSVC 14.44 and Windows SDK 10.0.26100 after you accept Microsoft's license. A case-insensitive overlay lets clang-cl and lld-link find `<WinDef.h>` / `Synchronization.lib` on a case-sensitive drive. |
| Game code | `infamous-installer install --cross .linux/toolchain/windows-x64.cmake` runs the same pipeline as on Windows. The Python tools write files exactly as they would on Windows (CRLF text, NTFS file order; see `installer/src/winlike/`), so the result matches the maintainer's `generated.lock` byte for byte, and the port's patches apply. |
| Build | The same CMake settings as on Windows, with clang-cl. The C/C++ runtime is linked in statically (`/MT`), so Proton's reimplemented `vcruntime140`/`msvcp140` aren't involved. CMake's CPU probe can't run in a cross build, so the installer does the same AVX2/FMA/BMI2 test and builds the compatibility version when the CPU needs it. |
| Shader compiler | The game compiles its shaders at run time with `D3DCompile`. Microsoft's `d3dcompiler_47.dll` (from the Windows SDK cab winetricks uses, SHA-256 pinned) goes next to the game, and the launcher prefers it over Wine's. |
| Running | `inFamous.exe` (the port's launcher, with its options window) runs through umu-launcher with GE-Proton and a prefix in `.linux/prefix`. On SteamOS it runs through Steam. |

Logs: `.linux/setup.log` for the setup and build, `.linux/play.log` for Proton, and
`infamous-recomp/play.out.log` / `play.err.log` for the game itself.

## Maintainers

```sh
linux/package.sh      # -> dist/inFamous-Linux-Setup.run (installs and updates from whompay/inFAMOUSreborn, main)
```

The Linux changes to the installer are kept apart: `--cross`, `installer/src/winlike/` and the
line-ending check in `apply_lock`. Windows builds don't use any of them.
