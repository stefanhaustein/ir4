# IR4th Language Specification

**IR4th** (*Typed Forth as an Intermediate Representation*) is a minimal, statically typed, monomorphic concatenative intermediate language designed for retro-computing CPUs (such as the Z80 and 6502) and constrained embedded systems.

IR4th compiles via direct 2-pass machine-code generation (with dead-code elimination) to native Z80, eZ80, 6502, RISC-V (RV32), and ARM64 binaries, as well as portable bytecode for the built-in VM.

## 1. Lexical Structure

### 1.1 Tokenization & Whitespace
* Programs consist of **whitespace-delimited tokens** (spaces, tabs, newlines).
* Line comments start with `\` followed by whitespace (or newline) and extend to the end of the line.
* `#line <num>` or `#line <num> "<filename>"` sets the current source line number (and optionally filename) for subsequent error messages.

### 1.2 Lexical Categories
* **Type Tokens (`TYPE`):** Any token starting with a leading colon (`:`) is strictly a type specifier (`:i`, `:i32`, `:a`, `:f`).
* **Keywords:** Reserved syntax and control-flow tokens (`fn`, `default`, `hot`, `const`, `fwd`, `init`, `code`, `inline`, `end`, `require`, `extern`, `var`, `:bytes`, `data`, `:i8`, `:i16`, `:i32`, `align`, `->`, `+->`, `&`, `__heap_start`, `return`, `if`, `else`, `then`, `begin`, `while`, `repeat`, `::`, `jmp`, `jz`, `(`, `)`).
* **Numeric Literals:** Decimal (`0`, `42`, `-10`), hexadecimal (`0xFF`, `$4000`), or floating-point (`3.14`, `-0.5`).
* **Identifiers (`IDENT`):** Any non-numeric token without a leading colon that is not a keyword.

## 2. Type System (v0.3)

IR4th v0.3 defines **four stack types** on a **single unified stack** with **explicitly typed operators** (no operator overloading). Nothing smaller than a native machine word `:i` exists on the stack; byte- and 16-bit memory accesses (`u8@`, `i8@`, `i8!`, `u16@`, `i16@`, `i16!`) implicitly widen to and truncate from `:i`.

| Type Token | Name | Width & Semantics | Z80 / 6502 | eZ80 (ADL) | ARM32 / RISC-V32 | ARM64 / x86-64 / VM |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **`:i`** | Native Word | Target-native signed integer (`isize`) | 2 bytes (`i16`) | 3 bytes (`i24`) | 4 bytes (`i32`) | 8 bytes (`i64`) |
| **`:i32`** | 32-bit Integer | Fixed-width 32-bit integer | 4 bytes (`i32`) | 4 bytes (`i32`) | 4 bytes (`i32`) | 4 bytes (`i32`, 8-byte slot) |
| **`:a`** | Address | Target-width memory pointer (`usize`) | 2 bytes (`u16`) | 3 bytes (`u24`) | 4 bytes (`u32`) | 8 bytes (`u64`) |
| **`:f`** | Float | Target-native floating-point | **6 bytes** (Z80) / **4 bytes** (6502) | **4 bytes** (IEEE-754 `binary32`) | **4 bytes** (IEEE-754 `binary32`) | **8 bytes** (IEEE-754 `binary64`) |

### 2.1 Literal Typing Rules
* **Bare Integer Literals** (`0`, `1`, `-42`, `0x10`, `$FF`) implicitly default to **`:i`**.
* **Bare Floating-Point Literals** (`3.14`, `-0.5`, `1e-3` — containing `.` or `e`/`E`) implicitly default to **`:f`**.
* **Literal Cast Folding:** Applying `i>i32`, `i>u32`, or `i>a` directly to an integer literal (e.g., `100000 i>i32`, `0x0400 i>a`) folds to a single `:i32` or `:a` push at compile time.
* **Literal Width Restriction:** In the current implementation, integer and address literals are restricted to 32-bit values (`-2147483648..2147483647` for `:i`, `0..0xFFFFFFFF` for `:a`), even on 64-bit targets.

## 3. Functions, Parameters & Locals

Functions are defined at the top level using `fn ... end` syntax with a `( ... -> ... )` signature header.

```forth
fn <name> ( <inputs> -> <outputs> )
  var <local> <type>
  <body>
end
```

* **Inputs (before `->` or `)`):**
  * Declared as `IDENT TYPE` pairs (e.g., `ptr :a count :i`) or `IDENT :bytes <size>` (e.g., `buf :bytes 16`, in `fn` definitions only). On entry, arguments are popped from the unified data stack in standard Forth order (rightmost parameter popped from the top of the stack) and bound to their local names. Unnamed input parameters are not permitted.
  * For `IDENT :bytes <size>` parameters, the caller passes an `:a` source pointer; on entry, the callee allocates a local `<size>`-byte buffer, copies `<size>` bytes from the source pointer into it, and binds `IDENT` to the copy's `:a` base address (pass-by-value semantics).
* **Local Variables (`var` inside function/`init` bodies):**
  * Declared anywhere in the function body as `var IDENT TYPE` (e.g., `var sum :i`) or `var IDENT :bytes <size>` for a stack-allocated byte buffer (e.g., `var tmp :bytes 16`).
  * Scalar locals do **not** pop from the caller's stack; they allocate local storage slots initialized to `0` (`0.0` for `:f`) once on function entry.
  * `:bytes <size>` locals allocate `<size>` bytes in the function's stack frame (freed on `end`/`return`), and referencing `<ident>` pushes its `:a` base address onto the stack.
* **Outputs (optional, after `->` and before `)`):**
  * A nameless sequence of `TYPE` tokens specifying the exact stack state required at `end` or `return` (e.g., `-> :i`, `-> :f`, `-> :a :i`, or omitted for `void`).
* **Forward Declarations (`fwd`):**
  * `fwd <name> ( <inputs> -> <outputs> )` declares a function signature before its `fn <name> ... end` definition (allowing mutual recursion); the later `fn` definition must match the declared input and output types. Forward declarations apply only to functions—`var` and `data` must be declared before use.
* **Initialization Blocks (`init ... end`):**
  * One or more top-level `init ... end` blocks may be defined across the program and `require`d modules. Each behaves as an anonymous void function (`( )`) and runs once in source order before `main`.
* **Machine-Code Words (`code ... end`):**
  * `code <name> ( <inputs> -> <outputs> ) <bytes> end` defines a raw machine-code word emitted into the executable code segment (subject to DCE) using `:i8`, `:i16`, `:i32`, `:ar` (16-bit address relative to the start of the `code` block), and `:as` (16-bit address relative to the 6502 data stack buffer), following the target's `call` / `return` convention.
* **Inline Machine Code (`inline ... end`):**
  * Inside a `fn` or `init` body, `inline <bytes> end` emits raw machine code bytes directly in place using the same `:i8`, `:i16`, `:i32`, `:ar` (relative to the start of the `inline` block), and `:as` tokens as `code`. It is assumed to be stack-neutral (`( -> )`) with respect to the static data stack, acts as an optimization/peephole fence, keeps the enclosing function native, and is not supported on the bytecode target (`-t bc`).

### 3.1 Accessing, Mutating & Addressing Locals
* **Read (`<ident>`):** Pushes the value of parameter or local `<ident>` (of its declared type `T`) onto the stack.
* **Write (`-> <ident>`):** Pops the top value from the stack (verifying its type matches `<ident>`'s declared type `T`) and stores it into `<ident>`.
* **Add-Assign (`+-> <ident>`):** Pops a delta (`:i` for `:i` or `:a` variables, `:i32` for `:i32`, `:f` for `:f`) and adds it in place to `<ident>`.
* **Declare & Initialize (`-> var <ident> :<type>`):** Declares a function-scoped local scalar (`:i`, `:i32`, `:a`, or `:f`) and initializes it by popping the top value from the stack (not valid for `:bytes`).
* **Address (`& <ident>`):** Pushes the memory address of parameter, local, typed global, or function `<ident>` onto the stack with type `:a` (functions referenced via `&` are retained by dead-code elimination).

### 3.2 Global Variables & Named Data Buffers (`var`)
Top-level writable globals and fixed-size buffers can be declared outside word definitions:

```forth
var <name> :i            \ Typed scalar global (:i, :i32, :a, or :f)
var <name> :bytes <size> \ Raw byte buffer
```

* Typed globals (`:i`, `:i32`, `:a`, `:f`) behave like locals: `<name>` reads the value, `-> <name>` assigns to it, `+-> <name>` adds to it, and `& <name>` pushes its `:a` address.
* `var <name> :bytes <size>` allocates `<size>` bytes aligned to `word_bytes`; referencing `<name>` pushes its `:a` base address (`( -> :a )`).
* **`__heap_start`:** Built-in constant pushing the `:a` address immediately following all static code, `data`, `var`, and target runtime data.

### 3.3 Compile-Time Constants (`const`)
Top-level compile-time constants can be declared outside word definitions:

```forth
const KEY_ESC 27
const PI 3.14159
default const SCREEN_W 80
```

* The value must be an integer literal (inferred as `:i`, checked against the target's `word_bytes` range) or a floating-point literal (inferred as `:f`).
* Referencing `<name>` inside a function pushes the literal directly onto the stack (participating in constant folding and immediate instruction fusion). Unlike `fn`, `const` cannot be used with `&`.
* Constants declared with `default const` may be overridden by a `const` with the same inferred type within the same `require` group.

### 3.4 Pre-Initialized Data (`data ... end`)
Top-level read-only tables and constants can be defined outside word definitions:

```forth
data <name>
  10 :i8 -5 :i8 1000 :i16
  "hi\n" 0 :i8
  align
  100 -200
  & my_var & my_fn
end
```

* `data <name> ... end` aligns the data pointer to the target's native word size and binds `<name>` (`( -- :a )`).
* `:i8` emits 1 byte (`-128..255`). `:i16` emits 2 little-endian bytes (`-32768..65535`). `:i32` emits 4 little-endian bytes (`-2147483648..4294967295`). `:i` (or a bare integer without a width specifier) emits 1 native word (`word_bytes`).
* `& <ident>` emits the `addr_bytes`-wide relocated address of a previously declared `var`, `data`, or `fn` (`fwd` allowed; functions referenced via `&` are retained by dead-code elimination).
* `"..."` emits raw string bytes (supporting `\n`, `\r`, `\t`, `\0`, `\\`, `\"` escapes) without an implicit NUL terminator.
* `align` advances the data pointer to the next multiple of `word_bytes`.
* `data` blocks are semantically read-only and must be declared before they are referenced.

### 3.5 Including Files (`require`)
Top-level `require` loads another `.ir4` source file at most once (include-once semantics):

```forth
require "<filename>"
```

* `<filename>` is resolved by searching roots in order: the directory of the file containing `require`, any `-I <dir>` directories in CLI order, and finally the compiler's `lib/` roots.
* Within the **first root** that contains at least one match, all matching specificity levels are loaded under a shared `require` group (most-specific parsed first):
  1. `<root>/<platform>/<filename>` and any parent platform directories (e.g. `-t x16` checks `x16/` then `c64/`, `-t agon` checks `agon/`)
  2. `<root>/<cpu>/<filename>` (e.g. `6502/i32.ir4`, `z80/mem.ir4`, `ez80/...`)
  3. `<root>/<filename>` (portable fallback)
* Functions declared with `default fn` (and constants declared with `default const`) may be overridden by a non-default definition with a matching signature or type within the same `require` group. Overrides outside that `require` group, duplicate `default` declarations, and duplicate non-default definitions are rejected.
* Repeated `require` directives for an already-loaded file path are ignored.

### 3.6 External Functions (`extern`)
Top-level `extern` declares an external platform/OS function:

```forth
extern putchar ( c :i -> :i )                  \ ARM64 JIT (dlsym, C ABI)
extern bdos 0x0005 ( C :i DE :i -> A :i )      \ Z80 CP/M (fixed addr + registers)
```

* Signatures use `( inputs -> outputs )` (or `( inputs )` for void) with at most 1 return value.
* On ARM64 JIT (`--run`), no address is given; symbols are resolved via `dlsym` and called using the ARM64 C ABI (`X0..X7`, `D0..D7`).
* On RISC-V RV32 (`--obj`), symbolic `extern` calls emit ELF relocations (`R_RISCV_HI20` / `R_RISCV_LO12_I`) using the `ilp32` C ABI (`a0..a7`); fixed-address `extern` declarations are also supported for bare-metal targets (`--virt`).
* On 16-bit targets (Z80 / 6502), a fixed 16-bit address is required and parameter/return names specify CPU registers (`A, B, C, D, E, H, L, BC, DE, HL` on Z80; `A, X, Y` on 6502).

## 4. Core Instruction Set

In stack-effect notation `( before -- after )`, the rightmost item is the top of the stack. All types—including `:f`—reside on the single unified stack.

### 4.1 Stack Manipulation
*(Stack primitives move existing typed slots without altering their types).*

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `dup` | `( t -- t t )` | Duplicate top stack item |
| `drop` | `( t -- )` | Discard top stack item |
| `swap` | `( t1 t2 -- t2 t1 )` | Exchange top two stack items |

### 4.2 Integer Arithmetic & Bitwise Operations (`:i` and `:i32`)
Native `:i` operators are unprefixed; `:i32` operators use an `i32` prefix (`u32` for unsigned division, remainder, and logical right shift):

| `:i` Op | `:i` Stack Effect | `:i32` Op | `:i32` Stack Effect | Description |
| :--- | :--- | :--- | :--- | :--- |
| `+` | `( :i :i -- :i )` | `i32+` | `( :i32 :i32 -- :i32 )` | Addition |
| `-` | `( :i :i -- :i )` | `i32-` | `( :i32 :i32 -- :i32 )` | Subtraction (`a - b`) |
| `*` | `( :i :i -- :i )` | `i32*` | `( :i32 :i32 -- :i32 )` | Multiplication |
| `/` | `( :i :i -- :i )` | `i32/` | `( :i32 :i32 -- :i32 )` | Signed division (`a / b`, truncating toward 0) |
| `mod` | `( :i :i -- :i )` | `i32mod` | `( :i32 :i32 -- :i32 )` | Signed remainder (`a mod b`) |
| `u/` | `( :i :i -- :i )` | `u32/` | `( :i32 :i32 -- :i32 )` | Unsigned division |
| `umod` | `( :i :i -- :i )` | `u32mod` | `( :i32 :i32 -- :i32 )` | Unsigned remainder |
| `and` | `( :i :i -- :i )` | `i32and` | `( :i32 :i32 -- :i32 )` | Bitwise AND |
| `or` | `( :i :i -- :i )` | `i32or` | `( :i32 :i32 -- :i32 )` | Bitwise OR |
| `xor` | `( :i :i -- :i )` | `i32xor` | `( :i32 :i32 -- :i32 )` | Bitwise XOR |
| `invert` | `( :i -- :i )` | `i32invert` | `( :i32 -- :i32 )` | Bitwise one's complement |
| `<<` | `( :i :i -- :i )` | `i32<<` | `( :i32 :i -- :i32 )` | Logical shift left (`a << b`) |
| `>>` | `( :i :i -- :i )` | `i32>>` | `( :i32 :i -- :i32 )` | Arithmetic (sign-preserving) shift right (`a >> b`) |
| `u>>` | `( :i :i -- :i )` | `u32>>` | `( :i32 :i -- :i32 )` | Logical (zero-filling) shift right (`a u>> b`) |

### 4.3 Address Arithmetic (`:a`)
Because `:i` and `:a` always share the same register width, address offsets require no sign-extension.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `a+` | `( :a :i -- :a )` | Add byte offset `:i` to address `:a` |
| `a-` | `( :a :i -- :a )` | Subtract byte offset `:i` from address `:a` |
| `adiff` | `( :a :a -- :i )` | Signed byte distance between two addresses (`a1 - a2`) |

### 4.4 Floating-Point Operations (`:f`)

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `f+` | `( :f :f -- :f )` | Floating-point addition |
| `f-` | `( :f :f -- :f )` | Floating-point subtraction (`a - b`) |
| `f*` | `( :f :f -- :f )` | Floating-point multiplication |
| `f/` | `( :f :f -- :f )` | Floating-point division (`a / b`) |
| `fnegate` | `( :f -- :f )` | Floating-point negation (`-a`) |

### 4.5 Comparisons (`:i`, `:i32`, `:a`, and `:f`)
All comparisons consume two values and push a boolean flag of type **`:i`** (`-1` for true, `0` for false).

| Instruction | Stack Effect | Semantics |
| :--- | :--- | :--- |
| `=` , `<>` | `( :i :i -- :i )` | Native word equal / not equal |
| `<` , `>` , `<=` , `>=` | `( :i :i -- :i )` | Signed native word comparisons |
| `u<` , `u>` , `u<=` , `u>=` | `( :i :i -- :i )` | Unsigned native word comparisons |
| `i32=` , `i32<>` | `( :i32 :i32 -- :i )` | 32-bit integer equal / not equal |
| `i32<` , `i32>` , `i32<=` , `i32>=` | `( :i32 :i32 -- :i )` | Signed 32-bit integer comparisons |
| `u32<` , `u32>` , `u32<=` , `u32>=` | `( :i32 :i32 -- :i )` | Unsigned 32-bit integer comparisons |
| `a=` , `a<>` , `a<` , `a>` | `( :a :a -- :i )` | Address equality and unsigned pointer comparisons |
| `f=` , `f<>` , `f<` , `f>` | `( :f :f -- :i )` | Floating-point comparisons |

### 4.6 Memory Load & Store (Standard Forth Operators)
Fixed-width memory operations (`u8@`, `i8@`, `i8!`, `u16@`, `i16@`, `i16!`) widen to `:i` on load and truncate the low bits of `:i` on store.

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `i8@` | `( :a -- :i )` | Fetch signed 8-bit byte from address, sign-extended to `:i` |
| `u8@` | `( :a -- :i )` | Fetch unsigned 8-bit byte from address, zero-extended to `:i` |
| `i8!` | `( :i :a -- )` | Store low 8 bits of `:i` to address |
| `i16@` | `( :a -- :i )` | Fetch signed 16-bit integer from address, sign-extended to `:i` |
| `u16@` | `( :a -- :i )` | Fetch unsigned 16-bit integer from address, zero-extended to `:i` |
| `i16!` | `( :i :a -- )` | Store low 16 bits of `:i` to address |
| `i32@` | `( :a -- :i32 )` | Fetch 32-bit integer (`:i32`) from address |
| `i32!` | `( :i32 :a -- )` | Store 32-bit integer (`:i32`) to address |
| `@` | `( :a -- :i )` | Fetch native word (`:i`) from address |
| `!` | `( :i :a -- )` | Store native word (`:i`) to address |
| `a@` | `( :a -- :a )` | Fetch target-width pointer (`:a`) from address |
| `a!` | `( :a :a -- )` | Store target-width pointer (`:a`) to address |
| `f@` | `( :a -- :f )` | Fetch target-width float (`:f`) from address |
| `f!` | `( :f :a -- )` | Store target-width float (`:f`) to address |

### 4.7 Explicit Type Conversions

| Instruction | Stack Effect | Description |
| :--- | :--- | :--- |
| `sext` | `( :i -- :i )` | Sign-extend low 8 bits of `:i` to full word width `:i` |
| `i>a` | `( :i -- :a )` | Reinterpret native word `:i` as address `:a` |
| `a>i` | `( :a -- :i )` | Reinterpret address `:a` as native word `:i` |
| `i>f` | `( :i -- :f )` | Convert signed native word `:i` to float `:f` |
| `f>i` | `( :f -- :i )` | Truncate float `:f` to signed native word `:i` |
| `i>i32` | `( :i -- :i32 )` | Sign-extend (or truncate on 64-bit) `:i` to `:i32` |
| `i>u32` | `( :i -- :i32 )` | Zero-extend (or truncate on 64-bit) `:i` to `:i32` |
| `i32>i` | `( :i32 -- :i )` | Sign-extend (or truncate on 16-bit) `:i32` to `:i` |

## 5. Control Flow & Static Stack Invariants

* **Conditional Branches (`<cond :i> if ... else ... then`):** Pops a `:i` flag. Both branches must leave the type stack with identical depth and types. If `else` is omitted, the true branch must leave the stack completely even (unchanged).
* **Pre-Tested Loops (`begin <cond :i> while ... repeat`):** `while` pops a `:i` flag. The combined condition and loop body must leave the type stack completely even at `repeat`.
* **Labels & Branches (`:: <name>`, `jmp <name>`, `<cond :i> jz <name>`):** Function-local labels and forward/backward branches. The type stack must be empty at `::`, at `jmp`, and after popping `:i` at `jz`; `::` cannot appear inside an open `if` or `begin` block.
* **Calls & Early Return (`<word_name>`, `( <inputs> -> <outputs> )`, `return`):** Direct calls (`<word_name>`) validate the caller stack against callee inputs and push outputs. Indirect calls (`<args...> <fn :a> ( <inputs> -> <outputs> )`, where `<inputs>` and `<outputs>` are nameless type tokens) pop the `:a` target address and declared input types, call the address, and push the declared output types. `return` verifies the stack matches the word's declared output types after `->`.

For complete runnable programs, see [examples/](examples/) and [tests/](tests/).
