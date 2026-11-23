# mem

[← Library Index](README.md) · Source: [lib/mem.ir4](../../lib/mem.ir4)

Memory operations and first-fit heap allocator starting at __heap_start

## Functions

- **heap_alloc** ( bytes :i -> :a )

  Allocate at least *bytes* bytes on the heap; returns block address

- **heap_free** ( p :a )

  Free block *p* previously returned by heap_alloc (coalesces adjacent free blocks)

- **heap_reset** ( ) — Reset the heap to empty (invalidates all prior allocations)
- **memcmp** ( a :a b :a len :i -> :i )

  Compare *len* bytes at *a* and *b*; returns first nonzero (*a*[i] - *b*[i]) or 0 if equal

- **memcpy** ( dst :a src :a len :i ) — Copy *len* bytes from *src* to *dst* (overlap-safe)
- **memfill** ( dst :a len :i val :i )

  Fill *len* bytes starting at *dst* with byte value *val*
