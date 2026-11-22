# stdio

[← Library Index](README.md) · Source: [lib/stdio.ir4](../../lib/stdio.ir4)

Console I/O, formatting, and screen control
Platform overrides implement raw `emit` (and optionally the `default fn` entries below).

## Functions

- **bgcolor** ( c :i )

  Set background text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default)
  - *c64:* Records background color for the next cls (`$D021`; standard text mode has no per-character background).
  - *zx81:* No-op on ZX81 (monochrome display).

- **charset_native** ( -> :i )

  Native boot/symbol-oriented character set (`setcharset`/`getcharset`)

- **charset_text** ( -> :i )

  Text-oriented character set with lowercase support (`setcharset`/`getcharset`)

- **cls** ( )

  Clear screen (using active background color where supported) and home cursor (ANSI default)
  - *c64:* Applies saved bgcolor (`$D021`) and clears the text screen.

- **cr** ( ) — Emit a newline
- **fgcolor** ( c :i )

  Set foreground text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default)
  - *zx81:* No-op on ZX81 (monochrome display).

- **getcharset** ( -> :i )

  Return active effective character set (`charset_text` when lowercase is supported, `charset_native` otherwise)
  - *zx81:* Always returns `charset_native` (`0`: ZX81 ROM has uppercase and block symbols only).

- **gotoxy** ( x :i y :i ) — Move text cursor to 0-based column *x*, row *y* (ANSI default)
  - *zx81:* Updates DF_CC (`$400E`) and S_POSN (`$4039`/`$403A`) directly in D_FILE (clamped to 32x22).

- **inkey** ( -> :i )

  Non-blocking read of next buffered character; returns and consumes char code or 0 if none ready; default: 0
  - *zx81:* Returns raw 2-byte LAST_K (`$4025`) scan code (non-ASCII), or 0 if no key is pressed.

- **key** ( -> :i )

  Wait for and return the next character from the input buffer (blocking except when unsupported; defaults to 0 in that case)

- **setcharset** ( mode :i )

  Select text character set (`charset_native` or `charset_text`; no-op where fixed)
  - *c64:* Switch between `charset_native` (`0`, `142 emit`) and `charset_text` (`1`, `14 emit`).
  - *x16:* Switch between `charset_native` (`0`, `143 emit 142 emit`) and `charset_text` (`1`, `15 emit` ISO-8859-15).

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
  - *c64:* Maps ASCII to PETSCII (`charset_text`: mixed case; `charset_native`: folds `a..z` to uppercase) and clears quote mode (`$D4`) after `"`.
  - *x16:* Maps ASCII to ISO (`charset_text`) or uppercase PETSCII (`charset_native`); uses `$80` verbatim mode for `"` so quote mode never triggers.

- **xinkey** ( -> :i )

  Non-blocking read of next key mapped to normalized ASCII (defaults to raw inkey)
  - *c64:* Maps PETSCII input (in Lowercase/Uppercase mode) to normalized ASCII (unshifted a-z, shifted A-Z).
  - *x16:* Maps key input to normalized ASCII (direct pass-through in ISO `charset_text`; PETSCII case swap in `charset_native`).
  - *zx81:* Decodes LAST_K (`$4025`) via ROM `$07BD` to normalized ASCII, or 0 if no key is pressed.

- **xkey** ( -> :i )

  Blocking read of next key mapped to normalized ASCII (defaults to raw key)
  - *c64:* Blocking read using xinkey.
  - *x16:* Blocking read using xinkey.
  - *zx81:* Blocking read using xinkey.

## Platforms

### agon

Agon MOS console I/O (RST $10 = print char in A)
ADL mode: POP DE (ret), POP HL (c), PUSH DE, LD A, L, RST $10, RET

### c64

Commodore 64 KERNAL console I/O (`$FFD2` / `$FFE4`)

### cpm

CP/M BDOS console I/O (BDOS 2 / 6 / 11)

### x16

Commander X16 KERNAL console I/O (`$FFD2` / `$FFE4` / `$FFF0`)

### zx81

Sinclair ZX81 console I/O (ROM RST `$10` / CLS `$0A2A`)
