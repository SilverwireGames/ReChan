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

## PSP real, PSPLINK e benchmark

Aplicativos necessários no computador:

- PSPSDK/PSPDev, instalado localmente ou pela imagem Docker
  `ghcr.io/pspdev/pspdev:latest`;
- Docker Desktop em Apple Silicon, usando `--platform linux/amd64`;
- ferramentas host do PSPLINK: `usbhostfs_pc`, `pspsh`, `psp-gdb` e
  `psp-nm`;
- `ssh`/`scp` somente quando a compilação for delegada ao servidor remoto.

No PSP são necessários um firmware com PSPLINK/Bootstrap funcional, cabo USB
de dados e os assets extraídos em `ms0:/PSP/GAME/RECHAN/data`. Os assets e a
imagem do jogo continuam sendo fornecidos somente pelo usuário.

### Build USB local

O build de teste é isolado em `build/psp-usb` e publica somente a versão mais
recente em `dist/psp-debug/host`:

```sh
PSP_BUILD_JOBS=10 sh scripts/build_psp_usb.sh
```

O diretório de host contém `EBOOT.PBP`, `rechan.prx` e `rechan.elf`. O `EBOOT`
é copiado pelo próprio script para não reaproveitar um pacote antigo. A build
USB usa `-O3 -DNDEBUG`, `PSP_AUTO_START_PHASE1`, telemetria em RAM e o caminho
VFPU GUM; ela é para diagnóstico/performance, não para distribuição final.

### Build no servidor remoto

Quando o host PSPDev remoto estiver disponível, envie somente o commit atual
para um diretório temporário, compile com Docker e copie os três artefatos por
SCP. Não documentar senhas no repositório; use chave SSH ou solicitação de
senha do próprio `ssh`:

```sh
REMOTE=dev@192.168.18.24
REMOTE_ROOT=/tmp/rechan-psp-build

git archive --format=tar HEAD | ssh "$REMOTE" \
  "mkdir -p '$REMOTE_ROOT' && tar -xf - -C '$REMOTE_ROOT'"
ssh "$REMOTE" "rm -f '$REMOTE_ROOT/src/gen/psprender.cpp' '$REMOTE_ROOT/src/gen/psprender.h' '$REMOTE_ROOT/src/gen/psprender.o'"
ssh "$REMOTE" "docker run --rm --platform linux/amd64 -e PSP_BUILD_JOBS=12 -v '$REMOTE_ROOT:/work' -w /work/psp ghcr.io/pspdev/pspdev:latest sh -c 'export PSPDEV=/usr/local/pspdev PSPSDK=/usr/local/pspdev/psp/sdk PATH=/usr/local/pspdev/bin:\$PATH; make -B -j12 BUILD_MODE=usb-debug BUILD_PRX=1 DEFINES=\"-DRC_PLATFORM_PSP -DPSP -DPSP_GE -DIMGUI_DISABLE -DPSP_ENABLE_NATIVE_INDEXED -DPSP_INDEXED_LIST_MEMORY -DPSP_INDEXED_PERSISTENT_INDICES -DPSP_SORT_OPAQUE_MATERIALS -DPSP_AUTO_START_PHASE1\"'"
scp "$REMOTE:$REMOTE_ROOT/psp/EBOOT.PBP" "$REMOTE:$REMOTE_ROOT/psp/rechan.prx" "$REMOTE:$REMOTE_ROOT/psp/rechan.elf" dist/psp-debug/host/
shasum -a 256 dist/psp-debug/host/EBOOT.PBP dist/psp-debug/host/rechan.prx dist/psp-debug/host/rechan.elf
```

O host remoto validado fica em uma rede local; a senha nunca deve ser salva
em scripts, README ou contexto.

### Conexão e execução no PSPLink

No diretório raiz do projeto, abra três terminais:

```sh
# Terminal 1 — manter ativo; aguardar “Found Sony PSP device” e “Connected”
sh scripts/psp_usb.sh usb

# Terminal 2 — comandos PSPLINK
sh scripts/psp_usb.sh shell

# Terminal 3 — somente se for depurar com GDB
sh scripts/psp_usb.sh gdb
```

No shell `pspsh`, valide a ponte e inicie o PRX que está exposto pelo
`host0:`:

```text
usbstat
ls host0:/
modlist
ldstart host0:/rechan.prx
scrshot host0:/psp-screen.bmp
```

`ldstart host0:/rechan.prx` é o procedimento validado para iniciar o jogo no
Bootstrap atual. `modexec host0:/EBOOT.PBP` pode retornar sem iniciar o módulo
nesse ambiente; o EBOOT continua sendo o pacote para instalação normal no
Memory Stick.

Para depuração, antes de abrir o terceiro terminal execute no segundo:

```text
debug rechan.prx
```

No GDB, use `continue`. Não pressione `Ctrl+C` durante uma coleta: nesta
configuração isso já provocou uma exceção no thread GDBServer do PSPLINK,
embora o jogo continuasse executando.

### Relatórios e screenshots

O benchmark não desenha overlay. Após a janela de 600 frames, os relatórios
são gravados no Memory Stick em `ms0:/PSP/GAME/RECHAN/data/`:

```text
PSP_PERF_S01_L01.TXT ... PSP_PERF_S07_L07.TXT
```

Eles contêm FPS médio, p50/p95/p99, pior frame, frames acima de 16.684 ms,
lógica, desenho, swap, draws, vértices, misses de página, RAM, VRAM e áudio.
Para copiar um relatório do PSP para a pasta exposta pelo USBHostFS:

```text
cp ms0:/PSP/GAME/RECHAN/data/PSP_PERF_S01_L01.TXT host0:/PSP_PERF_S01_L01_CURRENT.TXT
```

O arquivo aparecerá em `dist/psp-debug/host/`. `host0:` é apenas a pasta
servida pelo `usbhostfs_pc`; o renderer grava os relatórios de gameplay em
`ms0:`. Não atribuir todo o tempo de `draw` à GPU: no PSP ele inclui CPU,
submissão e esperas internas da GU.

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
