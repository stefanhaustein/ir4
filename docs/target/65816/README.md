# W65C816 (-t 65816)

Native-mode 16-bit word / 24-bit flat-address W65C816 backend (`src/backend/65816.c`) used by `-t x16gs` and `-t sim65816`.

## Compiler Flags

```sh
./build/ir4c -t 65816 [-b hex_addr] -o out.bin input.ir4
./build/ir4c -t sim65816 -o out.bin input.ir4   # compile and run in built-in simulator
```

## Execution Model

- **Processor mode:** Native mode (`E = 0`) with 16-bit accumulator (`M = 0`) and 16-bit index registers (`X = 0`) preserved across IR operations.
- **Type widths (`16x24`):**
  - `:i` = 2 bytes (signed 16-bit)
  - `:a` = 3 bytes (unsigned 24-bit linear address)
  - `:i32` = 4 bytes (32-bit integer)
  - `:f` = 4 bytes (IEEE-754 single-precision `binary32`)
- **Data stack:** `$0200..$0FFF` in the active Data Bank (`DBR`), indexed downward by `X` (top of stack at `$0200,X`, initial `X = $0E00`).
- **Return stack:** Hardware stack `S` using 24-bit `JSL` / `RTL` calls.
- **Direct Page (`D`):**
  - `$00..$03` (`DP_PTR0`), `$04..$07` (`DP_PTR1`), `$08..$0B` (`DP_TMP0`), `$0C..$0F` (`DP_TMP1`): scratch registers for 24-bit `[dp]` / `[dp],Y` addressing and temporaries.
  - `$10..$13` (`DP_LFP`): downward-growing 24-bit local frame pointer in a 16 KB BSS stack for recursive functions and `:bytes` locals, indexed directly via `[DP_LFP]` / `[DP_LFP],Y` (must be preserved by `code 65816` words).
  - `$14..$1B`: additional runtime temporaries.
- **Locals:** Non-recursive scalar locals reside in static BSS accessed via 24-bit long addressing; recursive functions allocate frames downward on `DP_LFP` so local offsets use direct `[DP_LFP]` / `[DP_LFP],Y` addressing without scratch pointer calculation.
- **64 KB bank boundaries:** Because the W65C816 `PC` wraps within a 64 KB program bank (`PBR`), `ir_emit` pads functions that would straddle a 64 KB code boundary into the next bank automatically. Data pointers (`a+`, `a-`, `+->`, and `[dp],Y` memory/string loops) propagate carry and borrow across 24-bit banks.

## Backend Optimizations

- **Static & direct-indexed frames:** Non-recursive scalar locals use direct 24-bit long loads/stores (`LDA/STA long`), while recursive frames index `[DP_LFP]` / `[DP_LFP],Y` directly.
- **Stack & TOS-in-`A` peepholes:** Cancels adjacent `INX` / `DEX` adjustments across 2-, 3-, and 4-byte stack slots, elides redundant `STA $0200,X` + `LDA $0200,X` round-trips across `:i` expression chains, uses `STZ` for zero-word pushes, and pops 24-bit pointers into 4-byte Direct Page scratch slots in 16-bit mode without `SEP`/`REP` toggling.
- **Bank-carry fast path:** Constant pointer offsets (`a+`, `a-`, `local_add_imm`) skip bank-byte adjustments via `BCC` / `BCS` when the low 16-bit addition does not cross a 64 KB boundary.
- **Immediate & compare-branch fusion:** Fuses constant arithmetic, bitwise, shift, comparison, static/recursive local memory ops, zero-compare branches (`BEQ`/`BNE`/`BMI`/`BPL` without `CMP #0`), and compare-and-branch pairs.
- **Native companion overrides (`lib/65816/`):** Provides native `code 65816` implementations for `i32<<`, `u32>>`, `i32>>`, `memfill`, `memcpy`, `memcmp`, `strlen0`, and `strcmp0`.

## Platforms

- **sim65816 (-t sim65816)** — Built-in 2 MB flat-RAM W65C816 simulator (`src/platform/sim65816.c`) with console and host file I/O traps for headless testing.
- [Commander X16GS (-t x16gs)](../x16gs/README.md)
