# sprite

[← Library Index](README.md) · Source: [lib/sprite.ir4](../../lib/sprite.ir4)

Hardware sprites

## Functions

- **sprcount** ( -> :i ) — Number of hardware sprite slots (0 = hardware sprites unsupported)
  - *agon:* 64 hardware sprites; mapped to VDP slots 63-id so sprite 0 renders in front.

- **sprdepth** ( -> :i ) — Maximum sprite color depth in bits per pixel (default: 0)
- **sprimg** ( img :i w :i h :i data :a pal :a ncolors :i -> :i )

  Define sprite image/frame *img* of screen size *w* x *h*. *data* holds *w* * *h* unsigned bytes (0 = transparent, 1..*ncolors* = 1-based palette index). *pal* holds *ncolors* unsigned bytes (color values returned by gfxrgb). Returns -1 on success, 0 if unsupported or out of sprite resources.
  - *c64:* Define sprite image img (0..47) in VIC Bank 3 (`$C000 + img*64`). ncolors = 1 uses hires (24x21); ncolors >= 2 uses multicolor (12x21, 1->`$D025`, 2->`$D027`+hw, 3->`$D026`). Pads any w < 24 or h < 21 with transparent pixels (0).

- **sprmaxh** ( -> :i ) — Maximum sprite height in pixels (default: 0)
- **sprmaxw** ( -> :i ) — Maximum sprite width in pixels (default: 0)
- **sprpos** ( id :i x :i y :i )

  Move sprite *id* to screen position (*x*, *y*) (default: no-op)

- **sprset** ( id :i img :i )

  Bind image *img* to hardware sprite *id* (sprite 0 has highest priority / renders in front; default: no-op).

- **sprshow** ( id :i on :i )

  Show (*on* <> 0) or hide (*on* = 0) sprite *id* (default: no-op)

## Platform Specifics

### agon

Agon VDP hardware sprites

### c64

Commodore 64 VIC-II hardware sprites (hires 24x21 or multicolor 12x21, 1x scale)

### web

Web runtime hardware-style sprites

### x16

Commander X16 VERA 8-bpp hardware sprites

### x16gs

Commander X16GS VERA 8-bpp hardware sprites
