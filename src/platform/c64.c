/* c64.c - Commodore 64 (.prg) BASIC SYS stub & KERNAL CHROUT epilogue (Strict C89) */
#include "codebuf.h"
#include "frontend.h"
#include "backend/6502.h"

#define C64_CHROUT 0xFFD2
#define C64_TMP_LO 0xFB
#define C64_TMP_HI 0xFC

static u16 c64_jmp_pc = 0;

void be_6502_cbm_begin(size_t base, int is_x16) {
    char digits[8];
    int len = 0;
    int i;
    u16 stub_end;
    u16 next_line;

    if (is_x16) {
        be_6502_set_ram_top(0x9F00);
        be_6502_set_default_zp(0x7C);
        be_6502_set_default_stack_zp(0x22, 0x5A);
    } else {
        be_6502_set_ram_top(0xCC00);
        be_6502_set_default_stack_zp(0x20, 0x70);
    }

    /* 10 SYS <addr> : 2-byte next_line + 2-byte line_num + 1-byte SYS(0x9E) + digits + NUL + 2-byte
     * end */
    sprintf(digits, "%u", (unsigned)(base + 12));
    while (digits[len])
        len++;
    if (len != 4) {
        /* Adjust for 3- or 5-digit addresses */
        sprintf(digits, "%u", (unsigned)(base + 8 + len));
        len = 0;
        while (digits[len])
            len++;
    }
    stub_end = (u16)(base + 8 + len);
    next_line = (u16)(base + 6 + len);

    cb_emit16(next_line);
    cb_emit16(10);
    cb_emit8(0x9E); /* BASIC SYS token */
    for (i = 0; i < len; i++)
        cb_emit8((u8)digits[i]);
    cb_emit8(0x00);
    cb_emit16(0x0000);
    (void)stub_end;

    if (!is_x16) {
        /* Unmap BASIC ROM ($A000-$BFFF -> RAM) while keeping KERNAL/I/O */
        cb_emit8_8(M6502_LDA_ZP, 0x01);
        cb_emit8_8(M6502_AND_IMM, 0xFE);
        cb_emit8_8(M6502_STA_ZP, 0x01);
    }
    cb_emit8(M6502_JMP);
    c64_jmp_pc = cb_pc();
    cb_emit16(0x0000);
}

static void c64_begin(size_t base) { be_6502_cbm_begin(base, 0); }

int be_6502_cbm_write(FILE *fp, size_t base) {
    u8 hdr[2];
    cb_put_le16(hdr, base);
    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
        return 0;
    return cb_write_raw(fp, 1);
}

void be_6502_cbm_end(int main_idx, int is_x16) {
    const FuncSig *sig = ir4_func_sig((u8)main_idx);
    u16 entry_addr = cb_cur_addr();
    u8 tmp_lo = is_x16 ? 0x02 : C64_TMP_LO;
    u8 tmp_hi = is_x16 ? 0x03 : C64_TMP_HI;
    u16 putchar_call1 = 0;
    u16 putchar_call2 = 0;
    u16 putchar_call3 = 0;
    u16 print_call1 = 0;
    u16 print_call2 = 0;
    u16 print_u16_addr;
    u16 div_loop_pc;

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "%s requires 'main' with 0 inputs and at most 1 output\n",
                is_x16 ? "--x16" : "--c64");
        exit(1);
    }

    cb_patch16(c64_jmp_pc, entry_addr);

    /* CLD; LDX #init_x (initialize IR4th data stack without clobbering hardware SP) */
    cb_emit8(M6502_CLD);
    cb_emit8_8(M6502_LDX_IMM, be_6502_stack_zp_init_x());

    /* JSR main */
    cb_emit8_16(M6502_JSR, sig->addr);

    if (sig->out_types.len == 1) {
        u8 zp_stk = be_6502_stack_zp_base();
        /* Pop 16-bit signed value from (stack_base,X) into tmp_lo/tmp_hi */
        if (zp_stk != 0) {
            cb_emit8_8(0xB5, zp_stk); /* LDA zp,X */
            cb_emit8_8(M6502_STA_ZP, tmp_lo);
            cb_emit8_8(0xB5, (u8)(zp_stk + 1)); /* LDA zp+1,X */
            cb_emit8_8(M6502_STA_ZP, tmp_hi);
        } else {
            u16 stk_off = be_6502_stack_data_off();
            cb_emit8(M6502_LDA_ABSX);
            cb_add_data_reloc(cb_pc(), stk_off);
            cb_emit16(0x0000);
            cb_emit8_8(M6502_STA_ZP, tmp_lo);
            cb_emit8(M6502_LDA_ABSX);
            cb_add_data_reloc(cb_pc(), (u16)(stk_off + 1));
            cb_emit16(0x0000);
            cb_emit8_8(M6502_STA_ZP, tmp_hi);
        }
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);

        /* If tmp_hi < 0, print '-' and negate tmp_lo/tmp_hi */
        cb_emit8_8(M6502_LDA_ZP, tmp_hi);
        cb_emit8_8(M6502_BPL, 18);

        cb_emit8_8(M6502_LDA_IMM, '-');
        cb_emit8(M6502_JSR);
        putchar_call1 = cb_pc();
        cb_emit16(0x0000);
        cb_emit8(M6502_SEC);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_SBC_ZP, tmp_lo);
        cb_emit8_8(M6502_STA_ZP, tmp_lo);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_SBC_ZP, tmp_hi);
        cb_emit8_8(M6502_STA_ZP, tmp_hi);

        /* JSR print_u16 */
        cb_emit8(M6502_JSR);
        print_call1 = cb_pc();
        cb_emit16(0x0000);

        /* Print PETSCII CR (13) */
        cb_emit8_8(M6502_LDA_IMM, 13);
        cb_emit8(M6502_JSR);
        putchar_call2 = cb_pc();
        cb_emit16(0x0000);
    }

    if (!is_x16) {
        /* Restore BASIC ROM before returning to READY. */
        cb_emit8_8(M6502_LDA_ZP, 0x01);
        cb_emit8_8(M6502_ORA_IMM, 0x01);
        cb_emit8_8(M6502_STA_ZP, 0x01);
    }
    cb_emit8(M6502_RTS);

    if (sig->out_types.len == 1) {
        print_u16_addr = cb_cur_addr();
        cb_patch16(print_call1, print_u16_addr);

        /* div10: tmp_lo/tmp_hi /= 10, A = remainder */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_LDY_IMM, 16);
        div_loop_pc = cb_pc();
        cb_emit8_8(M6502_ASL_ZP, tmp_lo);
        cb_emit8_8(M6502_ROL_ZP, tmp_hi);
        cb_emit8(M6502_ROL_A);
        cb_emit8_8(M6502_CMP_IMM, 10);
        cb_emit8_8(M6502_BCC, 4);
        cb_emit8_8(M6502_SBC_IMM, 10);
        cb_emit8_8(M6502_INC_ZP, tmp_lo);
        cb_emit8(M6502_DEY);
        cb_emit8_8(M6502_BNE, (u8)(div_loop_pc - (cb_pc() + 2)));

        cb_emit8(M6502_PHA);
        cb_emit8_8(M6502_LDA_ZP, tmp_lo);
        cb_emit8_8(M6502_ORA_ZP, tmp_hi);
        cb_emit8_8(M6502_BEQ, 3);
        cb_emit8(M6502_JSR);
        print_call2 = cb_pc();
        cb_emit16(0x0000);
        cb_patch16(print_call2, print_u16_addr);

        cb_emit8(M6502_PLA);
        cb_emit8(M6502_CLC);
        cb_emit8_8(M6502_ADC_IMM, '0');
        cb_emit8(M6502_JSR);
        putchar_call3 = cb_pc();
        cb_emit16(0x0000);
        cb_emit8(M6502_RTS);

        cb_patch16(putchar_call1, C64_CHROUT);
        cb_patch16(putchar_call2, C64_CHROUT);
        cb_patch16(putchar_call3, C64_CHROUT);
    }
}

static void c64_end(int main_idx) { be_6502_cbm_end(main_idx, 0); }

const Platform plat_6502_c64 = {"--c64",   &be_6502, 0x0801,            "c64", 1,
                                c64_begin, c64_end,  be_6502_cbm_write, NULL};
