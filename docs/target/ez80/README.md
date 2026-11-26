# eZ80 (`-t ez80`)

24-bit ADL-mode eZ80 backend (`src/backend/ez80.c`) used by `-t agon`.

## Compiler Flags

```sh
./build/ir4c -t ez80 [-b hex_addr] -o out.bin input.ir4
```

## Execution Model

- **24-bit ADL mode (`ADL = 1`):** 3-byte `:i`/`:a` slots and 6-byte `:f`/`:i32` slots.
- **Data stack:** Hardware `SP`.
- **Return stack:** Dedicated 24-bit BSS stack pointed to by a BSS `rsp` variable.
- **Locals:** Non-recursive scalar locals are placed in static BSS; recursive functions and `:bytes` locals are `SP`-allocated.

## Backend Optimizations

- **Static scalar locals:** Non-recursive scalar locals live in static BSS (`:bytes` locals remain `SP`-allocated).
- **`PUSH HL` / `POP` peephole:** Elides trailing `PUSH HL; POP HL` pairs and rewrites `PUSH HL; POP DE` to `EX DE, HL`.
- **Immediate & compare-branch lowering:** Fuses constant-operand arithmetic, comparisons, shifts, bit ops, memory accesses, static-local increments, and compare-and-branch pairs.
- **Hot `code` inlining:** `hot` `code` words up to 16 bytes are inlined directly into callers.

## Platforms

- [Agon Light / Console8 (`-t agon`)](../agon/README.md)
