# stdio

[← Library Index](README.md) · Source: [lib/stdio.ir4](../../lib/stdio.ir4)

Console I/O, formatting, and screen control
Platform overrides implement raw `emit` (and optionally the `default fn` entries below).

## Platforms

- *agon:* Agon MOS console I/O (RST $10 = print char in A)
- *c64:* Commodore 64 KERNAL console I/O ($FFD2 / $FFE4)
- *cpm:* CP/M BDOS console I/O (BDOS 2 / 6 / 11)
- *x16:* Commander X16 overrides (inherits emit, inkey, key, xemit, cls, gotoxy from c64/stdio.ir4)
- *zx81:* Sinclair ZX81 console I/O (ROM RST $10 / CLS $0A2A)

## Functions

- **bgcolor** ( c :i )

  Set background text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default)
  - *c64:* No-op on C64 (standard text mode has no per-character background color).
  - *zx81:* No-op on ZX81 (monochrome display).

- **cls** ( ) — Clear screen and home cursor (ANSI default)
- **cr** ( ) — Emit a newline
- **fgcolor** ( c :i )

  Set foreground text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default)
  - *zx81:* No-op on ZX81 (monochrome display).

- **gotoxy** ( x :i y :i ) — Move text cursor to 0-based column *x*, row *y* (ANSI default)
  - *zx81:* Updates DF_CC ($400E) and S_POSN ($4039/$403A) directly in D_FILE (clamped to 32x22).

- **inkey** ( -> :i )

  Non-blocking read of next buffered character; returns and consumes char code or 0 if none ready; default: 0
  - *zx81:* Returns raw 2-byte LAST_K ($4025) scan code (non-ASCII), or 0 if no key is pressed.

- **key** ( -> :i )

  Wait for and return the next character from the input buffer (blocking except when unsupported; defaults to 0 in that case)

- **termheight** ( -> :i ) — Return terminal height in rows (default: 24)
- **termwidth** ( -> :i ) — Return terminal width in columns (default: 80)
- **type** ( s :a len :i ) — Print *len* bytes starting at *s*
- **type0** ( s :a ) — Print NUL-terminated string at *s*
- **typeb** ( n :i ) — Print *n* as minimal binary
- **typebw** ( n :i w :i ) — Print *n* as *w* zero-padded binary digits
- **typeh** ( n :i ) — Print *n* as minimal uppercase hex
- **typehw** ( n :i w :i ) — Print *n* as *w* zero-padded uppercase hex digits
- **typei** ( n :i ) — Print *n* as signed decimal
- **typeiw** ( n :i w :i )

  Print *n* as signed decimal, right-aligned with spaces to width *w*

- **typeu** ( n :i ) — Print *n* as minimal unsigned decimal
- **xemit** ( c :i )

  Emit character *c* with platform text mapping (defaults to raw emit; PETSCII on C64)
  - *c64:* Switches C64 to Lowercase/Uppercase mode (14 emit) at startup; maps ASCII A-Z, a-z, _, and | to PETSCII.

- **xinkey** ( -> :i )

  Non-blocking read of next key mapped to normalized ASCII (defaults to raw inkey)
  - *c64:* Maps PETSCII input (in Lowercase/Uppercase mode) to normalized ASCII (unshifted a-z, shifted A-Z).
  - *zx81:* Decodes LAST_K ($4025) via ROM $07BD to normalized ASCII, or 0 if no key is pressed.

- **xkey** ( -> :i )

  Blocking read of next key mapped to normalized ASCII (defaults to raw key)
  - *c64:* Blocking read using xinkey.
  - *zx81:* Blocking read using xinkey.
