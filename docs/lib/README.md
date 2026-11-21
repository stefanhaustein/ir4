# IR4(th) Companion Library

> Generated from `lib/*.ir4` by `tools/gen_lib_docs.py`.

## Naming Convention

- **Public words & scalar constants:** Lowercase without underscores (`typeiw`, `sprmaxw`, `fsopen`).
- **Enum / option constants:** `<group>_<value>` in lowercase (`fsmode_read`, `fsmode_write`).
- **Internal library symbols:** Prefixed with `__` (`__gfx_w`, `__c64_modes`, `__heap_ptr`).
- **Frontend symbols:** Language frontends targeting IR4(th) are expected to mangle or prefix user-defined names to avoid colliding with built-in keywords, companion library words, or `__` internals.

## Modules

- [gfx](gfx.md) — Pixel graphics (default: ANSI 8-color UTF-8 half-block backend)
- [i32](i32.md) — 32-bit signed division and modulo fallbacks (auto-required when i32/ or i32mod is used)
- [joy](joy.md) — Joystick and gamepad input (optional override per platform)
- [math](math.md) — Floating-point math helpers
- [mem](mem.md) — Memory operations and first-fit heap allocator starting at __heap_start
- [sfx](sfx.md) — Sound effects (optional override per platform)
- [sprite](sprite.md) — Hardware sprites (optional override per platform)
- [stdio](stdio.md) — Console I/O, formatting, and screen control
