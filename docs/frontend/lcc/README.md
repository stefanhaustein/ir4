# LCC (C89) Frontend

[LCC](https://github.com/stefanhaustein/lcc) (forked from Chris Fraser and David Hanson's retargetable C89 compiler, version 4.2) is included as an example of how to cleanly integrate an IR4(th) backend into an existing compiler.

The IR4(th) target implementation lives in [src/ir4.c](https://github.com/stefanhaustein/lcc/blob/master/src/ir4.c) and provides 16-, 32-, and 64-bit targets (`ir4` / `ir4/16`, `ir4/32`, and `ir4/64`).

## 1. Prerequisites

1. **Build `ir4c`** from the IR4(th) root directory:
   ```sh
   make
   ```
2. **A host C compiler** (`cc`, Clang, or GCC) to build `rcc` and run the C preprocessor (`cc -E`).
3. **Target emulators/runtimes** (optional, depending on target):
   - ARM64 JIT: built into `ir4c` on Apple Silicon macOS (`-t arm64 --run`)
   - 6502/65C02: `sim65` (from cc65), `x64sc` (VICE), or `x16emu`
   - eZ80 / Agon Light: `agon-cli-emulator`

## 2. Clone and Build

Clone the LCC fork inside your IR4(th) checkout (the `lcc/` directory is gitignored by IR4(th) so both repositories remain independent):

```sh
git clone https://github.com/stefanhaustein/lcc.git
```

Build the compiler (`rcc`):

```sh
mkdir -p lcc/build
make -C lcc CC="cc -std=gnu89 -Wno-implicit-function-declaration" BUILDDIR="$(pwd)/lcc/build" rcc
```

> **Note:** On modern Clang/macOS, `-std=gnu89 -Wno-implicit-function-declaration` is required for `lburg` and LCC's C89 sources. Using `cc -E` for preprocessing avoids building LCC's bundled `cpp`.

## 3. Hello World

Create `hello.c`:

```c
#include <stdio.h>

int main(void) {
    printf("Hello from C89 on IR4(th)!\n");
    return 0;
}
```

Preprocess, compile to IR4(th), and run with the ARM64 JIT:

```sh
cc -E -P -nostdinc -Ilcc/include/ir4 hello.c -o hello.i
./lcc/build/rcc -target=ir4/64 hello.i hello.ir4
./build/ir4c -t arm64 -I lcc/lib -I lib --run hello.ir4
```

Generated `.ir4` files automatically `require "c.ir4"` and `require "libc<width>.ir4"`, pulling in the runtime bindings in `lcc/lib/c.ir4` and the C89 library helpers in `lcc/lib/libc.c`.

## 4. Targets & Standard Library

Select the `rcc` target to match the word/pointer width of your IR4(th) target:

| LCC Target | `int` / Pointer | `long` / `float` | Example `ir4c` Targets |
| --- | --- | --- | --- |
| `ir4` / `ir4/16` | 16-bit | 32-bit | `sim65`, `c64`, `x16`, `cpm`, `zx81` |
| `ir4/32` | 32-bit | 32-bit | `agon`, `rv32` |
| `ir4/64` | 64-bit | 64-bit (`double`) | `arm64`, `x86_64` |

Example 16-bit (`sim65`) and 32-bit (`agon`) builds:

```sh
# 16-bit sim65
./lcc/build/rcc -target=ir4 hello.i hello16.ir4
./build/ir4c -t sim65 -I lcc/lib -I lib hello16.ir4 -o hello.prg
sim65 hello.prg

# 32-bit Agon Light
./lcc/build/rcc -target=ir4/32 hello.i hello32.ir4
./build/ir4c -t agon -I lcc/lib -I lib hello32.ir4 -o hello.bin
```

Available C89 headers in `lcc/include/ir4/`:
`assert.h`, `ctype.h`, `errno.h`, `float.h`, `limits.h`, `math.h`, `stdarg.h`, `stddef.h`, `stdio.h`, `stdlib.h`, `string.h`, `time.h`.

If you modify `lcc/lib/libc.c`, regenerate the prebuilt `libc16.ir4`, `libc32.ir4`, and `libc64.ir4` libraries:

```sh
cc -E -P -nostdinc -Ilcc/include/ir4 lcc/lib/libc.c -o /tmp/libc.i
./lcc/build/rcc -target=ir4    /tmp/libc.i lcc/lib/libc16.ir4
./lcc/build/rcc -target=ir4/32 /tmp/libc.i lcc/lib/libc32.ir4
./lcc/build/rcc -target=ir4/64 /tmp/libc.i lcc/lib/libc64.ir4
```
