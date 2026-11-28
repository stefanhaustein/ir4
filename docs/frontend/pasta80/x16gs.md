# Self-Hosted PASTA/80 on Commander X16GS

This guide covers building and running the self-hosted PASTA/80 compiler and test runner directly on the W65C816-equipped Commander X16GS or in x16emu (`-gs`).

## Basic Setup

Before setting up the on-device toolchain, follow [README.md](README.md) to build the host tools and verify cross-compilation for X16GS with hello.pas:

```sh
./pasta84/pasta84.sh -t x16gs pasta84/examples/hello.pas
```

Run the resulting hello.prg (alongside its HELLO.BIN SuperRAM payload) in x16emu with `-gs` to confirm your X16GS setup works.

## Build the On-X16GS Toolchain

From the IR4th repository root:

```sh
# 1. On-X16GS IR4(th) compiler -> build/x16gs/ir4c.prg + IR4C.BIN
make x16gs

# 2. Resident Bank $F0 launcher & selftest runner -> build/x16gs-fs/MCP.PRG + MCP.BIN
mkdir -p build/x16gs-fs
./pasta84/pasta84.sh --release -t x16gs -b 0xF01000 -o build/x16gs-fs/MCP.PRG pasta84/ide/mcp_x16gs.pas

# 3. On-X16GS Pascal compiler -> build/x16gs-fs/PASTA80.PRG + PASTA80.BIN
./pasta84/pasta84.sh --release -t x16gs -o build/x16gs-fs/PASTA80.PRG pasta84/pasta80.pas
```

## Stage the HostFS or SD Directory

Copy the compiler binaries, runtime libraries, and test suites into your X16GS filesystem directory (for example `build/x16gs-fs`):

```text
MCP.PRG, MCP.BIN          <- resident launcher at $F0:1000
PASTA80.PRG, PASTA80.BIN  <- on-device Pascal compiler at $01:1000
IR4C.PRG, IR4C.BIN        <- build/x16gs/ir4c.prg + IR4C.BIN
rtl/                      <- pasta84/rtl/
pasta84/rtl/              <- pasta84/rtl/
lib/                      <- lib/
pasta84/tests/            <- pasta84/tests/
```

## Running the Self-Test

MCP stays resident in Bank `$F0` (`$F0:1000+`, using `$00:0620..$00:06FF` in Golden RAM for its hardware return stack) while loading and running child `.PRG` binaries in Banks `$01+` (`$01:1000+`). Command-line arguments are passed via the `"IR4A"` mailbox at `$00:0500`:

- All suites: `MCP --selftest`
- Single suite: `MCP --selftest sound` (or `core`, `heap`, `files`, `more`, `params`, `overlays`)

## Test Results

Via `MCP --selftest` in `x16emu -gs -mhz 40`:

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

                          X16GS ---> X16GS

   Tests    Passed    Failed     Total  PASTA/80      ir4c       Run
--------  --------  --------  --------  --------  --------  --------
    core      1572         0      1572     63.6s     33.6s      3.7s
    heap        26         0        26      8.5s     37.3s      4.3s
   files       194         0       194     24.2s     64.3s     10.7s
    more        49         0        49      8.9s     37.4s      0.6s
  params         6         0         6      5.2s     33.0s      0.0s
   sound         4         0         4      5.1s     32.9s      4.6s
overlays        32         0        32      8.7s     38.2s      0.0s
--------  --------  --------  --------  --------  --------  --------
   Total      1883         0      1883    124.2s    276.7s     23.9s

                             [  PASS  ]
```
