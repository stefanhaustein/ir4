# gfx

[← Library Index](README.md) · Source: [lib/gfx.ir4](../../lib/gfx.ir4)

Pixel graphics (default: ANSI 8-color UTF-8 half-block backend)
Platform overrides implement the `default fn` entries below.

## Platforms

- *agon:* Agon VDP stateful graphics
- *c64:* Commodore 64 VIC-II Hires Bitmap (320x200) in VIC Bank 3 ($C000-$FFFF)
- *cpm:* CP/M VT52 ASCII brightness graphics (80x24, 8 shades)
- *x16:* Commander X16 VERA 8bpp bitmap modes (320x240 and 160x120 via Mode $80)
- *zx81:* Sinclair ZX81 64x48 block-mosaic pixel graphics in D_FILE

## Functions

- **gfxborder** ( c :i )

  Set hardware border/overscan color *c* (value returned by gfxrgb; no-op if unsupported)

- **gfxcharh** ( -> :i )

  Character cell height in pixels (default: 2 in ANSI half-block mode)

- **gfxcharw** ( -> :i )

  Character cell width in pixels (default: 1 in ANSI half-block mode)

- **gfxcircle** ( cx :i cy :i r :i )

  Draw a circle outline of radius *r* centered at (*cx*, *cy*) using the midpoint algorithm

- **gfxclear** ( ) — Clear the graphics screen to the current drawing color
- **gfxcolor** ( c :i ) — Set current drawing color *c* (value returned by gfxrgb)
- **gfxdepth** ( -> :i )

  Per-pixel color depth in bits (1 on attribute-cell targets like C64/Spectrum, where gfxrgb may still return palette colors per cell)

- **gfxdone** ( ) — Restore text mode and release graphics resources
- **gfxfillrect** ( x :i y :i w :i h :i )

  Draw a filled rectangle at (*x*, *y*) with size *w* x *h* (default: gfxhline loop)

- **gfxheight** ( -> :i ) — Current graphics screen height in pixels
- **gfxhline** ( x :i y :i w :i )

  Draw a horizontal line of width *w* starting at (*x*, *y*) (default: gfxpx loop)

- **gfxinit** ( w :i h :i depth :i )

  Enter best-matching mode from gfxmodes for (*w*, *h*, *depth*); (0, 0, 0) selects entry 0

- **gfxline** ( x1 :i y1 :i x2 :i y2 :i )

  Draw a line from (*x1*, *y1*) to (*x2*, *y2*) (default: Bresenham via gfxhline/gfxvline/gfxpx)

- **gfxmode** ( mode :i )

  Switch to graphics mode index *mode* (default: ANSI half-block buffer)

- **gfxmodes** ( -> :a :i )

  Returns (addr, count) of :i16 4-tuples: (mode, width, height, depth_bpp)

- **gfxpx** ( x :i y :i ) — Plot a pixel at (*x*, *y*) in the current drawing color
- **gfxrect** ( x :i y :i w :i h :i )

  Draw a rectangle outline at (*x*, *y*) with size *w* x *h*

- **gfxrgb** ( r :i g :i b :i -> :i )

  Map 8-bit RGB (*r*, *g*, *b* in 0..255) to the platform's native color value for gfxcolor. On attribute-cell targets (gfxdepth = 1), non-zero colors may still set per-cell attributes.
  - *agon:* Map 8-bit RGB (0..255) to Agon default palette (1-bpp, 2-bpp, or 16-color VGA 0..15)
  - *x16:* Map 8-bit RGB to X16 default 6x6x6 palette cube (indices 16..231)

- **gfxtext** ( x :i y :i s :a len :i )

  Draw *len* normalized-ASCII bytes starting at *s* with top-left at pixel (*x*, *y*) in the current drawing color. Text-cell targets snap (*x*, *y*) to the nearest character cell (default: ANSI `x, y/2`).
  - *agon:* Pixel-positioned 8x8 text via VDP `VDU 5` at the graphics cursor.
  - *c64:* Pixel-positioned 8x8 text from C64 Char ROM ($D800) using `xemit` ASCII mapping.
  - *cpm:* Snaps (*x*, *y*) to VT52 character cell (`x, y`) via `type`.
  - *x16:* Pixel-positioned 8x8 text from VERA VRAM ($1F000) using `xemit` ASCII mapping.
  - *zx81:* Snaps (*x*, *y*) to the nearest 2x2 cell (`x/2, y/2`) in `D_FILE` via `type`.

- **gfxticks** ( -> :i ) — Return platform tick counter (wraps monotonically; default: 0)
- **gfxvline** ( x :i y :i h :i )

  Draw a vertical line of height *h* starting at (*x*, *y*) (default: gfxpx loop)

- **gfxvsync** ( ) — Wait for vertical blank / next frame sync (default: no-op)
- **gfxwidth** ( -> :i ) — Current graphics screen width in pixels
