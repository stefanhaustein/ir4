/* x16.c - Commander X16 (.prg & banked .crt cartridge) platform support */
#include "codebuf.h"
#include "frontend.h"
#include "backend/6502.h"

#define X16_CHROUT 0xFFD2

static void x16_begin(size_t base) { be_6502_cbm_begin(base, 1); }
static void x16_end(int main_idx) { be_6502_cbm_end(main_idx, 1); }

const Platform plat_6502_x16 = {"--x16",   &be_6502, 0x0801,            "x16", 1,
                                x16_begin, x16_end,  be_6502_cbm_write, NULL, "prg"};

static u16 cart_bank_call_addr = 0;
static u16 cart_kernal_call_addr = 0;
static int cart_main_idx = -1;

static void x16_cart_begin(size_t base) {
    u16 bc_patch_lo, bc_patch_hi, bc_jsr_addr;
    u16 kc_patch_lo, kc_patch_hi, kc_jsr_addr;

    cb_set_bank_config(0xC000, 16384);
    be_6502_set_ram_top(0x9F00);
    be_6502_set_default_zp(0x7C);
    be_6502_set_default_stack_zp(0x22, 0x5A);
    cart_main_idx = -1;

    /* Low-RAM __bank_call trampoline: JSR __bank_call; .byte phys_bank; .word addr */
    cart_bank_call_addr = (u16)cb_cur_addr();
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, 0x02);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, 0x03);
    cb_emit8(M6502_CLC);
    cb_emit8_8(M6502_LDA_ZP, 0x02);
    cb_emit8_8(M6502_ADC_IMM, 0x03);
    cb_emit8(M6502_TAY);
    cb_emit8_8(M6502_LDA_ZP, 0x03);
    cb_emit8_8(M6502_ADC_IMM, 0x00);
    cb_emit8(M6502_PHA);
    cb_emit8(M6502_TYA);
    cb_emit8(M6502_PHA);
    cb_emit8_8(M6502_LDA_ZP, 0x01);
    cb_emit8(M6502_PHA);
    cb_emit8_8(M6502_LDY_IMM, 0x03);
    cb_emit8_8(M6502_LDA_INDY, 0x02);
    cb_emit8(M6502_STA_ABS);
    bc_patch_hi = (u16)cb_pc();
    cb_emit16(0x0000);
    cb_emit8(M6502_DEY);
    cb_emit8_8(M6502_LDA_INDY, 0x02);
    cb_emit8(M6502_STA_ABS);
    bc_patch_lo = (u16)cb_pc();
    cb_emit16(0x0000);
    cb_emit8(M6502_DEY);
    cb_emit8_8(M6502_LDA_INDY, 0x02);
    cb_emit8_8(M6502_STA_ZP, 0x01);
    bc_jsr_addr = (u16)cb_cur_addr();
    cb_emit8_16(M6502_JSR, 0x0000);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, 0x01);
    cb_emit8(M6502_RTS);
    cb_patch16(bc_patch_lo, (u16)(bc_jsr_addr + 1));
    cb_patch16(bc_patch_hi, (u16)(bc_jsr_addr + 2));

    /* Low-RAM __kernal_call trampoline: JSR __kernal_call; .word ext_addr */
    cart_kernal_call_addr = (u16)cb_cur_addr();
    cb_emit8(M6502_PHA);
    cb_emit8(M6502_TYA);
    cb_emit8(M6502_PHA);
    cb_emit8(0xBA); /* TSX */
    cb_emit8_16(M6502_LDA_ABSX, 0x0103);
    cb_emit8_8(M6502_STA_ZP, 0x02);
    cb_emit8_16(M6502_LDA_ABSX, 0x0104);
    cb_emit8_8(M6502_STA_ZP, 0x03);
    cb_emit8(M6502_CLC);
    cb_emit8_8(M6502_LDA_ZP, 0x02);
    cb_emit8_8(M6502_ADC_IMM, 0x02);
    cb_emit8_16(M6502_STA_ABSX, 0x0103);
    cb_emit8_8(M6502_LDA_ZP, 0x03);
    cb_emit8_8(M6502_ADC_IMM, 0x00);
    cb_emit8_16(M6502_STA_ABSX, 0x0104);
    cb_emit8_8(M6502_LDY_IMM, 0x01);
    cb_emit8_8(M6502_LDA_INDY, 0x02);
    cb_emit8(M6502_STA_ABS);
    kc_patch_lo = (u16)cb_pc();
    cb_emit16(0x0000);
    cb_emit8(M6502_INY);
    cb_emit8_8(M6502_LDA_INDY, 0x02);
    cb_emit8(M6502_STA_ABS);
    kc_patch_hi = (u16)cb_pc();
    cb_emit16(0x0000);
    cb_emit8(M6502_PLA);
    cb_emit8(M6502_TAY);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, 0x02);
    cb_emit8_8(M6502_LDA_ZP, 0x01);
    cb_emit8(M6502_PHA);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_8(M6502_STA_ZP, 0x01);
    cb_emit8_8(M6502_LDA_ZP, 0x02);
    kc_jsr_addr = (u16)cb_cur_addr();
    cb_emit8_16(M6502_JSR, 0x0000);
    cb_emit8(M6502_PHP);
    cb_emit8_8(M6502_STA_ZP, 0x02);
    cb_emit8(0xBA); /* TSX */
    cb_emit8_16(M6502_LDA_ABSX, 0x0102);
    cb_emit8_8(M6502_STA_ZP, 0x01);
    cb_emit8_8(M6502_LDA_ZP, 0x02);
    cb_emit8(M6502_PLP);
    cb_emit8_8(M6502_STA_ZP, 0x02);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_LDA_ZP, 0x02);
    cb_emit8(M6502_RTS);
    cb_patch16(kc_patch_lo, (u16)(kc_jsr_addr + 1));
    cb_patch16(kc_patch_hi, (u16)(kc_jsr_addr + 2));

    (void)base;
    be_6502_set_trampoline(cart_bank_call_addr, cart_kernal_call_addr);
}

static void x16_cart_end(int main_idx) {
    const FnSig *sig = ir4_fn_sig(main_idx);
    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--x16-cart requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }
    cart_main_idx = main_idx;
}

static int x16_cart_write(FILE *fp, size_t base) {
    u8 hdr[480];
    u8 bank_buf[16384];
    size_t pos = 0;
    BankDesc b0 = cb_get_bank(0);
    size_t data_addr = b0.base + b0.len;
    size_t init_sz = cb_init_data_size();
    size_t bss_sz = cb_data_size() - init_sz;
    size_t ram_payload_sz = b0.len + init_sz;
    size_t bss_addr = data_addr + init_sz;
    size_t num_banks = cb_bank_count();
    size_t num_rom_banks = 1 + (num_banks > 1 ? num_banks - 1 : 0);
    const FnSig *sig = ir4_fn_sig(cart_main_idx);
    size_t i;
    u16 p_print1 = 0, p_print2 = 0, print_addr, loop_pc;

    (void)base;
    if (data_addr + cb_data_size() > 0x9F00) {
        ir4_die("X16 cartridge Low RAM overflow (code + data exceeds $9EFF)");
    }
    if (ram_payload_sz > 16384 - 256) {
        ir4_die("X16 cartridge Low RAM payload exceeds bank 32 capacity (16128 bytes)");
    }
    if (num_rom_banks > 224) {
        ir4_die("X16 cartridge exceeds 224 ROM banks");
    }

    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "CX16 CARTRIDGE\r\n01.00           ", 32);
    for (i = 0; i < num_rom_banks; i++) {
        hdr[256 + i] = 1;
    }
    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr)) {
        return 0;
    }

    memset(bank_buf, 0, sizeof(bank_buf));
    bank_buf[pos++] = 'C';
    bank_buf[pos++] = 'X';
    bank_buf[pos++] = '1';
    bank_buf[pos++] = '6';

    /* Boot at $C004: CLD; CLI */
    bank_buf[pos++] = M6502_CLD;
    bank_buf[pos++] = 0x58; /* CLI */

    /* Copy ram_payload_sz bytes from $C100 to b0.base */
    bank_buf[pos++] = M6502_LDA_IMM;
    bank_buf[pos++] = 0x00;
    bank_buf[pos++] = M6502_STA_ZP;
    bank_buf[pos++] = 0x02;
    bank_buf[pos++] = M6502_LDA_IMM;
    bank_buf[pos++] = 0xC1;
    bank_buf[pos++] = M6502_STA_ZP;
    bank_buf[pos++] = 0x03;
    bank_buf[pos++] = M6502_LDA_IMM;
    bank_buf[pos++] = (u8)(b0.base & 0xFF);
    bank_buf[pos++] = M6502_STA_ZP;
    bank_buf[pos++] = 0x04;
    bank_buf[pos++] = M6502_LDA_IMM;
    bank_buf[pos++] = (u8)((b0.base >> 8) & 0xFF);
    bank_buf[pos++] = M6502_STA_ZP;
    bank_buf[pos++] = 0x05;

    if ((ram_payload_sz >> 8) > 0) {
        bank_buf[pos++] = M6502_LDX_IMM;
        bank_buf[pos++] = (u8)(ram_payload_sz >> 8);
        bank_buf[pos++] = M6502_LDY_IMM;
        bank_buf[pos++] = 0x00;
        loop_pc = (u16)pos;
        bank_buf[pos++] = M6502_LDA_INDY;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_STA_INDY;
        bank_buf[pos++] = 0x04;
        bank_buf[pos++] = M6502_INY;
        bank_buf[pos++] = M6502_BNE;
        {
            u8 rel = (u8)(loop_pc - (pos + 1));
            bank_buf[pos++] = rel;
        }
        bank_buf[pos++] = M6502_INC_ZP;
        bank_buf[pos++] = 0x03;
        bank_buf[pos++] = M6502_INC_ZP;
        bank_buf[pos++] = 0x05;
        bank_buf[pos++] = 0xCA; /* DEX */
        bank_buf[pos++] = M6502_BNE;
        {
            u8 rel = (u8)(loop_pc - (pos + 1));
            bank_buf[pos++] = rel;
        }
    }
    if ((ram_payload_sz & 0xFF) > 0) {
        bank_buf[pos++] = M6502_LDY_IMM;
        bank_buf[pos++] = 0x00;
        loop_pc = (u16)pos;
        bank_buf[pos++] = M6502_LDA_INDY;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_STA_INDY;
        bank_buf[pos++] = 0x04;
        bank_buf[pos++] = M6502_INY;
        bank_buf[pos++] = 0xC0; /* CPY #imm */
        bank_buf[pos++] = (u8)(ram_payload_sz & 0xFF);
        bank_buf[pos++] = M6502_BNE;
        {
            u8 rel = (u8)(loop_pc - (pos + 1));
            bank_buf[pos++] = rel;
        }
    }

    /* Zero bss_sz bytes at bss_addr */
    if (bss_sz > 0) {
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = (u8)(bss_addr & 0xFF);
        bank_buf[pos++] = M6502_STA_ZP;
        bank_buf[pos++] = 0x04;
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = (u8)((bss_addr >> 8) & 0xFF);
        bank_buf[pos++] = M6502_STA_ZP;
        bank_buf[pos++] = 0x05;
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = 0x00;
        if ((bss_sz >> 8) > 0) {
            bank_buf[pos++] = M6502_LDX_IMM;
            bank_buf[pos++] = (u8)(bss_sz >> 8);
            bank_buf[pos++] = M6502_LDY_IMM;
            bank_buf[pos++] = 0x00;
            loop_pc = (u16)pos;
            bank_buf[pos++] = M6502_STA_INDY;
            bank_buf[pos++] = 0x04;
            bank_buf[pos++] = M6502_INY;
            bank_buf[pos++] = M6502_BNE;
            {
                u8 rel = (u8)(loop_pc - (pos + 1));
                bank_buf[pos++] = rel;
            }
            bank_buf[pos++] = M6502_INC_ZP;
            bank_buf[pos++] = 0x05;
            bank_buf[pos++] = 0xCA; /* DEX */
            bank_buf[pos++] = M6502_BNE;
            {
                u8 rel = (u8)(loop_pc - (pos + 1));
                bank_buf[pos++] = rel;
            }
        }
        if ((bss_sz & 0xFF) > 0) {
            bank_buf[pos++] = M6502_LDY_IMM;
            bank_buf[pos++] = 0x00;
            loop_pc = (u16)pos;
            bank_buf[pos++] = M6502_STA_INDY;
            bank_buf[pos++] = 0x04;
            bank_buf[pos++] = M6502_INY;
            bank_buf[pos++] = 0xC0; /* CPY #imm */
            bank_buf[pos++] = (u8)(bss_sz & 0xFF);
            bank_buf[pos++] = M6502_BNE;
            {
                u8 rel = (u8)(loop_pc - (pos + 1));
                bank_buf[pos++] = rel;
            }
        }
    }

    /* LDX #init_x; JSR main */
    bank_buf[pos++] = M6502_LDX_IMM;
    bank_buf[pos++] = be_6502_stack_zp_init_x();
    bank_buf[pos++] = M6502_JSR;
    bank_buf[pos++] = (u8)(sig->addr & 0xFF);
    bank_buf[pos++] = (u8)((sig->addr >> 8) & 0xFF);

    if (sig->out_types.len == 1) {
        u8 zp_stk = be_6502_stack_zp_base();
        if (zp_stk != 0) {
            bank_buf[pos++] = 0xB5;
            bank_buf[pos++] = zp_stk;
            bank_buf[pos++] = M6502_STA_ZP;
            bank_buf[pos++] = 0x02;
            bank_buf[pos++] = 0xB5;
            bank_buf[pos++] = (u8)(zp_stk + 1);
            bank_buf[pos++] = M6502_STA_ZP;
            bank_buf[pos++] = 0x03;
        } else {
            u16 stk_abs = (u16)(data_addr + be_6502_stack_data_off());
            bank_buf[pos++] = M6502_LDA_ABSX;
            bank_buf[pos++] = (u8)(stk_abs & 0xFF);
            bank_buf[pos++] = (u8)((stk_abs >> 8) & 0xFF);
            bank_buf[pos++] = M6502_STA_ZP;
            bank_buf[pos++] = 0x02;
            bank_buf[pos++] = M6502_LDA_ABSX;
            bank_buf[pos++] = (u8)((stk_abs + 1) & 0xFF);
            bank_buf[pos++] = (u8)(((stk_abs + 1) >> 8) & 0xFF);
            bank_buf[pos++] = M6502_STA_ZP;
            bank_buf[pos++] = 0x03;
        }
        bank_buf[pos++] = M6502_INX;
        bank_buf[pos++] = M6502_INX;
        bank_buf[pos++] = M6502_LDA_ZP;
        bank_buf[pos++] = 0x03;
        bank_buf[pos++] = M6502_BPL;
        bank_buf[pos++] = 20;
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = '-';
        bank_buf[pos++] = M6502_JSR;
        bank_buf[pos++] = (u8)(cart_kernal_call_addr & 0xFF);
        bank_buf[pos++] = (u8)((cart_kernal_call_addr >> 8) & 0xFF);
        bank_buf[pos++] = (u8)(X16_CHROUT & 0xFF);
        bank_buf[pos++] = (u8)((X16_CHROUT >> 8) & 0xFF);
        bank_buf[pos++] = M6502_SEC;
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = 0x00;
        bank_buf[pos++] = M6502_SBC_ZP;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_STA_ZP;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = 0x00;
        bank_buf[pos++] = M6502_SBC_ZP;
        bank_buf[pos++] = 0x03;
        bank_buf[pos++] = M6502_STA_ZP;
        bank_buf[pos++] = 0x03;
        bank_buf[pos++] = M6502_JSR;
        p_print1 = (u16)pos;
        bank_buf[pos++] = 0x00;
        bank_buf[pos++] = 0x00;
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = 13;
        bank_buf[pos++] = M6502_JSR;
        bank_buf[pos++] = (u8)(cart_kernal_call_addr & 0xFF);
        bank_buf[pos++] = (u8)((cart_kernal_call_addr >> 8) & 0xFF);
        bank_buf[pos++] = (u8)(X16_CHROUT & 0xFF);
        bank_buf[pos++] = (u8)((X16_CHROUT >> 8) & 0xFF);
    }

    /* Switch back to KERNAL ROM ($01=0) and enter BASIC cold start ($FF47 with C=0) */
    bank_buf[pos++] = M6502_LDA_IMM;
    bank_buf[pos++] = 0x00;
    bank_buf[pos++] = M6502_STA_ZP;
    bank_buf[pos++] = 0x01;
    bank_buf[pos++] = M6502_CLC;
    bank_buf[pos++] = M6502_JMP;
    bank_buf[pos++] = 0x47;
    bank_buf[pos++] = 0xFF;

    if (sig->out_types.len == 1) {
        print_addr = (u16)(0xC000 + pos);
        cb_put_le16(&bank_buf[p_print1], print_addr);
        bank_buf[pos++] = M6502_LDA_IMM;
        bank_buf[pos++] = 0x00;
        bank_buf[pos++] = M6502_LDY_IMM;
        bank_buf[pos++] = 16;
        loop_pc = (u16)pos;
        bank_buf[pos++] = M6502_ASL_ZP;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_ROL_ZP;
        bank_buf[pos++] = 0x03;
        bank_buf[pos++] = M6502_ROL_A;
        bank_buf[pos++] = M6502_CMP_IMM;
        bank_buf[pos++] = 10;
        bank_buf[pos++] = M6502_BCC;
        bank_buf[pos++] = 4;
        bank_buf[pos++] = M6502_SBC_IMM;
        bank_buf[pos++] = 10;
        bank_buf[pos++] = M6502_INC_ZP;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_DEY;
        bank_buf[pos++] = M6502_BNE;
        {
            u8 rel = (u8)(loop_pc - (pos + 1));
            bank_buf[pos++] = rel;
        }
        bank_buf[pos++] = M6502_PHA;
        bank_buf[pos++] = M6502_LDA_ZP;
        bank_buf[pos++] = 0x02;
        bank_buf[pos++] = M6502_ORA_ZP;
        bank_buf[pos++] = 0x03;
        bank_buf[pos++] = M6502_BEQ;
        bank_buf[pos++] = 3;
        bank_buf[pos++] = M6502_JSR;
        p_print2 = (u16)pos;
        bank_buf[pos++] = 0x00;
        bank_buf[pos++] = 0x00;
        cb_put_le16(&bank_buf[p_print2], print_addr);
        bank_buf[pos++] = M6502_PLA;
        bank_buf[pos++] = M6502_CLC;
        bank_buf[pos++] = M6502_ADC_IMM;
        bank_buf[pos++] = '0';
        bank_buf[pos++] = M6502_JSR;
        bank_buf[pos++] = (u8)(cart_kernal_call_addr & 0xFF);
        bank_buf[pos++] = (u8)((cart_kernal_call_addr >> 8) & 0xFF);
        bank_buf[pos++] = (u8)(X16_CHROUT & 0xFF);
        bank_buf[pos++] = (u8)((X16_CHROUT >> 8) & 0xFF);
        bank_buf[pos++] = M6502_RTS;
    }

    if (pos > 0x0100) {
        ir4_die("X16 cartridge boot stub exceeded 256 bytes");
    }
    memcpy(bank_buf + 0x0100, cb_data() + b0.start_pc, b0.len);
    if (init_sz > 0) {
        memcpy(bank_buf + 0x0100 + b0.len, cb_init_data(), init_sz);
    }
    if (fwrite(bank_buf, 1, sizeof(bank_buf), fp) != sizeof(bank_buf)) {
        return 0;
    }

    for (i = 1; i < num_banks; i++) {
        BankDesc bd = cb_get_bank((u16)i);
        if (bd.len > sizeof(bank_buf)) {
            char msg[128];
            sprintf(msg, "X16 cartridge ROM bank %lu exceeded 16384 bytes (%lu bytes)",
                    (unsigned long)i, (unsigned long)bd.len);
            ir4_die(msg);
        }
        memset(bank_buf, 0, sizeof(bank_buf));
        if (bd.len > 0) {
            memcpy(bank_buf, cb_data() + bd.start_pc, bd.len);
        }
        if (fwrite(bank_buf, 1, sizeof(bank_buf), fp) != sizeof(bank_buf)) {
            return 0;
        }
    }
    return 1;
}

const Platform plat_6502_x16_cart = {"--x16-cart",   &be_6502,     0x0801,         "x16", 1,
                                     x16_cart_begin, x16_cart_end, x16_cart_write, NULL, "bin"};
