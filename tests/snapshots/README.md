# Snapshot regression tests

`make_fixture.py` generates an original N64 bootstrap and RDP/RSP workload.
It needs no game or Nintendo IPL code. The pinned RetroArch test host is fetched on the first test build:

```sh
make -C tests/snapshots check \
  CORE=/absolute/path/to/mupen64plus_next_libretro.so
```

Build all three CPU engines, CXD4, ParaLLEl-RSP and Angrylion worker support.
On Windows use native Make, `SHELL=C:/msys64/usr/bin/sh.exe`,
`CC=C:/msys64/mingw64/bin/gcc.exe` and `PYTHON=C:/path/to/python.exe`.
Python 3 generates the fixture and runs profiles. `OUTPUT` must be new (default
`build/results`). The generator refuses to overwrite an existing ROM.

The guest consumes retained RDP fill color through a native framebuffer read,
and RSP scalar/vector/VCO state through real microcode, before replacing them.
It publishes complete double-buffered results after RDP/RSP completion. The host
compares full RAM immediately after restores, those independent expected reads,
progress and normalized RGB. Ordinary serialization is not used as an oracle.
It also reads the last presented software buffer after restoration, as a paused
frontend does; restoring VI history must not overwrite that image.

The five Angrylion profiles cover cached/pure/dynarec CPUs, both low-level RSP
engines and one/two workers. Each runs 32 retained steps with speculative depths
1/2/3/12, repeated loads and independent release. Deliberately omitting private
RDP, RSP or CPU/RAM restoration makes the respective consumer check fail.

The separate hardware suite uses the pinned proxy and a real frontend:

```sh
make -C tests/snapshots check-gpu \
  CORE=/absolute/path/to/mupen64plus_next_libretro.so \
  FRONTEND=/absolute/path/to/retroarch OUTPUT=/new/writable/path
```

It covers GLideN64 4x and ParaLLEl-RDP 2x. The guest updates narrow columns and
subpixel shaded triangles with changing colors. Fill rectangles alone bypass
ParaLLEl-RDP upscaling and cannot expose an omitted enhanced-buffer restore.
Rollback stays active through the final capture so later redraws cannot hide it.
Each profile compares full guest RAM and a nonblank final frontend screenshot
against its own uninterrupted reference. Both hardware profiles and the five
Angrylion profiles pass on native Windows x86_64/NVIDIA. These are finite
synthetic continuations; no cross-renderer or original-hardware claim follows.

These suites do not certify every private field, pending partial commands,
allocation failures, setting/device transitions, frontend scheduling or
commercial content. Linux execution is not claimed for the hardware suite.

## Test harness dependency

Only these explicit test targets fetch sources from RetroArch. Normal core
builds and gameplay do not download or use the test harness. `harness.lock.json`
pins a full commit and SHA-256 for each source file, including the matching
libretro header. The first `make harness`, `check` or `check-gpu` downloads
the sources into a shared user cache; later runs verify and reuse them.
The cache is under `%LOCALAPPDATA%/libretro/snapshot-tests` on Windows, or
`$XDG_CACHE_HOME/libretro/snapshot-tests` (`~/.cache` by default) elsewhere. Only targets supported by this core are available.

Use `OFFLINE=1` to require cached sources, or set
`TOOLS=/path/to/RetroArch/libretro-common/samples/snapshots` for local
development or an offline source checkout. `HARNESS_CACHE` selects another
cache directory. A local override intentionally bypasses the pin and fetching.
Updating the harness requires changing the revision and checksums together;
branch heads and tags are never used as dependency versions.
