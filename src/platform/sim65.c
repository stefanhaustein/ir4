/* sim65.c - sim65 (cc65 6502 simulator) header & exit/print stub */
#include "codebuf.h"
#include "frontend.h"
#include "backend/6502.h"

#define SIM65_WRITE 0xFFF7
#define SIM65_EXIT 0xFFF9
#define SIM65_SP_ZP 0x10

static void sim_begin(size_t base) {
    (void)base;
    be_6502_set_ram_top(0xF000);
    /* Reserve JMP 0x0000 at base; patched by sim_end() */
    cb_emit8_16(M6502_JMP, 0x0000);
}

static int sim_write(FILE *fp, size_t base) {
    u8 hdr[12];
    hdr[0] = 's';
    hdr[1] = 'i';
    hdr[2] = 'm';
    hdr[3] = '6';
    hdr[4] = '5';
    hdr[5] = 0x02; /* version 2 */
    hdr[6] = 0x00; /* 6502 */
    hdr[7] = SIM65_SP_ZP;
    cb_put_le16(hdr + 8, base);
    cb_put_le16(hdr + 10, base);
    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
        return 0;
    return cb_write_raw(fp, 1);
}

static void sim_end(int main_idx) {
    const FnSig *sig = ir4_fn_sig(main_idx);
    u16 entry_addr = cb_cur_addr();
    u16 putchar_call1 = 0;
    u16 putchar_call2 = 0;
    u16 putchar_call3 = 0;
    u16 print_call1 = 0;
    u16 print_call2 = 0;
    u16 print_u16_addr;
    u16 putchar_addr;
    u16 div_loop_pc;

    if (sig->in_cnt != 0 || sig->out_cnt > 1) {
        fprintf(stderr, "--sim65 requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    /* Patch the initial JMP 0x0000 at offset 0 (base + 1) to jump here */
    cb_patch_globally16(1, entry_addr);

    /* CLD; LDX #$FF; TXS; zero BSS; LDX #init_x */
    cb_emit8(M6502_CLD);
    if (sig->out_cnt == 1) {
        (void)be_6502_stack_data_off();
    }
    cb_emit8_8(M6502_LDX_IMM, 0xFF);
    cb_emit8(M6502_TXS);
    be_6502_emit_zero_bss(0x04, 0x05);
    cb_emit8_8(M6502_LDX_IMM, be_6502_stack_zp_init_x());

    /* JSR main */
    cb_emit8_16(M6502_JSR, sig->addr);

    if (sig->out_cnt == 1) {
        u8 zp_stk = be_6502_stack_zp_base();
        /* Pop 16-bit signed value from (stack_base,X) into $04/$05 */
        if (zp_stk != 0) {
            cb_emit8_8(0xB5, zp_stk); /* LDA zp,X */
            cb_emit8_8(M6502_STA_ZP, 0x04);
            cb_emit8_8(0xB5, (u8)(zp_stk + 1)); /* LDA zp+1,X */
            cb_emit8_8(M6502_STA_ZP, 0x05);
        } else {
            u16 stk_off = be_6502_stack_data_off();
            cb_emit8(M6502_LDA_ABSX);
            cb_add_data_reloc(cb_pc(), stk_off);
            cb_emit16(0x0000);
            cb_emit8_8(M6502_STA_ZP, 0x04);
            cb_emit8(M6502_LDA_ABSX);
            cb_add_data_reloc(cb_pc(), (u16)(stk_off + 1));
            cb_emit16(0x0000);
            cb_emit8_8(M6502_STA_ZP, 0x05);
        }
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);

        /* If $05 < 0, print '-' and negate $04/$05 */
        cb_emit8_8(M6502_LDA_ZP, 0x05);
        cb_emit8_8(M6502_BPL, 18);

        cb_emit8_8(M6502_LDA_IMM, '-');
        cb_emit8(M6502_JSR);
        putchar_call1 = cb_pc();
        cb_emit16(0x0000);
        cb_emit8(M6502_SEC);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_SBC_ZP, 0x04);
        cb_emit8_8(M6502_STA_ZP, 0x04);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_SBC_ZP, 0x05);
        cb_emit8_8(M6502_STA_ZP, 0x05);

        /* JSR print_u16 */
        cb_emit8(M6502_JSR);
        print_call1 = cb_pc();
        cb_emit16(0x0000);

        /* Print '\n' */
        cb_emit8_8(M6502_LDA_IMM, '\n');
        cb_emit8(M6502_JSR);
        putchar_call2 = cb_pc();
        cb_emit16(0x0000);
    }

    /* Exit with code 0 via sim65 paravirt exit ($FFF9) */
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_16(M6502_JMP, SIM65_EXIT);

    if (sig->out_cnt == 1) {
        print_u16_addr = cb_cur_addr();
        cb_patch_globally16(print_call1, print_u16_addr);

        /* div10: $04/$05 /= 10, A = remainder */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_LDY_IMM, 16);
        div_loop_pc = cb_pc();
        cb_emit8_8(M6502_ASL_ZP, 0x04);
        cb_emit8_8(M6502_ROL_ZP, 0x05);
        cb_emit8(M6502_ROL_A);
        cb_emit8_8(M6502_CMP_IMM, 10);
        cb_emit8_8(M6502_BCC, 4);
        cb_emit8_8(M6502_SBC_IMM, 10);
        cb_emit8_8(M6502_INC_ZP, 0x04);
        cb_emit8(M6502_DEY);
        cb_emit8_8(M6502_BNE, (u8)(div_loop_pc - (cb_pc() + 2)));

        cb_emit8(M6502_PHA);
        cb_emit8_8(M6502_LDA_ZP, 0x04);
        cb_emit8_8(M6502_ORA_ZP, 0x05);
        cb_emit8_8(M6502_BEQ, 3);
        cb_emit8(M6502_JSR);
        print_call2 = cb_pc();
        cb_emit16(0x0000);
        cb_patch_globally16(print_call2, print_u16_addr);

        cb_emit8(M6502_PLA);
        cb_emit8(M6502_CLC);
        cb_emit8_8(M6502_ADC_IMM, '0');
        cb_emit8(M6502_JSR);
        putchar_call3 = cb_pc();
        cb_emit16(0x0000);
        cb_emit8(M6502_RTS);

        putchar_addr = cb_cur_addr();
        cb_patch_globally16(putchar_call1, putchar_addr);
        cb_patch_globally16(putchar_call2, putchar_addr);
        cb_patch_globally16(putchar_call3, putchar_addr);

        /* putchar_a: write 1 byte in A to stdout (fd=1) via sim65 _write ($FFF7) */
        cb_emit8_8(M6502_STA_ZP, 0x16); /* char buf */
        cb_emit8_8(M6502_STX_ZP, 0x17); /* save X */
        cb_emit8_8(M6502_LDA_IMM, 0x16);
        cb_emit8_8(M6502_STA_ZP, 0x12); /* buf = $0016 */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_STA_ZP, 0x13);
        cb_emit8_8(M6502_LDA_IMM, 0x01);
        cb_emit8_8(M6502_STA_ZP, 0x14); /* fd = 1 */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_STA_ZP, 0x15);
        cb_emit8_8(M6502_LDA_IMM, 0x12);
        cb_emit8_8(M6502_STA_ZP, SIM65_SP_ZP);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_STA_ZP, SIM65_SP_ZP + 1);
        cb_emit8_8(M6502_LDA_IMM, 0x01); /* count lo = 1 */
        cb_emit8_8(M6502_LDX_IMM, 0x00); /* count hi = 0 */
        cb_emit8_16(M6502_JSR, SIM65_WRITE);
        cb_emit8_8(M6502_LDX_ZP, 0x17);
        cb_emit8(M6502_RTS);
    }
}

const Platform plat_6502_sim = {"--sim65", &be_6502, 0x0200,    "sim65", 1,
                                sim_begin, sim_end,  sim_write, NULL, "bin"};
