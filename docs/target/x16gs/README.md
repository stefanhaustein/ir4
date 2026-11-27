# Commander X16GS (-t x16gs)

24-bit SuperRAM platform targeting the W65C816-equipped Commander X16GS (`src/platform/x16gs.c`).

## Compiler Flags

```sh
./build/ir4c -t x16gs -o out.prg input.ir4
```

- **Target / Platform:** `-t x16gs` (implies `-t 65816`, platform name `"x16gs"`)
- **Output files:** Emits a small Bank 0 BASIC/KERNAL loader `out.prg` alongside the contiguous SuperRAM payload `OUT.BIN` (loaded into `$01:1000+` via KERNAL `LOAD` + `MACPTR` / bank-crossing transfers).
- **Fallback chain:** `lib/x16gs/` → `lib/65816/` → `lib/`

## Memory Layout

- **Bank `$00` (System / KERNAL Bank):**
  - `$00:0400..$00:043F`: Direct Page (`D = $0400` in Golden RAM)
  - `$00:0440..$00:04FF`: Saved caller stack pointer and `E=1` KERNAL call trampolines (switching cleanly between `E=0` native mode in SuperRAM and `E=1` 65C02 KERNAL routines in Bank `$00`)
  - `$00:0500..$00:05FF`: `"IR4A"` command-line argument mailbox (`sysargc` / `sysargv`)
  - `$00:0600..$00:06FF`: 256-byte Golden RAM block I/O bounce buffer for `MACPTR` (`$FF44`) / `MCIOUT` (`$FEB1`)
  - `$00:0801+`: `.prg` bootstrap loader
  - `$00:1FFF` (downward): Native hardware return stack (`S`)
- **Banks `$01+` (Contiguous SuperRAM):**
  - `$01:0200..$01:0FFF`: IR4(th) data stack (`DBR = $01`, indexed by `X`, initial `X = $0E00`)
  - `$01:1000+`: Contiguous program code, `data`, static `var` BSS, recursive frame stack, and heap (`__heap_start` upward across SuperRAM banks)

## Companion Libraries (`lib/x16gs/`)

- **Console & Terminal (`stdio.ir4`, `terminal.ir4`):** ISO/PETSCII character mapping via `CHROUT` (`$FFD2`) with automatic PETSCII restoration on exit, cursor positioning via `PLOT` (`$FFF0`), and ANSI color mapping.
- **File I/O (`file.ir4`):** Supports up to 8 concurrent open files (LFNs `2..9`) with buffered `MACPTR` / `MCIOUT` block transfers and lazy directory-based `fssize` lookup.
- **Graphics, Sprites, Sound & Joystick (`graphics.ir4`, `sprite.ir4`, `sound.ir4`, `joystick.ir4`):** VERA 320x240 / 160x120 8bpp bitmap graphics, hardware sprites, PSG sound channels, and SNES/keyboard controller polling via `joystick_get` (`$FF56`).

## Running in `x16emu`

Place both `out.prg` and `OUT.BIN` in the host filesystem directory (`-fsroot`):

```sh
x16emu -gs -rom /path/to/rom.bin -fsroot /path/to/dir -prg out.prg -run
```

## Self-Hosting `ir4c` on X16GS

`make x16gs` compiles the C89 `ir4c` sources through LCC (`-target=ir4/16x24`) and builds `build/x16gs/ir4c.prg` + `build/x16gs/IR4C.BIN`:

```sh
make x16gs        # builds build/x16gs/ir4c.prg + IR4C.BIN
make test-x16gs   # runs all X16GS library tests and verifies self-hosted HELLO.PRG & SPHERES.PRG
```

When launched interactively on X16GS without a pre-populated `$0500` argument mailbox, `ir4c.prg` prompts for the input `.ir4` file and output `.prg` path.
