; 6502 memory primitives for IR4th (converted via tools/asm2ir4.py).
; Preserves X (IR4th data stack pointer). Uses ZP $02..$08.

P0 = $02
P1 = $03
Q0 = $04
Q1 = $05
L0 = $06
L1 = $07
T0 = $08

; @code memfill ( dst :a len :i val :i )
    LDA STACK+0,X
    STA T0
    INX
    INX
    LDA STACK+0,X
    STA L0
    LDA STACK+1,X
    STA L1
    INX
    INX
    LDA STACK+0,X
    STA P0
    LDA STACK+1,X
    STA P1
    INX
    INX
    LDA L1
    BMI fill_done
    ORA L0
    BEQ fill_done
    LDA T0
    LDY L1
    BEQ fill_tail
    LDY #0
fill_page:
    STA (P0),Y
    INY
    BNE fill_page
    INC P1
    DEC L1
    BNE fill_page
fill_tail:
    LDY L0
    BEQ fill_done
    LDY #0
fill_tloop:
    STA (P0),Y
    INY
    CPY L0
    BNE fill_tloop
fill_done:
    RTS

; @code memcpy ( dst :a src :a len :i )
    LDA STACK+0,X
    STA L0
    LDA STACK+1,X
    STA L1
    INX
    INX
    LDA STACK+0,X
    STA P0
    LDA STACK+1,X
    STA P1
    INX
    INX
    LDA STACK+0,X
    STA Q0
    LDA STACK+1,X
    STA Q1
    INX
    INX
    LDA L1
    BMI copy_done
    ORA L0
    BEQ copy_done
    SEC
    LDA P0
    SBC Q0
    LDA P1
    SBC Q1
    BCS copy_fwd
    ; Backward copy (dst > src)
    CLC
    LDA P1
    ADC L1
    STA P1
    CLC
    LDA Q1
    ADC L1
    STA Q1
    LDY L0
    BEQ bk_pages
    DEY
bk_tloop:
    LDA (P0),Y
    STA (Q0),Y
    DEY
    CPY #$FF
    BNE bk_tloop
bk_pages:
    LDA L1
    BEQ copy_done
bk_ploop:
    DEC P1
    DEC Q1
    LDY #$FF
bk_iloop:
    LDA (P0),Y
    STA (Q0),Y
    DEY
    CPY #$FF
    BNE bk_iloop
    DEC L1
    BNE bk_ploop
    RTS
copy_fwd:
    LDA L1
    BEQ fwd_tail
    LDY #0
fwd_ploop:
    LDA (P0),Y
    STA (Q0),Y
    INY
    BNE fwd_ploop
    INC P1
    INC Q1
    DEC L1
    BNE fwd_ploop
fwd_tail:
    LDA L0
    BEQ copy_done
    LDY #0
fwd_tloop:
    LDA (P0),Y
    STA (Q0),Y
    INY
    CPY L0
    BNE fwd_tloop
copy_done:
    RTS

; @code memcmp ( a :a b :a len :i -> :i )
    LDA STACK+0,X
    STA L0
    LDA STACK+1,X
    STA L1
    INX
    INX
    LDA STACK+0,X
    STA Q0
    LDA STACK+1,X
    STA Q1
    INX
    INX
    LDA STACK+0,X
    STA P0
    LDA STACK+1,X
    STA P1
    LDA L1
    BMI cmp_zero
    ORA L0
    BEQ cmp_zero
    LDA L1
    BEQ cmp_tail
    LDY #0
cmp_ploop:
    LDA (P0),Y
    CMP (Q0),Y
    BNE cmp_diff
    INY
    BNE cmp_ploop
    INC P1
    INC Q1
    DEC L1
    BNE cmp_ploop
cmp_tail:
    LDA L0
    BEQ cmp_zero
    LDY #0
cmp_tloop:
    LDA (P0),Y
    CMP (Q0),Y
    BNE cmp_diff
    INY
    CPY L0
    BNE cmp_tloop
cmp_zero:
    LDA #0
    STA STACK+0,X
    STA STACK+1,X
    RTS
cmp_diff:
    SEC
    LDA (P0),Y
    SBC (Q0),Y
    STA STACK+0,X
    LDA #0
    SBC #0
    STA STACK+1,X
    RTS
