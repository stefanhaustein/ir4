# IR4(th) Companion Library

> Generated from `lib/*.ir4` by `tools/gen_lib_docs.py`.

## Naming Convention

- **Public words & scalar constants:** Lowercase without underscores (`typeiw`, `sprmaxw`, `fsopen`).
- **Enum / option constants:** `<group>_<value>` in lowercase (`fsmode_read`, `fsmode_write`).
- **Internal library symbols:** Prefixed with `__` (`__gfx_w`, `__c64_modes`, `__heap_ptr`).
- **Frontend symbols:** Language frontends targeting IR4(th) are expected to mangle or prefix user-defined names to avoid colliding with built-in keywords, companion library words, or `__` internals.

## Modules

- [file](file.md) — Sequential file I/O (fallback stubs + convenience wrappers)
- [graphics](graphics.md) — Pixel graphics (default: ANSI 16-color VGA / 256-color UTF-8 half-block backend)
- [heap](heap.md) — First-fit dynamic memory allocator starting at __heap_start
- [i32](i32.md) — 32-bit signed division and modulo fallbacks (auto-required when i32/ or i32mod is used)
- [joystick](joystick.md) — Joystick and gamepad input
- [math](math.md) — Floating-point math helpers
- [memory](memory.md) — Byte-slice memory operations
- [sound](sound.md) — Sound effects
- [sprite](sprite.md) — Hardware sprites
- [stdio](stdio.md) — Console I/O, formatting, and screen control
- [system](system.md) — System, CLI arguments, environment, CWD, and clock API
- [terminal](terminal.md) — Text screen & cursor control (ANSI defaults)
