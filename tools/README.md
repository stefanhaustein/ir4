# IR4(th) Runtime & Assembly Tooling

This directory contains generators that convert assembly (and C soft-float) sources into IR4(th) libraries (`code ... end` in `lib/`) and relocatable runtime headers (`src/`).

## `asm2ir4.py` — Assembly to `.ir4` `code` Libraries

Converts annotated 6502 (`ca65`/`ld65`) or Z80 (`z80asm`) assembly files into `lib/<cpu>/*.ir4` files containing `code ... end` words, which are individually subject to dead-code elimination.

### Usage

```sh
python3 tools/asm2ir4.py --cpu 6502 --input tools/6502/i32.asm --output lib/6502/i32.ir4
python3 tools/asm2ir4.py --cpu z80  --input tools/z80/i32.asm  --output lib/z80/i32.ir4
```

### Assembly Format

- Lines before the first `; @code` marker are shared header lines (e.g. zero-page equates) prepended to every slice.
- Each `; @code <name> ( <inputs> -> <outputs> )` marker starts an independent `code` word slice.
- **How relocations are detected (2-pass diff):**
  - Pass 1 assembles at `ORG = $4000` (with `-D STACK=$8000` on 6502).
  - Pass 2 assembles at `ORG = $4400` (with `-D STACK=$8800` on 6502).
  - High-byte diff `+4` emits `<offset> :ar` (16-bit address relative to the `code` block start).
  - High-byte diff `+8` emits `<offset> :as` (16-bit address relative to the 6502 data stack buffer `STACK`).
  - Unchanged bytes emit `0xNN :i8`.

## Floating-Point Runtime Generators

Regenerate all generated runtimes and `.ir4` `code` libraries via `make rt` (or per-target with `make rt-z80`, `make rt-ez80`, `make rt-6502`, `make rt-rv32`):

- `z80/gen_math48.py` — `tools/z80/math48.asm` → `src/be_z80_math48.h` (requires `z80asm`, `sjasmplus`, `pasmo`, or `zmac`)
- `ez80/gen_ez80_rt.py` — AgDev `libagon.a` → `src/be_ez80_rt.h` (requires `ez80-none-elf-as`/`ld`/`objcopy` + `libagon.a`)
- `6502/gen_wozfp.py` — `tools/6502/wozfp.asm` → `src/be_6502_wozfp.h` (requires `ca65` + `ld65`)
- `rv32/gen_rv32_fp.py` — `tools/rv32/rv32_fp.c` → `src/be_rv32_fp.h` (requires `riscv64-elf-gcc` / `riscv32` toolchain)
- `rt_util.py` — Shared relocation-diffing and C header helpers
