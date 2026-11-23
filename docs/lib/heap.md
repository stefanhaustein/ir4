# heap

[← Library Index](README.md) · Source: [lib/heap.ir4](../../lib/heap.ir4)

First-fit dynamic memory allocator starting at __heap_start

## Functions

- **heapalloc** ( bytes :i -> :a )

  Allocate at least *bytes* bytes on the heap; returns block address

- **heapfree** ( p :a )

  Free block *p* previously returned by heapalloc (coalesces adjacent free blocks)

- **heapreset** ( ) — Reset the heap to empty (invalidates all prior allocations)
