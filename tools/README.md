# IR4(th) Runtime & Assembly Tooling

This directory contains tooling to synchronize in-place assembly inside `.ir4` `code` blocks and generate relocatable runtime headers (`src/`).

## `sync_asm.py` — In-Place `\\` Assembly Synchronizer

Synchronizes structured `\\` assembly comments inside `.ir4` `code ... end` blocks with their left-hand machine-code byte and relocation tokens (`0xNN :i8`, `<off> :ar`, `<off> :as`, `& <sym>`) and updates the closing `end \\ <4-hex>` FNV-1a verification tag.

### Usage

```sh
python3 tools/sync_asm.py                  # sync any out-of-date lib/**/*.ir4 files
python3 tools/sync_asm.py --check          # fail with exit status 1 if any file is out of sync
python3 tools/sync_asm.py --force          # re-assemble all \\ blocks even when hashes match
```

### How Synchronization & Verification Work

- Top-level `\\ ...` lines in a `.ir4` file define file-scoped assembly equates prepended to every `code` block in that file.
- Inside `code <target> <name> ( ... ) ... end`, lines with `\\ <asm>` act as the assembly source of truth:
  - `6502` / `c64` / `sim65`, `x16` (`65C02`), and `65816` / `x16gs` / `sim65816` assemble via `ca65` + `ld65` (with `.p816`, `.smart`, `.a16`, `.i16` enabled on 65816).
  - `z80` / `cpm` / `zx81` assemble via `z80asm`.
  - `ez80` / `agon` assemble via `ez80-clang` + `ez80-ld` + `ez80-objcopy`.
- **Two-pass relocation detection:** Each `code` block is assembled at two distinct origins, stack addresses (`STACK`), and symbol bases (`var` / `data` names in the file), emitting `0xNN :i8` for fixed bytes, `<off> :ar` for block-relative 16-bit addresses, `<off> :as` for 6502 software-stack addresses (`STACK+n,X`), and `& <sym>` for `var`/`data` symbol addresses.
- **Two-layer staleness protection:**
  1. `Makefile` runs `tools/sync_asm.py --since build/.asm_sync` automatically before building or running tests.
  2. `ir4c` itself computes the same 16-bit FNV-1a mix over `\\` lines and raw code tokens and rejects any `code` block whose `end \\ <4-hex>` tag is missing or stale. When hashes match, neither `ir4c` nor `sync_asm.py` requires external assemblers to be installed.

## Floating-Point & VM Runtime Generators

Regenerate runtime headers and re-verify `.ir4` `code` libraries via `make rt` (or per-target with `make rt-z80`, `make rt-ez80`, `make rt-6502`, `make rt-rv32`, `make rt-web`):

- `z80/gen_math48.py` — `tools/z80/math48.asm` → `src/backend/z80_math48.h` (requires `z80asm`, `sjasmplus`, `pasmo`, or `zmac`)
- `ez80/gen_ez80_rt.py` — AgDev `libagon.a` → `src/backend/ez80_rt.h` (requires `ez80-none-elf-as`/`ld`/`objcopy` + `libagon.a`)
- `6502/gen_wozfp.py` — `tools/6502/wozfp.asm` → `src/backend/6502_wozfp.h` (requires `ca65` + `ld65`)
- `rv32/gen_rv32_fp.py` — `tools/rv32/rv32_fp.c` → `src/backend/rv32_fp.h` (requires `riscv64-elf-gcc` / `riscv32` toolchain)
- `web/gen_web_vm.py` — `tools/web/ir4_vm.js` → `src/plat_web_vm.h`
- `rt_util.py` — Shared relocation-diffing and C header helpers
