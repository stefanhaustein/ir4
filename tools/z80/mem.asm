; Z80 memory primitives for IR4th (converted via tools/asm2ir4.py).
; Uses IY for return address; zero relocations.

; @code memfill ( dst :a len :i val :i )
    POP IY
    POP DE              ; E = val
    POP BC              ; BC = len
    POP HL              ; HL = dst
    BIT 7, B
    JR NZ, fill_done
    LD A, B
    OR C
    JR Z, fill_done
    LD (HL), E
    DEC BC
    LD A, B
    OR C
    JR Z, fill_done
    LD D, H
    LD E, L
    INC DE
    LDIR
fill_done:
    JP (IY)

; @code memcpy ( dst :a src :a len :i )
    POP IY
    POP BC              ; BC = len
    POP HL              ; HL = src
    POP DE              ; DE = dst
    BIT 7, B
    JR NZ, copy_done
    LD A, B
    OR C
    JR Z, copy_done
    OR A
    PUSH HL
    SBC HL, DE          ; src - dst; C set if dst > src
    POP HL
    JR NC, copy_fwd
    ADD HL, BC
    DEC HL
    EX DE, HL
    ADD HL, BC
    DEC HL
    EX DE, HL
    LDDR
    JP (IY)
copy_fwd:
    LDIR
copy_done:
    JP (IY)

; @code memcmp ( a :a b :a len :i -> :i )
    POP IY
    POP BC              ; BC = len
    POP DE              ; DE = b
    POP HL              ; HL = a
    BIT 7, B
    JR NZ, cmp_zero
    LD A, B
    OR C
    JR Z, cmp_zero
cmp_loop:
    LD A, (DE)
    CP (HL)
    JR NZ, cmp_diff
    INC HL
    INC DE
    DEC BC
    LD A, B
    OR C
    JR NZ, cmp_loop
cmp_zero:
    LD HL, 0
    PUSH HL
    JP (IY)
cmp_diff:
    LD A, (HL)
    EX DE, HL
    SUB (HL)            ; A = *a - *b, C set if *a < *b
    LD L, A
    SBC A, A            ; 0xFF if *a < *b, else 0x00
    LD H, A
    PUSH HL
    JP (IY)
