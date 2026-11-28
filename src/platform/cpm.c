/* cpm.c - CP/M .COM startup & BDOS exit/print stub for Z80 */
#include "codebuf.h"
#include "frontend.h"
#include "backend/z80.h"

#define CPM_BDOS 0x0005

static void cpm_begin(size_t base) {
    (void)base;
    /* Reserve initial JP 0x0000 at 0x0100; patched by cpm_end() */
    cb_emit8_16(Z80_JP, 0x0000);
}

static void cpm_end(int main_idx) {
    const FnSig *sig = ir4_fn_sig(main_idx);
    u16 entry_addr = cb_cur_addr();
    u16 pos_patch;
    u16 print_u16_addr;
    u16 div10_addr;

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--cpm requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    cb_patch16(1, entry_addr);
    be_z80_emit_zero_bss();
    cb_emit8_16(Z80_CALL, sig->addr);

    if (sig->out_types.len == 1) {
        cb_emit8(Z80_POP_HL);

        /* If HL < 0, print '-' and negate HL */
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_A);
        cb_emit8(Z80_JP_P);
        pos_patch = cb_pc();
        cb_emit16(0x0000);

        cb_emit8(Z80_PUSH_HL);
        cb_emit8_8(Z80_LD_C_N, 2);
        cb_emit8_8(Z80_LD_E_N, '-');
        cb_emit8_16(Z80_CALL, CPM_BDOS);
        cb_emit8(Z80_POP_HL);
        cb_emit8(Z80_XOR_A);
        cb_emit8(Z80_SUB_L);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_SBC_A_A);
        cb_emit8(Z80_SUB_H);
        cb_emit8(Z80_LD_H_A);

        cb_patch16(pos_patch, cb_cur_addr());

        cb_emit8(Z80_CALL);
        pos_patch = cb_pc();
        cb_emit16(0x0000);

        cb_emit8_8(Z80_LD_C_N, 2);
        cb_emit8_8(Z80_LD_E_N, '\r');
        cb_emit8_16(Z80_CALL, CPM_BDOS);
        cb_emit8_8(Z80_LD_C_N, 2);
        cb_emit8_8(Z80_LD_E_N, '\n');
        cb_emit8_16(Z80_CALL, CPM_BDOS);
    }

    /* BDOS function 0: system reset / exit */
    cb_emit8_8(Z80_LD_C_N, 0);
    cb_emit8_16(Z80_JP, CPM_BDOS);

    if (sig->out_types.len == 1) {
        print_u16_addr = cb_cur_addr();
        cb_patch16(pos_patch, print_u16_addr);

        /* div10 is placed right after print_u16 */
        cb_emit8(Z80_CALL);
        pos_patch = cb_pc();
        cb_emit16(0x0000);
        cb_emit8(Z80_PUSH_AF);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_L);
        cb_emit8_8(Z80_JR_Z, 3);
        cb_emit8_16(Z80_CALL, print_u16_addr);
        cb_emit8(Z80_POP_AF);
        cb_emit8_8(Z80_ADD_A_N, '0');
        cb_emit8(Z80_LD_E_A);
        cb_emit8_8(Z80_LD_C_N, 2);
        cb_emit8_16(Z80_JP, CPM_BDOS);

        div10_addr = cb_cur_addr();
        cb_patch16(pos_patch, div10_addr);

        /* div10: HL = HL / 10, A = HL % 10 */
        cb_emit8_8(Z80_LD_B_N, 16);
        cb_emit8(Z80_XOR_A);
        cb_emit8(Z80_ADD_HL_HL);
        cb_emit8(Z80_RLA);
        cb_emit8_8(Z80_CP_N, 10);
        cb_emit8_8(Z80_JR_C, 3);
        cb_emit8_8(Z80_SUB_N, 10);
        cb_emit8(Z80_INC_L);
        cb_emit8_8(Z80_DJNZ, (u8)-11);
        cb_emit8(Z80_RET);
    }
}

const Platform plat_z80_cpm = {"--cpm", &be_z80, 0x0100, "cpm", 1, cpm_begin, cpm_end, NULL, NULL, "com"};
