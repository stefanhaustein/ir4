# 6502 (`-t 6502`)

16-bit cell / 16-bit address backend (`src/backend/6502.c`) used by `-t c64`, `-t x16`, `-t x16-cart`, and `-t sim65`.

## Compiler Flags

```sh
./build/ir4c -t 6502 [-b hex_addr] [--zp hex_addr] [--stack-zp hex_addr[:init_x]] [--no-stack-zp] -o out.bin input.ir4
```

- **`--zp <addr>`:** Base zero-page address for scratch/compiler temporaries.
- **`--stack-zp <addr[:init_x]>`:** Zero-page base for the 6502 data stack (indexed via `X`).
- **`--no-stack-zp`:** Place the data stack outside zero page.

## Execution Model

- **Data stack:** Byte-split 16-bit stack in zero page indexed by `X`.
- **Return stack:** 6502 hardware stack (`$0100–$01FF`).
- **Locals & frames:** Non-recursive scalar locals live in static RAM; recursive functions and `:bytes` locals use a downward-growing frame stack (`bfp`).
- **Float runtime:** Uses the embedded Wozniak `wozfp` floating-point runtime when floating-point words are referenced.

## Backend Optimizations

- **Static scalar locals:** Non-recursive functions place scalar locals in static RAM rather than `bfp` frames.
- **Auto compact mode:** `hot` functions emit native 6502 code while cold functions emit compact bytecode to fit larger programs in 64 KB.
- **Peepholes & immediate lowering:** Eliminates redundant `INX; INX` / `DEX; DEX` pairs around stack pops, fuses compare-and-branch, and emits immediate/static-local memory ops (`add_imm`, `cmp_imm`, `shift_imm`, `bit_imm`, `mem_imm`, `data_mem`, `local_mem`, `local_add_imm`).
- **Hot `code` inlining:** `hot` `code` words up to 16 bytes are inlined directly into callers.

## Platforms

- [Commodore 64 (`-t c64`)](../c64/README.md)
- [Commander X16 (`-t x16`, `-t x16-cart`)](../x16/README.md)
