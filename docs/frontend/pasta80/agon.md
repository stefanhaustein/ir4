# Self-Hosted PASTA/80 IDE on Agon

This guide covers running the self-hosted Pascal edit-compile-run loop directly on Agon Light / Console8 or in fab-agon-emulator.

## Basic Setup

Before setting up the on-device IDE, follow [README.md](README.md) to build the host tools and verify cross-compilation for Agon with hello.pas:

```sh
./pasta84/pasta84.sh -t agon pasta84/examples/hello.pas
```

Run the resulting hello.bin in fab-agon-emulator or on Agon hardware to confirm your Agon target setup works.

## Additional Prerequisite

- [agondev](https://github.com/pcawte/agondev) (with `agondev-config` in PATH or `~/opt/agondev/bin`) — needed to cross-compile ir4c for Agon.

## 3. Build the On-Agon Toolchain

From the IR4th repository root:

```sh
# 1. On-Agon IR4(th) compiler -> build/ir4c_agon.bin
make agon

# 2. Resident $B0000 MOSlet launcher -> pasta84/ide/mcp.bin
./pasta84/pasta84.sh --release -t agon -b 0xB0000 pasta84/ide/mcp.pas

# 3. Full-screen text editor -> pasta84/ide/pano.bin
./pasta84/pasta84.sh --release -t agon pasta84/ide/pano.pas

# 4. On-Agon Pascal compiler -> pasta84/pasta80.bin
./pasta84/pasta84.sh --release -t agon pasta84/pasta80.pas
```

## Stage the Agon SD Card

Copy the binaries and runtime libraries onto your Agon SD card (or an emulator SD directory such as build/agon-sd):

```text
/mos/mcp.bin          <- pasta84/ide/mcp.bin
/bin/pano.bin         <- pasta84/ide/pano.bin
/bin/pasta84.bin      <- pasta84/pasta80.bin
/bin/ir4c.bin         <- build/ir4c_agon.bin
/pasta84/rtl/         <- pasta84/rtl/
/lib/                 <- lib/
```

For quick testing, copy pasta84/examples/ onto the SD card as well.

## Running the IDE

From the MOS prompt:

```text
mcp hello.pas
```

mcp stays resident in $B0000 MOSlet space and launches pano at $40000. Inside pano:

- **^B** — Save and compile (Pascal, IR4th, or assembly by file extension)
- **^R** — Save, compile, and run the resulting binary at $40000
- **^O** — WriteOut (save file)
- **^K / ^U** — Cut / uncut line
- **^X** — Exit pano back to MOS

If compilation fails, mcp captures the error message from the screen into PANO.ERR and re-opens pano with the cursor positioned at the error.

Note that this is still pretty slow at the moment -- we might be able to squeeze a few seconds,
but please don't expect too much.

## Test Results

Via `mcp --selftest` (or `mcp --selftest <suite>`) in `fab-agon-emulator`:

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

                           Agon ---> Agon

   Tests    Passed    Failed     Total  PASTA/80      ir4c       Run
--------  --------  --------  --------  --------  --------  --------
    core       ???       ???       ???     24.2s      1.8s       ???
    heap       ???       ???       ???      2.9s      2.0s       ???
   files       ???       ???       ???      9.6s      1.8s       ???
    more       ???       ???       ???      3.6s      1.9s       ???
  params         6         0         6      1.6s      1.8s      0.0s
   sound         4         0         4      1.6s      1.8s      4.5s
overlays       ???       ???       ???      3.4s      1.9s       ???
--------  --------  --------  --------  --------  --------  --------
   Total        10         0        10     46.9s     13.0s      4.5s

                             [  FAIL  ]
```

(`core`, `heap`, `files`, `more`, and `overlays` currently hit `ir4c: out of memory` in Agon's ~329 KB user heap while loading the full runtime.)