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

Auto-generated from tools/6502/mem.asm by tools/asm2ir4.py (do not edit manually).

### 65816

Native W65C816 16-bit word memory operations
Uses 24-bit [dp],Y addressing (hardware bank-carry safe across 64 KB banks).
Preserves X (data stack), D, DBR, M=0, X=0; uses scratch DP $00..$0D.

### z80

Auto-generated from tools/z80/mem.asm by tools/asm2ir4.py (do not edit manually).
