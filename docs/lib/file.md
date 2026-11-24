# file

[← Library Index](README.md) · Source: [lib/file.ir4](../../lib/file.ir4)

Sequential file I/O (fallback stubs + convenience wrappers)

## Functions

- **fsattr_dir** ( -> :i )

- **fsattr_readonly** ( -> :i )

- **fsclose** ( fd :i -> :i )

  Close file descriptor fd; returns 0 on success or < 0 on error

- **fsdelete** ( path :a len :i -> :i )

  Delete file at (path, len); returns 0 on success or < 0 on error

- **fsdelete0** ( path0 :a -> :i )

  Delete file at NUL-terminated path0; returns 0 on success or < 0 on error

- **fsdirclose** ( dd :i -> :i )

  Close directory descriptor dd; returns 0 on success or < 0 on error

- **fsdiropen** ( path :a len :i -> :i )

  Open directory at (path, len) for iteration; returns descriptor dd >= 0 or < 0 on error

- **fsdiropen0** ( path0 :a -> :i )

  Open directory at NUL-terminated path0; returns dd >= 0 or < 0 on error

- **fsdirread** ( dd :i buf :a max :i -> :i :i :i )

  Read next directory entry into buf (up to max bytes); returns (namelen, flags, size) with namelen=0 at end or <0 on error

- **fsflush** ( fd :i -> :i )

  Flush buffered writes on fd; returns 0 on success or < 0 on error

- **fsgetc** ( fd :i -> :i ) — Read a single byte from fd; returns 0..255 or -1 on EOF/error
- **fsgranularity** ( -> :i )

  Byte granularity of fssize (1 on byte-exact filesystems, 128 on CP/M, 254 on C64 1541)
  - *c64:* CBM 1541 DOS reports directory sizes in 254-byte blocks
  - *cpm:* CP/M BDOS stores file lengths in 128-byte records
  - *x16:* CMDR-DOS $=L provides exact 1-byte file sizes

- **fsmkdir** ( path :a len :i -> :i )

  Create directory at (path, len); returns 0 on success or < 0 on error

- **fsmkdir0** ( path0 :a -> :i )

  Create directory at NUL-terminated path0; returns 0 on success or < 0 on error

- **fsmode_append** ( -> :i )

- **fsmode_read** ( -> :i )

- **fsmode_readwrite** ( -> :i )

- **fsmode_write** ( -> :i )

- **fsopen** ( path :a len :i mode :i -> :i )

  Open file at (path, len) with mode (fsmode_read..fsmode_readwrite); returns fd >= 0 or < 0 on error

- **fsopen0** ( path0 :a mode :i -> :i )

  Open NUL-terminated path0 with mode; returns fd >= 0 or < 0 on error

- **fspos** ( fd :i -> :i ) — Return current byte offset in fd or < 0 on error
- **fsputc** ( fd :i c :i -> :i )

  Write a single byte c to fd; returns 0 on success or < 0 on error

- **fsread** ( fd :i buf :a len :i -> :i )

  Read up to len bytes from fd into buf; returns bytes read (0 on EOF) or < 0 on error

- **fsrename** ( oldp :a oldlen :i newp :a newlen :i -> :i )

  Rename file from (oldp, oldlen) to (newp, newlen); fails if target exists; returns 0 or < 0 on error

- **fsrename0** ( old0 :a new0 :a -> :i )

  Rename NUL-terminated old0 to NUL-terminated new0; returns 0 on success or < 0 on error

- **fsrmdir** ( path :a len :i -> :i )

  Remove empty directory at (path, len); returns 0 on success or < 0 on error

- **fsrmdir0** ( path0 :a -> :i )

  Remove empty directory at NUL-terminated path0; returns 0 on success or < 0 on error

- **fsseek** ( fd :i pos :i -> :i )

  Seek fd to byte offset pos from start; returns 0 on success or < 0 on error

- **fssize** ( fd :i -> :i ) — Return total byte size of open fd or < 0 on error
- **fswrite** ( fd :i buf :a len :i -> :i )

  Write len bytes from buf to fd; returns bytes written or < 0 on error

## Platforms

### agon

Agon Light MOS file I/O (8 open file slots via RST.LIL $08)

### arm64

File I/O via C stdio

### c64

Commodore 64 KERNAL / 1541 sequential file & directory I/O

### cpm

CP/M BDOS random/sequential file I/O (4 static FCB + 128-byte slots)

### x16

Commander X16 KERNAL / CMDR-DOS file & directory I/O
