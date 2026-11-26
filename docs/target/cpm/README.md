# CP/M (`-t cpm`)

## Compiler Flags

```sh
./build/ir4c -t cpm -o out.com input.ir4
```

- **Target / Platform:** `-t cpm` (implies `-t z80`, platform name `"cpm"`)
- **Base address:** `0x0100` (standard CP/M `.COM` TPA transient program area)
- **Fallback chain:** `lib/cpm/` → `lib/z80/` → `lib/`

## Memory Layout

- **`0x0005`:** CP/M BDOS entry point (used for console I/O and exit).
- **`0x0100+`:** `JP entry` followed by compiled code, data, BSS, and heap.
- **`main` signature:** 0 inputs, at most 1 output (return value printed on exit).
- **Filename note:** CP/M emulators like `tnylpo` enforce 8.3 `.COM` filenames (max 8 chars before `.com`).

## Assumptions & Notes

- **Terminal (`lib/cpm/stdio.ir4`):** VT52 cursor/screen control on `80×24`.
- **Graphics (`lib/cpm/graphics.ir4`):** `80×24` VT52 ASCII luminance (`" .:+*%#@"`, 8 shades, `gfxdepth = 3`).
- **Running in `tnylpo`:**
  ```sh
  tnylpo out.com
  ```
