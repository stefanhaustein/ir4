# memory

[← Library Index](README.md) · Source: [lib/memory.ir4](../../lib/memory.ir4)

Byte-slice memory operations

## Functions

- **memcmp** ( a :a b :a len :i -> :i )

  Compare *len* bytes at *a* and *b*; returns first nonzero (*a*[i] - *b*[i]) or 0 if equal

- **memcpy** ( dst :a src :a len :i ) — Copy *len* bytes from *src* to *dst* (overlap-safe)
- **memfill** ( dst :a len :i val :i )

  Fill *len* bytes starting at *dst* with byte value *val*

## Platform Specifics

### 6502

6502 memory primitives for IR4th.
Preserves X (IR4th data stack pointer). Uses ZP $02..$08.

### 65816

Native W65C816 16-bit word memory operations
Uses 24-bit [dp],Y addressing (hardware bank-carry safe across 64 KB banks).
Preserves X (data stack), D, DBR, M=0, X=0; uses scratch DP $00..$0D.

### z80

Z80 memory primitives for IR4th.
Uses IY for return address; zero relocations.
