# sys

[← Library Index](README.md) · Source: [lib/sys.ir4](../../lib/sys.ir4)

Portable system, CLI arguments, environment, CWD, and clock API

## Functions

- **sysargc** ( -> :i )

  Return number of command-line arguments (including index 0); 0 if unavailable

- **sysargv** ( idx :i buf :a max :i -> :i )

  Copy argument idx (0 .. sysargc-1) into buf (at most max bytes); return length or < 0 if unavailable

- **syscwd** ( buf :a max :i -> :i )

  Copy current working directory path into buf (at most max bytes); return length or < 0

- **sysenv** ( name :a nlen :i buf :a max :i -> :i )

  Look up environment variable name (nlen bytes); copy up to max bytes into buf; return length or < 0

- **sysenv0** ( name0 :a buf :a max :i -> :i )

  Look up NUL-terminated environment variable name0; copy up to max bytes into buf; return length or < 0

- **sysmillis** ( -> :i32 )

  Return monotonic millisecond counter as 32-bit signed integer (:i32)

- **syssleep** ( ms :i ) — Sleep for approximately ms milliseconds

## Platforms

### agon

Agon Light MOS system, CLI args, CWD, and millisecond clock

### c64

Commodore 64 KERNAL system clock and sleep

### cpm

CP/M BDOS CLI arguments and current drive letter

### x16

Commander X16 KERNAL system clock, sleep, and CMDR-DOS CWD
