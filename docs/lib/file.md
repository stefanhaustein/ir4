# file

[← Library Index](README.md) · Source: [lib/file.ir4](../../lib/file.ir4)

Portable sequential file I/O (fallback stubs + convenience wrappers)

## Functions

- **fsclose** ( fd :i -> :i )

  Close file descriptor fd; returns 0 on success or < 0 on error

- **fsgetc** ( fd :i -> :i ) — Read a single byte from fd; returns 0..255 or -1 on EOF/error
- **fsmode_read** ( -> :i )

- **fsmode_write** ( -> :i )

- **fsopen** ( path :a len :i mode :i -> :i )

  Open file at (path, len) with mode (fsmode_read or fsmode_write); returns fd >= 0 or < 0 on error

- **fsopen0** ( path0 :a mode :i -> :i )

  Open NUL-terminated path0 with mode; returns fd >= 0 or < 0 on error

- **fsputc** ( fd :i c :i -> :i )

  Write a single byte c to fd; returns 0 on success or < 0 on error

- **fsread** ( fd :i buf :a len :i -> :i )

  Read up to len bytes from fd into buf; returns bytes read (0 on EOF) or < 0 on error

- **fswrite** ( fd :i buf :a len :i -> :i )

  Write len bytes from buf to fd; returns bytes written or < 0 on error

## Platforms

### cpm

CP/M BDOS sequential file I/O (4 static FCB + 128-byte DMA slots)
