# sfx

[← Library Index](README.md) · Source: [lib/sfx.ir4](../../lib/sfx.ir4)

Sound effects (optional override per platform)
Waveforms: 0 = pulse/square, 1 = sawtooth, 2 = triangle, 3 = noise.

## Functions

- **sfxcount** ( -> :i ) — Query number of hardware sound channels (0 = sound unsupported)
- **sfxplay** ( ch :i freq :i vol :i wave :i dur_ms :i )

  Play a tone on channel *ch* at *freq* Hz, volume *vol* (0..15), waveform *wave* (0..3), for *dur_ms* ms (0 = until sfxstop; default: no-op).

- **sfxstop** ( ch :i ) — Silence channel *ch* (default: no-op).
- **sfxtick** ( )

  Advance software duration timers once per frame (required on platforms without hardware duration, e.g. C64/X16; default: no-op).

## Platforms

### agon

Agon VDP sound effects (VDU 23, 0, &85)

### c64

Commodore 64 SID sound effects (3 voices at $D400)

### x16

Commander X16 VERA PSG sound effects (16 channels at VRAM $1F9C0)
