/* agon.c - Agon MOS (eZ80 ADL) executable stub for IR4th */
#include "backend/ez80.h"
#include "codebuf.h"
#include "frontend.h"

size_t be_ez80_rsp_off(void);
size_t be_ez80_rstack_top_off(void);

static size_t agon_base = 0x040000UL;

static void agon_begin(size_t base) {
    size_t i;
    agon_base = base;
    /* Reserve JP start (4 bytes) + pad to 0x40 + "MOS" header (5 bytes) */
    cb_emit8(EZ80_JP);
    cb_emit24(0);
    for (i = 4; i < 0x40; i++)
        cb_emit8(0x00);
    cb_emit8('M');
    cb_emit8('O');
    cb_emit8('S');
    cb_emit8(0x00); /* header version 0 */
    cb_emit8(0x01); /* ADL = 1 (24-bit) */
}

static void agon_end(int main_idx) {
    const FuncSig *sig = ir4_func_sig((size_t)main_idx);
    u32 entry = (u32)cb_cur_addr();
    size_t rsp_off = be_ez80_rsp_off();
    size_t rtop_off = be_ez80_rstack_top_off();

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--agon requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    cb_patch24(1, entry);

    /* Preserve MOS entry HL (cmd tail ptr), IX (argv), BC (C = argc) in header pad 0x37..0x3F */
    cb_emit8(EZ80_LD_NN_HL);
    cb_emit24((u32)(agon_base + 0x37));
    cb_emit8(0xDD);
    cb_emit8(0x22);
    cb_emit24((u32)(agon_base + 0x3A));
    cb_emit8(0xED);
    cb_emit8(0x43);
    cb_emit24((u32)(agon_base + 0x3D));

    /* Initialize BSS return stack pointer */
    cb_emit8(EZ80_LD_HL_N24);
    cb_add_data_reloc24(cb_pc(), rtop_off);
    cb_emit24(0);
    cb_emit8(EZ80_LD_NN_HL);
    cb_add_data_reloc24(cb_pc(), rsp_off);
    cb_emit24(0);

    cb_emit8(EZ80_CALL);
    cb_emit24((u32)sig->addr);

    if (sig->out_types.len == 1) {
        cb_emit8(EZ80_POP_BC); /* preserve main() return value in BC for test harness */
    }
    cb_emit8(EZ80_LD_HL_N24);
    cb_emit24(0); /* MOS exit code */
    cb_emit8(EZ80_RET);
}

const Platform plat_ez80_agon = {"--agon", &be_ez80, 0x040000UL, "agon", true,
                                 agon_begin, agon_end, NULL, NULL};
