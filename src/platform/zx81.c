/* zx81.c - Sinclair ZX81 (.p snapshot) startup stub & writer for Z80 */
#include "codebuf.h"
#include "frontend.h"
#include "backend/z80.h"

#define ZX81_BASE 0x4091 /* 16529: $407D + 15-byte Line 1 + 5-byte Line 2 REM header */

static void zx81_begin(size_t base) {
    (void)base;
    cb_emit8_16(Z80_JP, 0x0000);
}

static void zx81_end(int main_idx) {
    const FuncSig *sig = ir4_func_sig((u8)main_idx);
    u16 entry_addr = cb_cur_addr();

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--zx81 requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    cb_patch16(1, entry_addr);
    be_z80_emit_zero_bss();
    cb_emit8_16(Z80_CALL, sig->addr);
    if (sig->out_types.len == 1) {
        cb_emit8(0xC1); /* POP BC (return value for RAND USR) */
    } else {
        cb_emit8(0x01); /* LD BC, 0 */
        cb_emit16(0x0000);
    }
    cb_emit8(Z80_RET);
}

static int zx81_write(FILE *fp, size_t base) {
    u8 sysvars[116];
    u8 line1[15];
    u8 line2_hdr[5];
    u8 dfile[793];
    u8 trailer[4];
    size_t code_len = cb_pc();
    size_t data_sz = cb_data_size();
    size_t init_sz = cb_init_data_size();
    size_t bss_sz = data_sz - init_sz;
    size_t rem_body_len = code_len + data_sz + 1;
    size_t dfile_addr = base + rem_body_len;
    size_t vars_addr = dfile_addr + sizeof(dfile);
    size_t eline_addr = vars_addr + 1;
    size_t stk_addr = eline_addr + 2;
    size_t r;

    if (stk_addr > 0xFF00) {
        ir4_die("ZX81 program exceeds 64K address space");
    }

    /* System variables $4009..$407C (116 bytes) */
    memset(sysvars, 0, sizeof(sysvars));
    sysvars[0x400A - 0x4009] = 0x01; /* E_PPC = 1 */
    cb_put_le16(&sysvars[0x400C - 0x4009], (u16)dfile_addr);
    cb_put_le16(&sysvars[0x400E - 0x4009], (u16)(dfile_addr + 1));
    cb_put_le16(&sysvars[0x4010 - 0x4009], (u16)vars_addr);
    cb_put_le16(&sysvars[0x4014 - 0x4009], (u16)eline_addr);
    cb_put_le16(&sysvars[0x4016 - 0x4009], (u16)(eline_addr + 1));
    cb_put_le16(&sysvars[0x401A - 0x4009], (u16)stk_addr);
    cb_put_le16(&sysvars[0x401C - 0x4009], (u16)stk_addr);
    cb_put_le16(&sysvars[0x401F - 0x4009], 0x405D); /* MEM = MEMBOT */
    sysvars[0x4022 - 0x4009] = 2;                 /* DF_SZ */
    sysvars[0x4023 - 0x4009] = 1;                 /* S_TOP */
    sysvars[0x4025 - 0x4009] = 0xFF;              /* LAST_K */
    sysvars[0x4026 - 0x4009] = 0xFF;
    sysvars[0x4027 - 0x4009] = 0xFF;
    sysvars[0x4028 - 0x4009] = 55;                /* MARGIN (PAL 50Hz) */
    cb_put_le16(&sysvars[0x4029 - 0x4009], 0x407D);  /* NXTLIN = Line 1 (autostart) */
    cb_put_le16(&sysvars[0x4030 - 0x4009], 0x0C8D);  /* T_ADDR */
    sysvars[0x4034 - 0x4009] = 0xFF;              /* FRAMES */
    sysvars[0x4035 - 0x4009] = 0xFF;
    sysvars[0x4038 - 0x4009] = 0xBC;              /* PR_CC */
    sysvars[0x4039 - 0x4009] = 33;                /* S_POSN col */
    sysvars[0x403A - 0x4009] = 24;                /* S_POSN row */
    sysvars[0x403B - 0x4009] = 0x40;              /* CDFLAG = SLOW mode */
    sysvars[0x405C - 0x4009] = 0x76;              /* PRBUFF end */

    /* Line 1 ($407D): 1 RAND USR VAL "16529" */
    line1[0] = 0x00;
    line1[1] = 0x01;
    cb_put_le16(&line1[2], 11);
    line1[4] = 0xF9;  /* RAND */
    line1[5] = 0xD4;  /* USR */
    line1[6] = 0xC5;  /* VAL */
    line1[7] = 0x0B;  /* " */
    line1[8] = 0x1D;  /* 1 */
    line1[9] = 0x22;  /* 6 */
    line1[10] = 0x21; /* 5 */
    line1[11] = 0x1E; /* 2 */
    line1[12] = 0x25; /* 9 */
    line1[13] = 0x0B; /* " */
    line1[14] = 0x76; /* NL */

    /* Line 2 ($408C): 2 REM <code + data + bss + NL> */
    line2_hdr[0] = 0x00;
    line2_hdr[1] = 0x02;
    cb_put_le16(&line2_hdr[2], (u16)rem_body_len);
    line2_hdr[4] = 0xEA; /* REM */

    if (fwrite(sysvars, 1, sizeof(sysvars), fp) != sizeof(sysvars) ||
        fwrite(line1, 1, sizeof(line1), fp) != sizeof(line1) ||
        fwrite(line2_hdr, 1, sizeof(line2_hdr), fp) != sizeof(line2_hdr)) {
        return 0;
    }
    if (!cb_write_raw(fp, 1)) {
        return 0;
    }
    while (bss_sz-- > 0) {
        if (fputc(0x00, fp) == EOF) {
            return 0;
        }
    }

    /* Expanded 32x24 D_FILE (793 bytes) */
    memset(dfile, 0x00, sizeof(dfile));
    dfile[0] = 0x76;
    for (r = 0; r < 24; r++) {
        dfile[1 + r * 33 + 32] = 0x76;
    }

    /* Line 2 NL + VARS (0x80) + E_LINE (0x76, 0x80) */
    trailer[0] = 0x76;
    trailer[1] = 0x80;
    trailer[2] = 0x76;
    trailer[3] = 0x80;

    if (fwrite(&trailer[0], 1, 1, fp) != 1 ||
        fwrite(dfile, 1, sizeof(dfile), fp) != sizeof(dfile) ||
        fwrite(&trailer[1], 1, 3, fp) != 3) {
        return 0;
    }
    return 1;
}

const Platform plat_z80_zx81 = {"--zx81",   &be_z80,  ZX81_BASE,  "zx81", 1,
                                zx81_begin, zx81_end, zx81_write, NULL};
