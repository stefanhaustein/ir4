# IR4(th) as Backend for Pasta80

[PASTA/84](https://github.com/stefanhaustein/pasta84/tree/pasta84) is a fork of [PASTA/80](https://github.com/pleumann/pasta80) that replaces the Z80/eZ80 code generator with an **IR4(th)** backend. Because `pasta80` emits target-independent `.ir4` source files, a single Pascal build can be compiled by `ir4c` for Commodore 64, Commander X16, Agon Light, CP/M, ZX81, ARM64, RISC-V, and the bytecode VM.

## 1. Prerequisites (Linux / macOS)

- C89-capable C compiler (`clang` or `gcc`) and `make` (for `ir4c`)
- [Free Pascal (`fpc`)](https://www.freepascal.org/) (for building the host `pasta80` compiler)
- Optional emulators for running target binaries:
  - **C64:** [VICE](https://vice-emu.sourceforge.io/) (`x64sc`)
  - **Commander X16:** [x16emu](https://github.com/X16Community/x16-emulator)
  - **Other targets:** `fab-agon-emulator` (Agon), `tnylpo` (CP/M), `sim65` / `qemu-system-riscv32`

## 2. Clone and Build

Clone IR4(th) and check out the `pasta84` branch inside it:

> **Note on the nested layout:** `pasta84/` is an independent Git repository (an IR4(th)-specific branch of PASTA/80) placed inside the `IR4th` checkout and listed in `IR4th/.gitignore`. Keeping it nested here lets both repositories stay separate in Git while keeping all relative paths (`./build/ir4c`, `-I pasta84/rtl`, and `pasta84/examples/...`) short without requiring system-wide installation.

```sh
git clone https://github.com/stefanhaustein/IR4th.git
cd IR4th
make

git clone -b pasta84 https://github.com/stefanhaustein/pasta84.git
(cd pasta84 && fpc pasta80.pas)
```

This produces:
- `build/ir4c` — the IR4(th) compiler
- `pasta84/pasta80` — the Pascal-to-IR4(th) frontend
- `pasta84/pasta84.sh` — wrapper script that runs `pasta80` and `ir4c` in one step

## 3. Compiling Pascal Programs

Use `pasta84/pasta84.sh` to compile a `.pas` file to a target binary in one step (it emits `<program>.ir4` via `pasta80`, invokes `build/ir4c -I pasta84/rtl` with your target flags, and defaults `-o` to `<program>.<ext>` when omitted):

### Commodore 64 (`c64`)

Compile to `hello.prg` (BASIC stub at `$0801`):

```sh
./pasta84/pasta84.sh -t c64 pasta84/examples/hello.pas
```

Run in VICE:

```sh
x64sc -autostartprgmode 1 pasta84/examples/hello.prg
```

For graphics or larger programs (such as `pasta84/examples/mandelbrot.pas` or `pasta84/examples/hat.pas`), the exact same flags apply:

```sh
./pasta84/pasta84.sh -t c64 pasta84/examples/mandelbrot.pas
x64sc -autostartprgmode 1 pasta84/examples/mandelbrot.prg
```

### Commander X16 (`x16`)

Compile to `hello.prg` (BASIC stub at `$0801`):

```sh
./pasta84/pasta84.sh -t x16 pasta84/examples/hello.pas
```

Run in `x16emu`:

```sh
x16emu -prg pasta84/examples/hello.prg -run
```

If you need a cartridge ROM image instead of a `.prg`, use `-t x16-cart`.

*(If you prefer to invoke the two stages manually: `./pasta84/pasta80 <program>.pas` followed by `./build/ir4c -I pasta84/rtl -t <target> -o <output> <program>.ir4`.)*

## 4. Other Targets & Fast Host Testing

`pasta80` emits the same `.ir4` file regardless of target, and `pasta84.sh` forwards any `ir4c` target flags:

| Target | `ir4c` Flag | Output | Notes |
| --- | --- | --- | --- |
| ARM64 JIT (macOS / Linux) | `-t arm64 --run` | Native JIT | Fast host testing without an emulator |
| Agon Light / Console8 | `-t agon` | `.bin` (`0x040000`) | MOS ADL 24-bit executable |
| CP/M-80 | `-t cpm` | `.com` (`0x0100`) | Run with `tnylpo` or on CP/M hardware |
| ZX81 | `-t zx81` | `.p` | 32×22 display |
| Commander X16GS | `-t x16gs` | `.bin` | 24-bit W65C816 |
| QEMU RISC-V `virt` | `-t virt` | `.elf` | Run with `qemu-system-riscv32` |
| Bytecode VM | `-t bc --run` | `.ir4b` | Portable bytecode interpreter |

Example host test run:

```sh
./pasta84/pasta84.sh -t arm64 --run pasta84/examples/fibo.pas
```

## 5. Compiler Options

`pasta84/pasta80` accepts:

- `--release` — strips `Assert(...)` checks and breakpoints from the generated `.ir4`
- `--version` — prints the compiler version

For `ir4c`, `-O1` (constant folding, immediate lowering, static locals on 8-bit/eZ80, and `PUSH`/`POP` peepholes) is enabled by default; pass `-O0` to disable optimizations if debugging generated code.
