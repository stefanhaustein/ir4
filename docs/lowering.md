# IR4th Lowering Guide

Practical patterns for compiler and language frontends targeting IR4th. For exact syntax and semantics, see [spec.md](spec.md).

## 1. Types & Portability

- **`:i` (native word):** 16-bit on Z80/6502, 24-bit on eZ80, 32-bit on RV32, 64-bit on ARM64. Use `:i` for loop counters, small integers, booleans (`true` = `-1`, `false` = `0`), and `jtab` indices.
- **`:i32` (fixed 32-bit):** Use when language semantics require 32-bit wraparound or range across 8/16-bit targets.
- **`:a` (address):** Use for all pointers. Pointer arithmetic uses `a+`, `a-`, and `adiff`.
- **Literal folding:** Casting an integer literal with `i>a`, `i>i32`, or `u>i32` folds at compile time with zero runtime cost.

## 2. Structs, Arrays & Buffers

IR4th does not have built-in `struct` or array types. Lower them to raw byte allocations (`:bytes`) and constant byte offsets:

- **Global / static structs & arrays:** `var player :bytes 16` or `data ... end`. Accessing `player 4 a+ i16@` folds the offset at compile time into a direct static load on Z80/6502.
- **Local / stack-allocated structs & arrays:** `var tmp :bytes 16` inside `fn ... end`. Accessing `tmp 4 a+ i16@` fuses into a direct frame-relative load/store (`local_mem`).
- **Dynamic array indexing:** `base idx elem_size * a+ @`. When `elem_size` is a power of two (1, 2, 4, 8), `*` strength-reduces to a shift (`<<`) automatically.

## 3. Locals & In-Place Updates

- Declare and initialize locals in one step: `<expr> -> var x :i`.
- Use `+-> x` instead of `x <delta> + -> x` for increments/accumulators — it lowers to in-place `LOCAL_ADD` (and `local_add_imm` for constant deltas).
- To pass a variable by reference (out-parameter), pass `& x` (`:a`) to the callee.

## 4. Control Flow & Short-Circuiting

- **If / else / loops:** `if ... else ... then`, `begin ... while ... repeat`, and `begin ... until` fuse directly with comparisons (`=`, `<>`, `<`, `>`, `<=`, `>=`, `u<`, `u>`, `u<=`, `u>=`, `a=`, `a<>`, `a<`, `a>`) without materializing `-1`/`0` on Z80, eZ80, 6502, ARM64, or RV32.
- **Short-circuit `&&` / `||`:** Lower using `if ... else ... then` or `::` / `jz` / `jmp`. Remember that `::` and `jmp` require the evaluation stack to be empty, so store short-circuit results into a local `var` before jumping.

## 5. `switch` / `match` Statements

Lower `switch` based on case density:

- **Dense integer ranges (`jtab`):**
  Subtract the minimum case value, emit an explicit unsigned bounds check (`u>=`) to a default label if needed, then dispatch with `jtab ... end`:

  ```forth
  x 10 -
  dup 3 u>= if drop jmp sw_default then
  jtab sw_0 sw_1 sw_1 sw_2 end

  :: sw_0
    100 -> res
    jmp sw_done     \ break
  :: sw_1
    200 -> res
    \ omit jmp to fall through
  :: sw_2
    res 1 + -> res
    jmp sw_done
  :: sw_default
    0 -> res
  :: sw_done
  ```

  - Shared case arms simply repeat the same label (`sw_1 sw_1`).
  - If the index is already masked (e.g. `3 and`), omit the `u>=` guard.
  - Note: on 6502, a single `jtab` supports up to 256 entries.
- **Sparse cases:** Lower to a chain of `dup <const> = if drop ... jmp sw_done then` (or binary search), which uses `cmp_br_fwd` directly.
- **Dispatch / vtables across functions:** Store `& fn_name` entries in a `data ... end` block, scale the index by pointer width (computed via `& probe_b & probe_a adiff` or a platform constant), load with `a@`, and invoke via typed indirect call `( <inputs> -> <outputs> )`.

## 6. Constants & Platform Abstraction

- Emit `const NAME VALUE` for compile-time `:i` or `:f` constants; they participate in constant folding and immediate instruction fusion (`add_imm`, `bit_imm`, `cmp_imm`, `shift_imm`).
- Use `default const` and `default fn` in portable libraries so target-specific `require` overrides can specialize them cleanly.
