# Linux (native)

This is the native POSIX build of the toolkit and the port. It is the first
stage of Linux support and is deliberately headless:

- `ps3recomp` (the runtime library) builds and passes its tests with GCC and
  Clang.
- `ps3recomp_host` drives the `cellGcm` → RSX → backend bridge with no lifted
  game. On Linux the backend is still the null/headless software path.
- The port's runner (`infamous-recomp/main.cpp` + `stubs.cpp` + the PPU
  scaffold) builds and boots the toolkit's synthetic smoke title end to end.

It does **not** render a retail game yet. The Windows D3D12 backend is not
available on Linux; the native Vulkan RSX backend is the next stage. Until it
lands, a real game boot on Linux stops at a headless frame, and the port's
Windows-only UI overlay/trainer are compiled out.

## Requirements

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build libsdl2-dev
# Clang instead of GCC:
sudo apt install clang
```

SDL2 provides `cellPad` and `cellAudio` off Windows. No sudo is needed to build
or run.

## Build and test the toolkit

```sh
cmake -S ps3recomp -B build-runtime -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS3RECOMP_BUILD_TESTS=ON
cmake --build build-runtime --parallel
ctest --test-dir build-runtime --output-on-failure

./build-runtime/ps3recomp_host
./build-runtime/ps3recomp_boot_smoke build-runtime/smoke/smoke.elf
```

The last two commands should print `PASS` (the host harness checks the clear
colour and the presented pixel; the smoke title checks its 11 guest markers and
`sys_process_exit`).

## Build the port against the smoke lift

The port normally builds its lifted game code from your own dump. For a
compile/link check without game data, point it at the smoke lift written by the
runtime configure:

```sh
cmake -S infamous-recomp -B build-port -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPS3RECOMP_DIR="$PWD/ps3recomp" \
  -DRECOMP_DIR="$PWD/build-runtime/smoke/lift" \
  -DPORT_ENABLE_SPU=OFF \
  -DPORT_BUILD_TRAINER=OFF \
  -DPORT_PPU_REGCACHE=OFF \
  -DPORT_PGO=OFF \
  -DPORT_CPU_AUTO=OFF
cmake --build build-port --parallel
timeout 60 ./build-port/inFamousRecomp build-runtime/smoke/smoke.elf
```

## Building the real game (current state)

The installer pipeline is cross-platform in principle (`configure_unix` /
`compile_unix` in `installer/infamous.toml`), but the Linux-native build is not
packaged yet. On Linux the supported way to produce the Windows binary today is
the Proton setup in the separate Linux fork/PR; this branch is the native
runtime work.

Once the Vulkan backend lands, the real-game build will be the same
`infamous-installer` pipeline with the Unix configure step, plus a Vulkan
runtime. The dump used for development is the European release
(`BCES00220`).

## What is different on Linux

- The Win32 UI overlay (`rsx_ui_overlay.c`), the F10 trainer page and the raw
  mouse path are Windows-only. The game's own menus, the SDL2 controller and
  SDL2 audio work.
- Native save/message dialogs fall back to the non-interactive behaviour
  already used by the runtime off Windows.
- The runtime's write-watch vertex cache is inert; the D3D12 backend owns it.
- `ps3_plug_lookup` has a weak fallback so a build without the WWS plugin lift
  still links.
