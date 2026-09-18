# Building ReChan

ReChan supports 64-bit x86 (`x86_64`) Windows and Linux, and Apple Silicon
(`arm64`) macOS. A 32-bit build is not supported.

## Clone the repository

Install Git, open a terminal, and run:

```bash
git clone https://github.com/SilverwireGames/ReChan.git
cd rechan
```

## Windows

### Requirements

- Windows 10 or newer
- Visual Studio 2022 or newer with **Desktop development with C++**
- A 64-bit Windows installation

Generate the Visual Studio project from a Developer PowerShell prompt:

```powershell
.\premake5.cmd vs2022
```

Open `build/rechan.sln`, select **Release** and **x64**, then build the `rechan`
project. The executable is written to `bin/rechan.exe`.

## Linux

The Linux helper generates GNU Make files with Premake and builds the selected
configuration. It accepts `release` (recommended) or `debug`:

```bash
sh scripts/build_linux.sh release
```

The executable is written to `bin/rechan`.

### Arch Linux and Garuda Linux

```bash
sudo pacman -S --needed base-devel premake mesa sdl2-compat libx11 \
  libxcursor libxi libxinerama libxrandr
sh scripts/build_linux.sh release
```

### Ubuntu and Debian

Install the dependencies and Premake 5, then build:

```bash
sudo apt update
sudo apt install build-essential libgl1-mesa-dev libsdl2-dev libx11-dev \
  libxcursor-dev libxi-dev libxinerama-dev libxrandr-dev
sh scripts/build_linux.sh release
```

If Premake is not available from your distribution, download Premake 5 and
place the `premake5` executable either in your `PATH` or in the repository root.

## macOS (Apple Silicon)

### Requirements

- macOS 11 or newer on an Apple Silicon (arm64) Mac
- Xcode Command Line Tools (`xcode-select --install`)
- Premake 5 (`brew install premake`)

The macOS helper generates GNU Make files with Premake and builds the selected
configuration (`release` recommended, or `debug`/`shipping`):

```bash
sh scripts/build_macos.sh release
```

The executable is written to `bin/rechan`. macOS uses a native Metal rendering
backend.

## Run the game

Create a `discimage` folder beside the built executable and place exactly one
legally obtained NTSC-U (`SLUS-00684`) `.bin` or `.iso` image inside it:

```text
bin/
|-- rechan.exe       # Windows (use "rechan" on Linux and macOS)
`-- discimage/
    `-- game.bin
```

Run rechan from that directory. On first launch, its built-in extractor verifies
the disc and prepares the required assets. PAL and other regional releases are
not supported.

On Linux, make the executable runnable if necessary:

```bash
chmod +x bin/rechan
cd bin
./rechan
```

## Clean and rebuild

Generated projects and intermediate files are stored under `build`. To force a
clean Linux rebuild:

```bash
rm -rf build
sh scripts/build_linux.sh release
```

## PSP (PSPSDK)

The PSP target is a separate native build. Install the PSPDEV toolchain and
export `PSPDEV`, then run from the repository root:

```sh
PSPDEV=/opt/pspdev sh scripts/build_psp.sh release
```

The target uses PSPSDK's GU/GUM, controller, audio and `sceIo` APIs, with a
fixed 480×272 framebuffer at 60 Hz. Place the pre-extracted game data below
`data/` beside `EBOOT.PBP` before launching on hardware or PPSSPP. The command produces `psp/rechan.elf` and
`psp/EBOOT.PBP` when PSPSDK is available.

On hosts without a native PSPDEV installation, use the official PSPDev
container:

```sh
docker run --platform linux/amd64 --rm \
  -v "$PWD:/work" -w /work ghcr.io/pspdev/pspdev:latest \
  sh -lc 'export PSPDEV=/usr/local/pspdev; sh scripts/build_psp.sh release'
```
