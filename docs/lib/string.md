# string

[← Library Index](README.md) · Source: [lib/string.ir4](../../lib/string.ir4)

String operations

## Functions

- **str0>str** ( s0 :a -> :a :i ) — Unpack NUL-terminated string *s0* to (addr, len)
- **strcmp0** ( a0 :a b0 :a -> :i )

  Compare NUL-terminated strings *a0* and *b0*; returns first nonzero (*a0*[i] - *b0*[i]) or 0

- **strlen0** ( s0 :a -> :i ) — Return length in bytes of NUL-terminated string *s0*

## Platforms

### 65816

Native W65C816 NUL-terminated string primitives
