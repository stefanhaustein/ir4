# Commodore 64 (`-t c64`)

## Compiler Flags

```sh
./build/ir4c -t c64 -o out.prg input.ir4
```

- **Target / Platform:** `-t c64` (implies `-t 6502`, platform name `"c64"`)
- **Base address:** `0x0801` (BASIC `10 SYS ...` autostart `.prg` header)
- **Fallback chain:** `lib/c64/` → `lib/6502/` → `lib/`

## Memory Layout

- **Zero Page:**
  - `stack_zp_base`: `0x20..0x70` (6502 zero-page data stack)
  - `tmp`: `0x02/0x03`
- **Hardware return stack:** `0x0100..0x01FF`
- **`ram_top` / `bfp`:** `0xCC00` (`bfp` frame stack grows downward from `0xCC00`)
- **Program / BSS / Heap:** `0x0801..0xCBFF` (~49 KB contiguous)

### Graphics (`lib/c64/graphics.ir4`)

Uses **VIC Bank 3 (`$C000–$FFFF`)** so there is no mid-RAM graphics hole:

| Address Range | Purpose |
|---|---|
| `$0801–$CBFF` | Code + BSS + Heap + `bfp` frame stack (↓ from `$CC00`) |
| `$CC00–$CFE7` | VIC Character RAM (1,000 bytes, `40×25` color attributes) |
| `$D000–$DFFF` | VIC / SID / CIA I/O |
| `$E000–$FF3F` | VIC Graphics RAM (8,000 bytes, under KERNAL ROM) |

## Assumptions & Notes

- **ROM banking (`$01`):** Startup unmaps BASIC ROM (`$01 = $36`, keeping KERNAL ROM mapped at `$E000–$FFFF` for `CHROUT`/`GETIN` and IRQs) and restores `$01` on exit.
- **Reading graphics RAM:** Writes to `$E000–$FF3F` go directly to RAM, while reads use `__c64_peek_under` (`SEI`, `$01 = $35`, read, `$01 = $36`, `CLI`).
- **Color model:** `320×200` hires (`gfxdepth = 1` per pixel). `gfxrgb` maps to 8 C64 colors (`0..15`) and updates the 8×8 cell's foreground attribute in `$CC00` (ZX Spectrum-style attribute clash; background stays color 0).
- **Running in VICE:**
  ```sh
  x64sc -autostartprgmode 1 -autostart out.prg
  ```
