# Re:Chain PSP

This is the native PlayStation Portable target. It uses PSPSDK's GU, GUM,
controller, audio and I/O APIs and runs at the PSP's fixed 480x272/60 Hz
 display mode. Game assets must be extracted before packaging and copied below
`data/` in the EBOOT directory. The package step also generates the frontend
art as native `PSTX` files: 480x272, 16-bit GU 4444, with no PNG decoder work
or 1920x1080 transient allocation during PSP boot.

At startup the EBOOT requests the PSP performance profile of 333 MHz CPU and
166 MHz bus, when the firmware permits it, so the native 60 Hz frame budget is
not reduced by the default handheld clock.

Build with:

```sh
PSPDEV=/opt/pspdev sh ../scripts/build_psp.sh release
```

The official PSPDev container can be used on hosts without a native PSP
toolchain (including Apple Silicon):

```sh
docker run --platform linux/amd64 --rm \
  -v "$PWD:/work" -w /work ghcr.io/pspdev/pspdev:latest \
  sh -lc 'export PSPDEV=/usr/local/pspdev; sh scripts/build_psp.sh release'
```

The generated `EBOOT.PBP` and `rechan.elf` are written to this directory by
the PSPSDK build system.

For real-hardware USB debugging through PSPLINK, run
`sh scripts/build_psp_usb.sh` from the repository. This creates an isolated
build in `build/psp-usb` and puts the unencrypted `rechan.prx` and matching
symbol-bearing `rechan.elf` in `dist/psp-debug/host`. It preserves the tested
release package and enables normal menus, optimized code and RAM-only
telemetry. Optimizations can make source-level stepping skip or reorder lines.

Install PSPLINK on the console and keep the game's extracted assets at
`PSP/GAME/RECHAN/data`. On this Mac the PSPDEV host tools live under
`build/psp-usb-tools/pspdev`. In separate terminals run
`sh scripts/psp_usb.sh usb`, `sh scripts/psp_usb.sh shell`, then enter
`debug rechan.prx` in pspsh and run `sh scripts/psp_usb.sh gdb`.
The USB service listens on loopback and exports only the `host` output folder.
See https://pspdev.github.io/debugging.html for the upstream protocol and setup.

For a clean optimized measurement build, use `profile` instead of `release`:

```sh
PSPDEV=/opt/pspdev sh ../scripts/build_psp.sh profile
```

`profile` keeps `-O3 -DNDEBUG` and enables a fixed 600-frame rolling window.
It does not print or write during gameplay. Hold `START+SELECT` to exit; the
runtime writes `data/PSP_PERF.TXT` with average FPS, p50/p95/p99, worst frame,
frames over the approximately 16.684 ms budget and logic/draw/swap averages.

Use `soak` for a prolonged manual session. It keeps the normal menu and does
not stop after 600 frames. Both measurement modes keep statistics in RAM and
write the report only at shutdown; only `diagnostic-opt` writes checkpoints.
Memory samples include the kernel's free partition and the libc allocator's
used/free bytes and free-chunk count. These describe different memory pools;
stable kernel free space alone does not rule out a growing C++ heap.

The PSP background draws from the renderer's indexed PSX pages and CLUTs,
avoiding full RGBA page allocations under heap pressure. Page and palette
storage is reused only after the GU submission has finished reading it.
The world camera uses a 64-unit near plane for the 16-bit depth buffer;
desktop retains its existing projection. Simulation remains at 30 Hz with
interpolation: the PSP drops overdue whole ticks after a stall but retains
the fractional remainder, with at most one logic tick per presented frame.

Host regression checks include `sh scripts/test_psp_renderer.sh`,
`sh scripts/test_psp_perf.sh`, and `sh scripts/test_psp_frame_pacing.sh`.

Package an extracted asset tree with:

```sh
sh scripts/package_psp.sh bin dist/RECHAN
```

`ffmpeg` is used only by packaging to downscale the five frontend images and
write the native `data/psp/textures/frontend/*.ptex` set.
