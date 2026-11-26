# Z80 (`-t z80`)

16-bit cell / 16-bit address Z80 backend (`src/backend/z80.c`) used by `-t cpm` and `-t zx81`.

## Compiler Flags

```sh
./build/ir4c -t z80 [-b hex_addr] -o out.bin input.ir4
```

## Execution Model

- **Data stack:** Hardware `SP` (2-byte `:i`/`:a` slots, 4-byte `:f`/`:i32` slots).
- **Return stack:** Dedicated BSS return stack (leaving `IX`/`IY` untouched).
- **Locals:** Non-recursive scalar locals are placed in static RAM; recursive functions and `:bytes` locals are `SP`-allocated.
- **Float runtime:** Uses the embedded `math48` floating-point runtime when floating-point words are referenced.

## Backend Optimizations

- **Static scalar locals:** Non-recursive scalar locals are placed in static RAM (`:bytes` locals remain `SP`-allocated).
- **`PUSH HL` / `POP` peephole:** Elides trailing `PUSH HL; POP HL` pairs and rewrites `PUSH HL; POP DE` to `EX DE, HL`.
- **Immediate & compare-branch lowering:** Fuses constant-operand arithmetic, comparisons, shifts, bit ops, memory accesses, static-local increments, and compare-and-branch pairs.
- **Hot `code` inlining:** `hot` `code` words up to 16 bytes are inlined directly into callers.

## Platforms

- [CP/M (`-t cpm`)](../cpm/README.md)
- [Sinclair ZX81 (`-t zx81`)](../zx81/README.md)
