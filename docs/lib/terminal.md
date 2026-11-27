# terminal

[← Library Index](README.md) · Source: [lib/terminal.ir4](../../lib/terminal.ir4)

Text screen & cursor control (ANSI defaults)

## Functions

- **termbg** ( c :i )

  Set background text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default)
  - *c64:* Records background color for the next termclear (`$D021`; standard text mode has no per-character background).

- **termclear** ( )

  Clear screen (applying active background color where supported) and home cursor (ANSI default)
  - *c64:* Applies saved termbg color (`$D021`) and clears the text screen.

- **termcleareol** ( ) — Clear from cursor to end of current line (ANSI default)
- **termcleareos** ( ) — Clear from cursor to end of screen (ANSI default)
- **termcursor** ( visible :i )

  Show (*visible* <> 0) or hide (*visible* = 0) the text cursor (ANSI default)
  - *c64:* Set cursor flash flag via BLNSW ($00CC: 0 = enabled, 1 = disabled).
  - *x16:* No-op on X16 (no single KERNAL cursor visibility toggle)
  - *x16gs:* No-op on X16GS (no single KERNAL cursor visibility toggle)

- **termfg** ( c :i )

  Set foreground text color *c* using ANSI color index (0..7, 8..15 bright; ANSI default)

- **termgoto** ( x :i y :i ) — Move text cursor to 0-based column *x*, row *y* (ANSI default)
  - *zx81:* Updates DF_CC (`$400E`) and S_POSN (`$4039`/`$403A`) directly in D_FILE (clamped to 32x22).

- **termheight** ( -> :i ) — Return terminal height in rows (default: 24)
- **termreset** ( ) — Reset text attributes and colors to terminal defaults (ANSI default)
- **termwidth** ( -> :i ) — Return terminal width in columns (default: 80)
- **termx** ( -> :i )

  Return current 0-based cursor column, or < 0 if cursor readback is unsupported

- **termy** ( -> :i )

  Return current 0-based cursor row, or < 0 if cursor readback is unsupported

## Platform Specifics

### agon

Agon Light VDP text screen & cursor control

### c64

Commodore 64 KERNAL text screen & cursor control

### cpm

CP/M VT52 terminal screen & cursor control

### web

Web runtime terminal

### x16

Commander X16 KERNAL text screen & cursor control

### x16gs

Commander X16GS KERNAL text screen & cursor control

### zx81

Sinclair ZX81 D_FILE text screen & cursor control
