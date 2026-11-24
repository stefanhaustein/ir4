# Agon Light / Console8 (`-t agon`)

## Compiler Flags

```sh
./build/ir4c -t agon -o out.bin input.ir4
```

- **Target / Platform:** `-t agon` (implies `-t ez80`, platform name `"agon"`)
- **Base address:** `0x40000` (MOS 24-bit ADL executable with `MOS` header at `+0x40`, `ADL = 1`)
- **Fallback chain:** `lib/agon/` → `lib/ez80/` → `lib/`

## Memory Layout

- **24-bit eZ80 ADL mode:** `JP entry` at `0x40000`, `MOS\0\1` header at `0x40040`, code + data + BSS + return stack following.
- **`main` signature:** 0 inputs, at most 1 output.
- **Float runtime:** Uses the embedded AgDev `float32` runtime when floating-point words are referenced.

## Assumptions & Notes

- **Graphics (`lib/agon/graphics.ir4`):** Uses VDP VDU `22` (mode), `18` (color), `25` (`PLOT` pixel/line/fillrect).
- **Modes:** Supports Agon VDP modes `8` (`320×240×6`), `9–11` (`320×240` 4/2/1-bpp), `1–2` (`512×384`), `0`/`4` (`640×480`), `12` (`320×200×6`), `16` (`640×240×6`).
- **`gfxrgb`:** Maps `0..255` RGB to Agon's default 16-color VGA palette (`0..15`, where `15` = bright white and `8` = dark gray) for 4/6-bpp modes, plus 1-bpp and 2-bpp handling.
- **Running in `fab-agon-emulator`:** Place `out.bin` on the emulated SD card alongside `MOS.bin`/`firmware.bin`, then `load out.bin` and `run` (or use `SDL_AUDIODRIVER=dummy` for silent headless runs).

## Running `ir4c` Natively on Agon

You can build a minimal Agon-only `ir4c` binary (~70 KB) using `agondev`:

```sh
make agon   # builds build/ir4c_agon.bin via agondev (-DIR4_MINIMAL -DIR4_ENABLE_EZ80=1 -DIR4_ENABLE_AGON=1)
```

- Copy `build/ir4c_agon.bin` to `/bin/ir4c.bin` on the Agon SD card (not `/mos/`, which is reserved for `$B0000` MOSlets) along with `lib/`.
- Compile on Agon at the MOS prompt:
  ```sh
  ir4c -t agon -I lib -o out.bin input.ir4
  load out.bin
  run
  ```
