/* x16gs.c - Commander X16GS (W65C816, .prg self-loading into Bank $01) platform support */
#include "backend/65816.h"
#include "codebuf.h"
#include "frontend.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *x16gs_out_path = NULL;
static size_t x16gs_base = 0x011000UL;

void x16gs_set_out_path(const char *path) {
    x16gs_out_path = path;
}

static void x16gs_begin(size_t base) {
    x16gs_base = base ? base : 0x011000UL;
    be_65816_reset();
    /* Entry trampoline at base ($01:1000 or e.g. $F0:1000): JML entry_addr */
    cb_emit8(M816_JML);
    cb_emit24(0);
}

static void x16gs_emit_dp_data_addr(u8 dp, size_t data_off) {
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc(cb_pc(), data_off);
    cb_emit16(0);
    cb_emit8_8(M816_STA_DP, dp);
    cb_emit8_8(M816_SEP, 0x20);
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc_bank(cb_pc(), data_off);
    cb_emit8(0);
    cb_emit8_8(M816_STA_DP, (u8)(dp + 2));
    cb_emit8_8(M816_REP, 0x20);
}

static void x16gs_end(int main_idx) {
    const FuncSig *sig = ir4_func_sig((size_t)main_idx);
    size_t entry_addr;
    size_t putc_call1 = 0, putc_call2 = 0, print_call = 0;

    if ((cb_cur_addr() & 0xFFFFUL) > 0xFE80UL) {
        size_t pad = 0x10000UL - (cb_cur_addr() & 0xFFFFUL);
        while (pad-- > 0)
            cb_emit8(0);
    }
    entry_addr = cb_cur_addr();

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "-t x16gs requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    cb_patch24(1, (u32)entry_addr);

    /* Entered in native 16-bit mode (E=0, M=0, X=0, I=1) from Bank-0 .prg loader */
    cb_emit8(M816_CLD);

    /* Save caller's hardware stack S (from JSL $011000) to Golden RAM $00:0440 */
    cb_emit8(0x3B); /* TSC */
    cb_emit8(M816_STA_LONG);
    cb_emit24(0x000440UL);

    /* Hardware stack S = $1FFF (Bank $00), Direct Page D = $0400 (X16 Golden RAM $0400..$043F) */
    cb_emit8(M816_LDX_IMM);
    cb_emit16(0x1FFF);
    cb_emit8(M816_TXS);
    cb_emit8(M816_LDA_IMM);
    cb_emit16(0x0400);
    cb_emit8(M816_TCD);

    /* Data stack lives at <bank>:0200..<bank>:0FFF (DBR = base >> 16, below code at <bank>:1000) */
    cb_emit8_8(M816_SEP, 0x20);
    cb_emit8_8(M816_LDA_IMM, (u8)((x16gs_base >> 16) & 0xFFU));
    cb_emit8(M816_PHA);
    cb_emit8(M816_PLB);
    cb_emit8_8(M816_REP, 0x20);

    be_65816_emit_zero_bss();
    x16gs_emit_dp_data_addr(M816_DP_LFP, be_65816_rstack_off());

    /* Initialize data stack pointer X = $0E00 */
    cb_emit8(M816_LDX_IMM);
    cb_emit16(M816_STACK_INIT_X);

    /* JSL main */
    cb_emit8(M816_JSL);
    cb_emit24((u32)sig->addr);

    if (sig->out_types.len == 1) {
        size_t pos_br;
        static const u8 ktramp[10] = {
            0x38, 0xFB,             /* SEC; XCE (enter E=1 inside Bank $00) */
            0x58,                   /* CLI */
            0x20, 0xD2, 0xFF,       /* JSR $FFD2 (CHROUT) */
            0x78,                   /* SEI */
            0x18, 0xFB,             /* CLC; XCE (return to E=0 inside Bank $00) */
            0x6B                    /* RTL */
        };
        int ti;

        /* Pop 16-bit signed return value from $0200,X */
        cb_emit8(M816_LDA_ABSX);
        cb_emit16(M816_STACK_BASE);
        cb_emit8(M816_INX);
        cb_emit8(M816_INX);
        cb_emit8(M816_PHA);

        /* Write 10-byte Bank-0 CHROUT trampoline at $00:0450..$00:0459 */
        cb_emit8_8(M816_SEP, 0x20);
        for (ti = 0; ti < 10; ti++) {
            cb_emit8_8(M816_LDA_IMM, ktramp[ti]);
            cb_emit8(M816_STA_LONG);
            cb_emit24((u32)(0x000450UL + (u32)ti));
        }
        cb_emit8_8(M816_REP, 0x20);

        cb_emit8(M816_PLA);
        cb_emit8(M816_CMP_IMM);
        cb_emit16(0);
        pos_br = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        cb_emit8(M816_PHA);
        cb_emit8(M816_LDA_IMM);
        cb_emit16('-');
        cb_emit8(M816_JSL);
        putc_call1 = cb_pc();
        cb_emit24(0);
        cb_emit8(M816_PLA);
        cb_emit8(M816_EOR_IMM);
        cb_emit16(0xFFFF);
        cb_emit8(M816_INC_A);
        cb_patch8(pos_br + 1, (u8)(cb_pc() - (pos_br + 2)));

        cb_emit8(M816_JSL);
        print_call = cb_pc();
        cb_emit24(0);

        cb_emit8(M816_LDA_IMM);
        cb_emit16(13); /* CBM CR */
        cb_emit8(M816_JSL);
        putc_call2 = cb_pc();
        cb_emit24(0);
    }

    /* Restore caller S from $00:0440, DBR = $00, D = $0000, RTL in E=0 (loader switches to E=1) */
    cb_emit8(M816_LDA_LONG);
    cb_emit24(0x000440UL);
    cb_emit8(0x1B); /* TCS */
    cb_emit8_8(M816_SEP, 0x20);
    cb_emit8_8(M816_LDA_IMM, 0x00);
    cb_emit8(M816_PHA);
    cb_emit8(M816_PLB);
    cb_emit8_8(M816_REP, 0x20);
    cb_emit8(M816_LDA_IMM);
    cb_emit16(0x0000);
    cb_emit8(M816_TCD);
    cb_emit8(M816_RTL);

    if (sig->out_types.len == 1) {
        size_t putchar_addr = cb_cur_addr();
        size_t print_u16_addr, div_lp, bit_lp, pr_lp;

        /* putchar: emit low 8 bits of A via $00:0450 CHROUT (entered in E=0), preserving Y and S */
        cb_emit8_8(M816_STY_DP, 0x24);
        cb_emit8(M816_TAY);
        cb_emit8(0x3B); /* TSC */
        cb_emit8_8(M816_STA_DP, 0x26);
        cb_emit8(M816_LDA_IMM);
        cb_emit16(0x0000);
        cb_emit8(M816_TCD);
        cb_emit8_8(M816_SEP, 0x20);
        cb_emit8_8(M816_LDA_IMM, 0x00);
        cb_emit8(M816_PHA);
        cb_emit8(M816_PLB);
        cb_emit8_8(M816_REP, 0x20);
        cb_emit8(M816_LDX_IMM);
        cb_emit16(0x01DF);
        cb_emit8(M816_TXS);
        cb_emit8(M816_TYA);
        cb_emit8(M816_JSL);
        cb_emit24(0x000450UL);
        cb_emit8_8(M816_REP, 0x30);
        cb_emit8(M816_LDA_IMM);
        cb_emit16(0x0400);
        cb_emit8(M816_TCD);
        cb_emit8_8(M816_LDA_DP, 0x26);
        cb_emit8(0x1B); /* TCS */
        cb_emit8_8(M816_LDY_DP, 0x24);
        cb_emit8(M816_RTL);

        /* print_u16: unsigned 16-bit A -> decimal via putchar_addr */
        print_u16_addr = cb_cur_addr();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8(M816_LDY_IMM);
        cb_emit16(0);

        div_lp = cb_cur_addr();
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP2);
        cb_emit8(M816_LDA_IMM);
        cb_emit16(16);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);

        bit_lp = cb_cur_addr();
        cb_emit8_8(M816_ASL_DP, M816_DP_TMP0);
        cb_emit8_8(M816_ROL_DP, M816_DP_TMP2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        cb_emit8(M816_SEC);
        cb_emit8(M816_SBC_IMM);
        cb_emit16(10);
        cb_emit8_8(M816_BCC, 4);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_INC_DP, M816_DP_TMP0);
        cb_emit8_8(M816_DEC_DP, M816_DP_TMP1);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)bit_lp - (i32)(cb_cur_addr() + 2))));

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        cb_emit8(M816_CLC);
        cb_emit8(M816_ADC_IMM);
        cb_emit16('0');
        cb_emit8(M816_PHA);
        cb_emit8(M816_INY);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)div_lp - (i32)(cb_cur_addr() + 2))));

        pr_lp = cb_cur_addr();
        cb_emit8(M816_PLA);
        cb_emit8(M816_JSL);
        cb_emit24((u32)putchar_addr);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)pr_lp - (i32)(cb_cur_addr() + 2))));
        cb_emit8(M816_RTL);

        cb_patch24(putc_call1, (u32)putchar_addr);
        cb_patch24(putc_call2, (u32)putchar_addr);
        cb_patch24(print_call, (u32)print_u16_addr);
    }
}

static void x16gs_build_bin_paths(const char *prg_path, char *full_buf, size_t full_cap,
                                  char *base_buf, size_t base_cap) {
    const char *slash = NULL;
    const char *stem;
    const char *dot = NULL;
    const char *p;
    size_t dir_len = 0;
    size_t stem_len;
    size_t i;

    if (!prg_path || !*prg_path) {
        if (full_cap >= 10)
            strcpy(full_buf, "X16GS.BIN");
        if (base_cap >= 10)
            strcpy(base_buf, "X16GS.BIN");
        return;
    }
    for (p = prg_path; *p; p++) {
        if (*p == '/' || *p == '\\')
            slash = p;
    }
    stem = slash ? (slash + 1) : prg_path;
    dir_len = (size_t)(stem - prg_path);
    for (p = stem; *p; p++) {
        if (*p == '.')
            dot = p;
    }
    stem_len = dot ? (size_t)(dot - stem) : strlen(stem);
    if (stem_len == 0)
        stem_len = strlen(stem);
    if (stem_len + 4 >= base_cap)
        stem_len = base_cap - 5;

    for (i = 0; i < stem_len; i++)
        base_buf[i] = (char)toupper((unsigned char)stem[i]);
    memcpy(base_buf + stem_len, ".BIN", 5);

    if (dir_len + strlen(base_buf) + 1 > full_cap)
        ir4_die("x16gs sidecar path too long");
    memcpy(full_buf, prg_path, dir_len);
    strcpy(full_buf + dir_len, base_buf);
}

static int x16gs_write_streaming(FILE *fp, size_t base, u32 sz, u32 sz_even) {
    static const u8 basic_stub[12] = {
        0x0B, 0x08, 0x0A, 0x00, 0x9E, '2', '0', '6', '1', 0x00, 0x00, 0x00
    };
    /* 193-byte streaming loader at $080D: MACPTR ($FF44 -> $00:1000) + native MVN -> Bank $01+ */
    static const u8 loader_tmpl[193] = {
        0xA9, 0x02, 0xA2, 0x08, 0xA0, 0x02, 0x20, 0xBA, 0xFF, 0xA9, 0x00, 0xA2,
        0xCE, 0xA0, 0x08, 0x20, 0xBD, 0xFF, 0x20, 0xC0, 0xFF, 0xB0, 0x83, 0xA2,
        0x02, 0x20, 0xC6, 0xFF, 0xB0, 0x7C, 0xA9, 0x00, 0x85, 0x02, 0xA9, 0x00,
        0x85, 0x03, 0xA9, 0x00, 0x85, 0x04, 0xA9, 0x00, 0x85, 0x05, 0xA9, 0x00,
        0x85, 0x06, 0xA9, 0x00, 0x85, 0x07, 0xA5, 0x06, 0x05, 0x07, 0xD0, 0x06,
        0xA5, 0x05, 0xF0, 0x63, 0x80, 0x02, 0xA9, 0x00, 0xA2, 0x00, 0xA0, 0x10,
        0x18, 0x20, 0x44, 0xFF, 0xB0, 0x4C, 0x86, 0x08, 0x84, 0x09, 0x8A, 0x05,
        0x09, 0xF0, 0x4C, 0x38, 0xA5, 0x05, 0xE5, 0x08, 0x85, 0x05, 0xA5, 0x06,
        0xE5, 0x09, 0x85, 0x06, 0xA5, 0x07, 0xE9, 0x00, 0x85, 0x07, 0xA5, 0x04,
        0x8D, 0x8A, 0x08, 0x78, 0x18, 0xFB, 0xC2, 0x30, 0xA5, 0x08, 0x3A, 0xA2,
        0x00, 0x10, 0xA4, 0x02, 0x54, 0x01, 0x00, 0xE2, 0x20, 0xA9, 0x00, 0x48,
        0xAB, 0xC2, 0x20, 0x18, 0xA5, 0x02, 0x65, 0x08, 0x85, 0x02, 0x90, 0x04,
        0xE2, 0x20, 0xE6, 0x04, 0x38, 0xFB, 0x58, 0x4C, 0x43, 0x08, 0x20, 0xCC,
        0xFF, 0xA9, 0x02, 0x20, 0xC3, 0xFF, 0x60, 0x20, 0xCC, 0xFF, 0xA9, 0x02,
        0x20, 0xC3, 0xFF, 0x78, 0x18, 0xFB, 0xC2, 0x30, 0x22, 0x00, 0x00, 0x00,
        0x38, 0xFB, 0x58, 0xA9, 0x8F, 0x20, 0xD2, 0xFF, 0xA9, 0x8E, 0x4C, 0xD2,
        0xFF
    };
    char bin_path[512];
    char bin_name[64];
    FILE *bin_fp;
    u8 loader[193];
    u8 fname_len;

    x16gs_build_bin_paths(x16gs_out_path, bin_path, sizeof(bin_path), bin_name, sizeof(bin_name));
    fname_len = (u8)strlen(bin_name);

    bin_fp = fopen(bin_path, "wb");
    if (!bin_fp)
        return 0;
    if (!cb_write_raw(bin_fp, 1)) {
        fclose(bin_fp);
        return 0;
    }
    if (sz & 1U)
        fputc(0x00, bin_fp);
    if (ferror(bin_fp)) {
        fclose(bin_fp);
        return 0;
    }
    fclose(bin_fp);

    memcpy(loader, loader_tmpl, sizeof(loader_tmpl));
    loader[10] = fname_len;
    loader[31] = (u8)(base & 0xFFU);
    loader[35] = (u8)((base >> 8) & 0xFFU);
    loader[39] = (u8)((base >> 16) & 0xFFU);
    loader[43] = (u8)(sz_even & 0xFFU);
    loader[47] = (u8)((sz_even >> 8) & 0xFFU);
    loader[51] = (u8)((sz_even >> 16) & 0xFFU);
    loader[177] = (u8)(base & 0xFFU);
    loader[178] = (u8)((base >> 8) & 0xFFU);
    loader[179] = (u8)((base >> 16) & 0xFFU);

    fputc(0x01, fp);
    fputc(0x08, fp);
    fwrite(basic_stub, 1, sizeof(basic_stub), fp);
    fwrite(loader, 1, sizeof(loader), fp);
    fwrite(bin_name, 1, fname_len, fp);
    return ferror(fp) ? 0 : 1;
}

static int x16gs_write(FILE *fp, size_t base) {
    /* 12-byte BASIC stub at $0801: 10 SYS 2061 ($080D) */
    static const u8 basic_stub[12] = {
        0x0B, 0x08, 0x0A, 0x00, 0x9E, '2', '0', '6', '1', 0x00, 0x00, 0x00
    };
    /* 40-byte 65C816 long-indexed word-copy loader at $080D
     * (uses 24-bit LDA $00xxxx,X because ROM -gs far-RAM probe leaves DBR != $00):
     *   SEI; CLC; XCE; REP #$30; LDX #0;
     *   lp: LDA $000835,X; STA $011000,X; INX; INX; CPX #sz_even; BCC lp;
     *   JSL $011000; SEC; XCE; CLI; LDA #$8F; JSR $FFD2; LDA #$8E; JMP $FFD2 */
    u32 sz = (u32)(cb_pc() + cb_init_data_size());
    u32 sz_even = (sz + 1U) & ~1U;
    u16 src = 0x080D + 40; /* $0835 */
    u8 loader[40];

    if (sz_even > (u32)(0x9F00 - src))
        return x16gs_write_streaming(fp, base, sz, sz_even);

    loader[0] = 0x78; /* SEI */
    loader[1] = 0x18; /* CLC */
    loader[2] = 0xFB; /* XCE */
    loader[3] = 0xC2; /* REP #$30 */
    loader[4] = 0x30;
    loader[5] = 0xA2; /* LDX #$0000 */
    loader[6] = 0x00;
    loader[7] = 0x00;
    loader[8] = 0xBF; /* LDA $00:src,X */
    loader[9] = (u8)(src & 0xFFU);
    loader[10] = (u8)((src >> 8) & 0xFFU);
    loader[11] = 0x00;
    loader[12] = 0x9F; /* STA base,X */
    loader[13] = (u8)(base & 0xFFU);
    loader[14] = (u8)((base >> 8) & 0xFFU);
    loader[15] = (u8)((base >> 16) & 0xFFU);
    loader[16] = 0xE8; /* INX */
    loader[17] = 0xE8; /* INX */
    loader[18] = 0xE0; /* CPX #sz_even */
    loader[19] = (u8)(sz_even & 0xFFU);
    loader[20] = (u8)((sz_even >> 8) & 0xFFU);
    loader[21] = 0x90; /* BCC -15 (offset 8) */
    loader[22] = 0xF1;
    loader[23] = 0x22; /* JSL base */
    loader[24] = (u8)(base & 0xFFU);
    loader[25] = (u8)((base >> 8) & 0xFFU);
    loader[26] = (u8)((base >> 16) & 0xFFU);
    loader[27] = 0x38; /* SEC */
    loader[28] = 0xFB; /* XCE (back to E=1 inside Bank $00) */
    loader[29] = 0x58; /* CLI */
    loader[30] = 0xA9; /* LDA #$8F (exit ISO mode) */
    loader[31] = 0x8F;
    loader[32] = 0x20; /* JSR $FFD2 */
    loader[33] = 0xD2;
    loader[34] = 0xFF;
    loader[35] = 0xA9; /* LDA #$8E (PETSCII upper/gfx) */
    loader[36] = 0x8E;
    loader[37] = 0x4C; /* JMP $FFD2 (returns via RTS) */
    loader[38] = 0xD2;
    loader[39] = 0xFF;

    /* Write PRG 2-byte load address $0801 */
    fputc(0x01, fp);
    fputc(0x08, fp);
    fwrite(basic_stub, 1, sizeof(basic_stub), fp);
    fwrite(loader, 1, sizeof(loader), fp);

    if (!cb_write_raw(fp, 1))
        return 0;
    if (sz & 1U)
        fputc(0x00, fp);
    return ferror(fp) ? 0 : 1;
}

const Platform plat_65816_x16gs = {"--x16gs", &be_65816, 0x011000UL, "x16gs", 1,
                                   x16gs_begin, x16gs_end, x16gs_write, NULL, "prg"};
