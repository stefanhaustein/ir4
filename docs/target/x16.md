# Commander X16 (`-t x16`)

## Compiler Flags

```sh
./build/ir4c -t x16 -o out.prg input.ir4
```

- **Target / Platform:** `-t x16` (implies `-t 6502`, platform name `"x16"`)
- **Base address:** `0x0801` (BASIC `10 SYS ...` autostart `.prg` header)
- **Fallback chain:** `lib/x16/` → `lib/6502/` → `lib/`

## Memory Layout

- **Zero Page:**
  - `default_zp`: `0x7C`
  - `default_stack_zp`: `0x22..0x5A`
  - `tmp`: `0x02/0x03`
- **`ram_top` / `bfp`:** `0x9F00` (below `$9F00–$9FFF` I/O)
- **Program / BSS / Heap:** `0x0801..0x9EFF` (~37.7 KB contiguous)

## Assumptions & Notes

- **No `$01` ROM unmapping:** Unlike C64, `$01` is the X16 ROM/RAM bank register and is left untouched.
- **Console & Terminal (`lib/x16/stdio.ir4`, `lib/x16/terminal.ir4`):** PETSCII color codes, KERNAL `emit` (`$FFD2`) / `inkey` (`$FFE4`) / `termgoto` & `termx`/`termy` via `PLOT` (`$FFF0`), `termwidth`/`termheight` via `SCREEN` (`$FFED`), and `$80` verbatim `"` handling in `xemit`.
- **Graphics (`lib/x16/graphics.ir4`):**
  - Mode `128`: `320×240×8bpp` (VERA Mode `$80` via KERNAL `screen_mode` `$FF5F`)
  - Mode `1`: `160×120×8bpp` (VERA Mode `$80` with `DC_HSCALE`/`DC_VSCALE = 32`)
  - `gfxmode` programs VERA palette indices `16..231` (`$1FA20`) to a `6×6×6` RGB cube (`v * 3` per 4-bit channel) for `gfxrgb`.
  - `gfxhline` uses VERA `+1` (`$10`) auto-increment; `gfxvline` uses VERA `+320` (`$E0`) auto-increment.
- **Running in `x16emu`:**
  ```sh
  x16emu -rom /path/to/rom.bin -prg out.prg -run
  ```

---

# Commander X16 Cartridge (`-t x16-cart`) & Bank Switching

For programs larger than Low RAM (`$0801–$9EFF`, ~37.7 KB), `-t x16-cart` emits a banked `.crt` cartridge image with 16 KB ROM banks mapped at `$C000–$FFFF` (controlled via `$01`).

## Compiler Flags

```sh
./build/ir4c -t x16-cart -o out.crt input.ir4
```

- **Running in `x16emu`:**
  ```sh
  x16emu -rom /path/to/rom.bin -cart out.crt
  ```

## Shared Bank-Switching Architecture

The bank-switching layer in `src/codebuf.c` and `src/frontend/ir_emit.c` is target-independent (shared with future banked targets such as ZX Spectrum 128K):

- **`FuncSig.bank` (`src/ir4.h`):** `0` for fixed Low RAM, `1..N` for switchable 16 KB banks.
- **`codebuf` bank descriptors (`src/codebuf.c`):** `cb_set_bank_config(bank_base, bank_size)` enables banked mode while keeping a single flat `code_buf`, so `cb_pc()`, data relocations, and forward-call records remain globally unique. `cb_begin_bank(id)` / `cb_end_banks()` track each bank's `{ base, start_pc, len }` slice, and `cb_cur_addr()` computes the virtual address within the active bank.
- **Pass 2 bank assignment (`src/frontend/ir_emit.c`):**
  - `bank 0` (fixed Low RAM): `main`, `init`, `hot fn`s, function-pointer targets (`is_code`, keeping function pointers 16-bit), and any runtime helpers (`mul16`, `divmod16`, `icall`, `wozfp`) referenced by banked code.
  - `bank 1..N` (16 KB banks at `$C000`): remaining reachable functions are packed greedily up to a soft cap (`bank_size - 4096`).

## X16 `.crt` Layout & Runtime Trampolines (`src/platform/x16.c`, `src/backend/6502.c`)

- **`.crt` file format:**
  - `480`-byte `CX16 CARTRIDGE` header (`01.00`, bank-type table starting at offset `256` with `1` for each ROM bank).
  - **Physical bank 32 (boot bank):** `"CX16"` signature at `$C000`, boot stub at `$C004` that copies `bank 0` + initialized `data` to `$0801`, zeroes BSS, initializes `X`, calls `main`, optionally prints a 16-bit return value, and enters BASIC via `$FF47` (`CLC`).
  - **Physical banks `33+`:** Logical code banks `1..N`, zero-padded to 16 KB each.
- **Cross-bank calls (`__bank_call`):** Calls between different ROM banks (or from Low RAM to a ROM bank) emit `JSR __bank_call; .byte phys_bank; .word addr` (6 bytes). The Low-RAM trampoline saves the caller's `$01` bank on the hardware stack, updates the return address past the 3-byte inline descriptor, switches `$01`, calls the target without clobbering `X` (the data stack pointer), and restores `$01`.
- **KERNAL calls (`__kernal_call`):** Calls to `extern` routines at `>= $C000` (e.g. `CHROUT` `$FFD2`) go through a Low-RAM `__kernal_call` trampoline (`JSR __kernal_call; .word ext_addr`) that saves `$01`, switches to KERNAL ROM (`$01 = 0`), preserves `A`/`X`/`Y`/Carry across the call, and restores the caller's bank.
