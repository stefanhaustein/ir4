# memory

[← Library Index](README.md) · Source: [lib/memory.ir4](../../lib/memory.ir4)

Byte-slice memory operations

## Functions

- **memcmp** ( a :a b :a len :i -> :i )

  Compare *len* bytes at *a* and *b*; returns first nonzero (*a*[i] - *b*[i]) or 0 if equal

- **memcpy** ( dst :a src :a len :i ) — Copy *len* bytes from *src* to *dst* (overlap-safe)
- **memfill** ( dst :a len :i val :i )

  Fill *len* bytes starting at *dst* with byte value *val*

## Platforms

### 6502

Auto-generated from tools/6502/mem.asm by tools/asm2ir4.py (do not edit manually).

### z80

Auto-generated from tools/z80/mem.asm by tools/asm2ir4.py (do not edit manually).
