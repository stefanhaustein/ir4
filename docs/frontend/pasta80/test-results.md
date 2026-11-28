# PASTA/80 Cross-Target Test Results

Automated results from running the PASTA/80 v1.00 test suites across IR4(th) targets with pasta84/selftest.py.

## Results Matrix (Passed Assertions)

| ⁉️ | Target | core | heap | files | more | prm | snd | ovl | Total | Time |
| :---: | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| ℹ️ | Expected | 1572 | 26 | 194 | 49 | 6 | 4 | 32 | 1883 |  |
| 🅾️ | [Agon](#agon) | 1553 | 17 | 192 | 45 | 6 | 4 | 32 | 1849 | 16.10s |
| 🅾️ | [ARM64](#arm64) | 1549 | 17 | 194 | 45 | 5 | 4 | 32 | 1846 | 4.71s |
| 🅾️ | [C64](#c64) | n/a | 17 | ??? | 49 | n/a | 4 | 32 | \*102 | 99.14s |
| 🅾️ | [CP/M](#cpm) | n/a | ??? | \*180 | 49 | 6 | 4 | 32 | \*271 | 30.97s |
| 🅾️ | [RV32](#rv32) | 1538 | 17 | n/a | 45 | n/a | 4 | 32 | \*1636 | 0.54s |
| 🅾️ | [Web](#web) | 1550 | 17 | n/a | 45 | n/a | 4 | 32 | \*1648 | 1.00s |
| 🅾️ | [X16](#x16) | n/a | 17 | n/a | 49 | n/a | 4 | 32 | \*102 | 5.84s |
| 🅾️ | [X16GS](#x16gs) | 1572 | 17 | 194 | 49 | 6 | 4 | 32 | 1874 | 11.09s |
| 🅾️ | [ZX81](#zx81) | n/a | ??? | n/a | ??? | n/a | 4 | ??? | \*4 | 44.47s |

## Notes

- 16-bit integer targets (C64, CP/M, X16, X16GS, ZX81): Match Turbo Pascal 3's 16-bit Integer width (SizeOf(Integer) = 2 and 16-bit signed overflow wraparound).
- 64 KB RAM targets (C64, CP/M, X16, ZX81): core exceeds the 64 KB address space without bank switching or disk overlays.
- 24/32/64-bit integer targets (Agon, ARM64, RV32, Web): The IR4(th) backend currently maps Pascal Integer to IR4(th)'s native :i cell (3, 4, or 8 bytes), which accounts for the core and more assertions that test SizeOf(Integer) = 2 or 16-bit overflow wraparound (plus ParamStr(0) = '' on POSIX ARM64).
- Sandbox / bare-metal targets (RV32, Web): files and params are marked n/a where no host filesystem or CLI argument vector is exposed.

## Reproducing

```sh
python3 pasta84/selftest.py                # run all available targets
python3 pasta84/selftest.py -t x16gs       # run a single target
python3 pasta84/selftest.py --update-docs  # refresh this page
```

## Individual Target Reports

### Agon

Target flag: -t agon — eZ80 ADL (24-bit int, 24-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> Agon

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core      1553        19      1572
    heap        17         9        26
   files       192         2       194
    more        45         4        49
  params         6         0         6
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total      1849        34      1883

              [  FAIL  ]

Tests finished (16.103s).

Failed assertions:
  core.pas:174 (TestConst)
  core.pas:201 (TestAdd)
  core.pas:216 (TestSubtract)
  core.pas:276 (TestShift)
  core.pas:282 (TestShift)
  core.pas:524 (TestBitOps)
  core.pas:525 (TestBitOps)
  core.pas:926 (TestVariantRecords)
  core.pas:934 (TestVariantRecords)
  core.pas:973 (TestVariantRecords)
  core.pas:974 (TestVariantRecords)
  core.pas:975 (TestVariantRecords)
  core.pas:976 (TestVariantRecords)
  core.pas:977 (TestVariantRecords)
  core.pas:2315 (TestSizeOf)
  core.pas:2320 (TestSizeOf)
  core.pas:2321 (TestSizeOf)
  core.pas:2323 (TestSizeOf)
  core.pas:2329 (TestSizeOf)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  files.pas:492 (TestTextReadIOResult)
  files.pas:585 (TestTextWithReals)
  more.pas:112 (TestSizeOfSubranges)
  more.pas:114 (TestSizeOfSubranges)
  more.pas:150 (TestNegativeLiteralBounds)
  more.pas:222 (TestForByteLimit)
```

### ARM64

Target flag: -t arm64 — AArch64 JIT (64-bit int, 64-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

      Darwin (arm64) ---> ARM64

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core      1549        23      1572
    heap        17         9        26
   files       194         0       194
    more        45         4        49
  params         5         1         6
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total      1846        37      1883

              [  FAIL  ]

Tests finished (4.708s).

Failed assertions:
  core.pas:174 (TestConst)
  core.pas:201 (TestAdd)
  core.pas:216 (TestSubtract)
  core.pas:276 (TestShift)
  core.pas:282 (TestShift)
  core.pas:524 (TestBitOps)
  core.pas:525 (TestBitOps)
  core.pas:926 (TestVariantRecords)
  core.pas:934 (TestVariantRecords)
  core.pas:973 (TestVariantRecords)
  core.pas:974 (TestVariantRecords)
  core.pas:975 (TestVariantRecords)
  core.pas:976 (TestVariantRecords)
  core.pas:977 (TestVariantRecords)
  core.pas:2709 (TestReal)
  core.pas:2746 (TestReal)
  core.pas:2749 (TestReal)
  core.pas:2751 (TestReal)
  core.pas:2315 (TestSizeOf)
  core.pas:2320 (TestSizeOf)
  core.pas:2321 (TestSizeOf)
  core.pas:2323 (TestSizeOf)
  core.pas:2329 (TestSizeOf)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  more.pas:112 (TestSizeOfSubranges)
  more.pas:114 (TestSizeOfSubranges)
  more.pas:150 (TestNegativeLiteralBounds)
  more.pas:222 (TestForByteLimit)
  params.pas:27
```

### C64

Target flag: -t c64 — 6502 (16-bit int, 16-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> C64

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core       n/a       n/a       n/a
    heap        17         9        26
   files       ???       ???       ???
    more        49         0        49
  params       n/a       n/a       n/a
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total       102         9       111

              [  FAIL  ]

Tests finished (99.138s).

Failed assertions:
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
```

### CP/M

Target flag: -t cpm — Z80 (16-bit int, 16-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> CP/M

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core       n/a       n/a       n/a
    heap       ???       ???       ???
   files       180        15       195
    more        49         0        49
  params         6         0         6
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total       271        15       286

              [  FAIL  ]

Tests finished (30.970s).

Failed assertions:
  files.pas:386 (TestTextWithStrings)
  files.pas:402 (TestTextWithStrings)
  files.pas:919 (TestTypedFileIO)
  files.pas:944 (TestTypedFileIO)
  files.pas:944 (TestTypedFileIO)
  files.pas:944 (TestTypedFileIO)
  files.pas:944 (TestTypedFileIO)
  files.pas:957 (TestTypedFileIO)
  files.pas:966 (TestTypedFileIO)
  files.pas:971 (TestTypedFileIO)
  files.pas:1064 (TestLargeFiles)
  files.pas:1085 (TestLargeFiles)
  files.pas:1096 (TestLargeFiles)
  files.pas:1124 (TestLargeFiles)
  files.pas:1125 (TestLargeFiles)
```

### RV32

Target flag: -t virt — QEMU RISC-V virt (32-bit int, 32-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> RV32

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core      1538        34      1572
    heap        17         9        26
   files       n/a       n/a       n/a
    more        45         4        49
  params       n/a       n/a       n/a
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total      1636        47      1683

              [  FAIL  ]

Tests finished (0.537s).

Failed assertions:
  core.pas:174 (TestConst)
  core.pas:201 (TestAdd)
  core.pas:216 (TestSubtract)
  core.pas:276 (TestShift)
  core.pas:282 (TestShift)
  core.pas:524 (TestBitOps)
  core.pas:525 (TestBitOps)
  core.pas:926 (TestVariantRecords)
  core.pas:934 (TestVariantRecords)
  core.pas:973 (TestVariantRecords)
  core.pas:974 (TestVariantRecords)
  core.pas:975 (TestVariantRecords)
  core.pas:976 (TestVariantRecords)
  core.pas:977 (TestVariantRecords)
  core.pas:2684 (TestReal)
  core.pas:2746 (TestReal)
  core.pas:2749 (TestReal)
  core.pas:2751 (TestReal)
  core.pas:2315 (TestSizeOf)
  core.pas:2320 (TestSizeOf)
  core.pas:2321 (TestSizeOf)
  core.pas:2323 (TestSizeOf)
  core.pas:2329 (TestSizeOf)
  core.pas:3202 (TestStr)
  core.pas:3203 (TestStr)
  core.pas:3206 (TestStr)
  core.pas:3207 (TestStr)
  core.pas:3208 (TestStr)
  core.pas:3223 (TestStr)
  core.pas:3224 (TestStr)
  core.pas:3225 (TestStr)
  core.pas:3226 (TestStr)
  core.pas:3229 (TestStr)
  core.pas:3230 (TestStr)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  more.pas:112 (TestSizeOfSubranges)
  more.pas:114 (TestSizeOfSubranges)
  more.pas:150 (TestNegativeLiteralBounds)
  more.pas:222 (TestForByteLimit)
```

### Web

Target flag: -t web — Bytecode VM bc32 (32-bit int, 32-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> Web

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core      1550        22      1572
    heap        17         9        26
   files       n/a       n/a       n/a
    more        45         4        49
  params       n/a       n/a       n/a
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total      1648        35      1683

              [  FAIL  ]

Tests finished (0.996s).

Failed assertions:
  core.pas:174 (TestConst)
  core.pas:201 (TestAdd)
  core.pas:216 (TestSubtract)
  core.pas:276 (TestShift)
  core.pas:282 (TestShift)
  core.pas:524 (TestBitOps)
  core.pas:525 (TestBitOps)
  core.pas:926 (TestVariantRecords)
  core.pas:934 (TestVariantRecords)
  core.pas:973 (TestVariantRecords)
  core.pas:974 (TestVariantRecords)
  core.pas:975 (TestVariantRecords)
  core.pas:976 (TestVariantRecords)
  core.pas:977 (TestVariantRecords)
  core.pas:2746 (TestReal)
  core.pas:2749 (TestReal)
  core.pas:2751 (TestReal)
  core.pas:2315 (TestSizeOf)
  core.pas:2320 (TestSizeOf)
  core.pas:2321 (TestSizeOf)
  core.pas:2323 (TestSizeOf)
  core.pas:2329 (TestSizeOf)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  more.pas:112 (TestSizeOfSubranges)
  more.pas:114 (TestSizeOfSubranges)
  more.pas:150 (TestNegativeLiteralBounds)
  more.pas:222 (TestForByteLimit)
```

### X16

Target flag: -t x16 — 65C02 (16-bit int, 16-bit addr, Low RAM .prg)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> X16

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core       n/a       n/a       n/a
    heap        17         9        26
   files       n/a       n/a       n/a
    more        49         0        49
  params       n/a       n/a       n/a
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total       102         9       111

              [  FAIL  ]

Tests finished (5.843s).

Failed assertions:
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
```

### X16GS

Target flag: -t x16gs — W65C816 (16-bit int, 24-bit addr)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

      Darwin (arm64) ---> X16GS

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core      1572         0      1572
    heap        17         9        26
   files       194         0       194
    more        49         0        49
  params         6         0         6
   sound         4         0         4
overlays        32         0        32
--------  --------  --------  --------
   Total      1874         9      1883

              [  FAIL  ]

Tests finished (11.094s).

Failed assertions:
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
  heap.pas:118 (TestStress)
```

### ZX81

Target flag: -t zx81 — Z80 (16-bit int, 16-bit addr, 48 KB RAM)

```text
======================================
Results of PASTA/80 version 1.00 tests
======================================

       Darwin (arm64) ---> ZX81

   Tests    Passed    Failed     Total
--------  --------  --------  --------
    core       n/a       n/a       n/a
    heap       ???       ???       ???
   files       n/a       n/a       n/a
    more       ???       ???       ???
  params       n/a       n/a       n/a
   sound         4         0         4
overlays       ???       ???       ???
--------  --------  --------  --------
   Total         4         0         4

              [  FAIL  ]

Tests finished (44.473s).
```
