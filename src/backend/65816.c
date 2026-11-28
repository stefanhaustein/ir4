/* 65816.c - W65C816 Native-Mode (16-bit word, 24-bit flat address) Backend
 *
 * Processor state invariant between IR ops:
 *   - Native mode (E=0), 16-bit A (M=0), 16-bit X/Y (X=0), Decimal off
 *   - Direct Page D and Data Bank DBR preserved across IR ops (platform-chosen)
 *   - Data stack in DBR at M816_STACK_BASE ($0200), indexed by X (TOS at $0200,X)
 *     - :i = 2 bytes, :a = 3 bytes, :f / :i32 = 4 bytes
 *   - Return stack: hardware S (JSL/RTL, 3-byte return address)
 *   - Non-recursive locals: static BSS in data segment accessed via 24-bit long
 *   - Recursive locals: 24-bit frame pointer DP_LFP ($10..$13) in BSS rstack
 */
#include "backend/65816.h"
#include "codebuf.h"
#include <math.h>

static size_t m816_rstack_off = 0;
static int m816_rt_inited = 0;
static size_t m816_mul16_addr = 0;
static size_t m816_udivmod16_addr = 0;
static size_t m816_sdivmod16_addr = 0;
static size_t m816_shl16_addr = 0;
static size_t m816_shr16_addr = 0;
static size_t m816_ashr16_addr = 0;
static size_t m816_bcopy_addr = 0;
static size_t m816_fp_fadd_addr = 0;
static size_t m816_fp_fsub_addr = 0;
static size_t m816_fp_fmul_addr = 0;
static size_t m816_fp_fdiv_addr = 0;
static size_t m816_fp_w2f_addr = 0;
static size_t m816_fp_f2w_addr = 0;
static size_t m816_fp_fcmp_addr = 0;
static size_t m816_inx_pc = 0;
static u8 m816_inx_cnt = 0;
static size_t m816_sta_tos_pc = 0;

typedef struct {
    size_t patch_pc;
    size_t br_idx;
} M816FwdPatch;

VEC_OF(M816ByteVec, u8);
VEC_OF(M816FwdPatchVec, M816FwdPatch);

static M816ByteVec m816_br_long;
static M816FwdPatchVec m816_fwd_list;
static size_t m816_br_idx = 0;
static int m816_br_need_relax = 0;

static void m816_peep_fence(void) {
    m816_inx_pc = 0;
    m816_inx_cnt = 0;
    m816_sta_tos_pc = 0;
}

void be_65816_relax_reset(void) {
    m816_br_long.len = 0;
    m816_fwd_list.len = 0;
    m816_br_idx = 0;
    m816_br_need_relax = 0;
}

int be_65816_need_relax(void) { return m816_br_need_relax; }

#define M816_RSTACK_BYTES 65536UL

static void ensure_rt_data(void) {
    if (!m816_rt_inited) {
        m816_rstack_off = cb_alloc_data(M816_RSTACK_BYTES + 2, 1) + M816_RSTACK_BYTES;
        m816_rt_inited = 1;
    }
}

void be_65816_reset(void) {
    m816_rstack_off = 0;
    m816_rt_inited = 0;
    m816_mul16_addr = 0;
    m816_udivmod16_addr = 0;
    m816_sdivmod16_addr = 0;
    m816_shl16_addr = 0;
    m816_shr16_addr = 0;
    m816_ashr16_addr = 0;
    m816_bcopy_addr = 0;
    m816_fp_fadd_addr = 0;
    m816_fp_fsub_addr = 0;
    m816_fp_fmul_addr = 0;
    m816_fp_fdiv_addr = 0;
    m816_fp_w2f_addr = 0;
    m816_fp_f2w_addr = 0;
    m816_fp_fcmp_addr = 0;
    be_65816_relax_reset();
    m816_peep_fence();
}

void be_65816_invalidate_from(size_t addr) {
    m816_peep_fence();
    if (m816_mul16_addr >= addr)
        m816_mul16_addr = 0;
    if (m816_udivmod16_addr >= addr)
        m816_udivmod16_addr = 0;
    if (m816_sdivmod16_addr >= addr)
        m816_sdivmod16_addr = 0;
    if (m816_shl16_addr >= addr)
        m816_shl16_addr = 0;
    if (m816_shr16_addr >= addr)
        m816_shr16_addr = 0;
    if (m816_ashr16_addr >= addr)
        m816_ashr16_addr = 0;
    if (m816_bcopy_addr >= addr)
        m816_bcopy_addr = 0;
    if (m816_fp_w2f_addr >= addr || m816_fp_fadd_addr >= addr) {
        m816_fp_fadd_addr = 0;
        m816_fp_fsub_addr = 0;
        m816_fp_fmul_addr = 0;
        m816_fp_fdiv_addr = 0;
        m816_fp_w2f_addr = 0;
        m816_fp_f2w_addr = 0;
        m816_fp_fcmp_addr = 0;
    }
}

size_t be_65816_rstack_off(void) {
    ensure_rt_data();
    return m816_rstack_off;
}

static size_t m816_cur_addr(void) { return cb_cur_addr(); }

static u8 slot_bytes(StackType t) {
    switch (t) {
    case TY_W:
        return 2;
    case TY_A:
        return 3;
    case TY_F:
    case TY_I32:
        return 4;
    default:
        return 0;
    }
}

static void emit8_16(u8 op, u16 val) {
    cb_emit8(op);
    cb_emit16(val);
}

static void emit8_24(u8 op, u32 val) {
    cb_emit8(op);
    cb_emit24(val);
}

static void emit_sep_m(void) { cb_emit8_8(M816_SEP, 0x20); }
static void emit_rep_m(void) { cb_emit8_8(M816_REP, 0x20); }

static void emit_dex_n(u8 n) {
    u8 i;
    if (ir4_get_opt_level() && m816_inx_pc != 0 && cb_pc() == m816_inx_pc) {
        u8 cancel = (n < m816_inx_cnt) ? n : m816_inx_cnt;
        cb_rewind(cancel);
        n = (u8)(n - cancel);
        m816_inx_cnt = (u8)(m816_inx_cnt - cancel);
        m816_inx_pc = m816_inx_cnt ? cb_pc() : 0;
    } else {
        m816_peep_fence();
    }
    for (i = 0; i < n; i++)
        cb_emit8(M816_DEX);
}

static void emit_inx_n(u8 n) {
    u8 i;
    if (n == 0)
        return;
    if (!ir4_get_opt_level() || m816_inx_pc == 0 || cb_pc() != m816_inx_pc)
        m816_inx_cnt = 0;
    for (i = 0; i < n; i++)
        cb_emit8(M816_INX);
    if (ir4_get_opt_level()) {
        m816_inx_cnt = (u8)(m816_inx_cnt + n);
        m816_inx_pc = cb_pc();
    }
}

/* Load 24-bit data address into DP slot (dp..dp+2) */
static void emit_dp_data_addr(u8 dp, size_t data_off) {
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc(cb_pc(), data_off);
    cb_emit16(0);
    cb_emit8_8(M816_STA_DP, dp);
    emit_sep_m();
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc_bank(cb_pc(), data_off);
    cb_emit8(0);
    cb_emit8_8(M816_STA_DP, (u8)(dp + 2));
    emit_rep_m();
}

void be_65816_emit_zero_bss(void) {
    /* Zero [__bss_start .. __bss_end) in 16-bit words via [DP_PTR0],Y */
    size_t init_sz = cb_init_data_size();
    size_t total_sz = cb_data_size();
    size_t bss_sz = (total_sz > init_sz) ? (total_sz - init_sz) : 0;
    u16 full_banks = (u16)(bss_sz >> 16);
    u16 even_rem = (u16)(bss_sz & 0xFFFEU);

    if (bss_sz == 0)
        return;

    emit_dp_data_addr(M816_DP_PTR0, init_sz);
    emit8_16(M816_LDA_IMM, 0);
    emit8_16(M816_LDY_IMM, 0);

    if (full_banks > 0) {
        size_t bank_loop;
        emit8_16(M816_LDX_IMM, full_banks);
        bank_loop = cb_cur_addr();
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        cb_emit8(M816_INY);
        cb_emit8(M816_INY);
        cb_emit8_8(M816_BNE, (u8)((i8)-6));
        cb_emit8_8(M816_INC_DP, M816_DP_PTR0 + 2);
        cb_emit8(M816_DEX);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)bank_loop - (i32)(cb_cur_addr() + 2))));
    }

    if (even_rem > 0) {
        size_t rem_loop = cb_cur_addr();
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        cb_emit8(M816_INY);
        cb_emit8(M816_INY);
        emit8_16(M816_CPY_IMM, even_rem);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)rem_loop - (i32)(cb_cur_addr() + 2))));
    }

    if (bss_sz & 1U) {
        emit_sep_m();
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        emit_rep_m();
    }
}

static void emit_sta_tos_w(void) {
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    if (ir4_get_opt_level())
        m816_sta_tos_pc = cb_pc();
}

static void emit_load_tos_w_a(void) {
    if (ir4_get_opt_level() && m816_sta_tos_pc != 0 && cb_pc() == m816_sta_tos_pc) {
        cb_rewind(3);
        m816_sta_tos_pc = 0;
        return;
    }
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
}

static void emit_push_w_a(void) {
    emit_dex_n(2);
    emit_sta_tos_w();
}

static void emit_pop_w_a(void) {
    if (ir4_get_opt_level() && m816_sta_tos_pc != 0 && cb_pc() == m816_sta_tos_pc) {
        cb_rewind(3);
        m816_sta_tos_pc = 0;
        emit_inx_n(2);
        return;
    }
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
    emit_inx_n(2);
}

/* Pop 24-bit :a from TOS into 4-byte DP slot dp..dp+3 ([dp]/[dp],Y ignore dp+3) */
static void emit_pop_a_dp(u8 dp) {
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
    cb_emit8_8(M816_STA_DP, dp);
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
    cb_emit8_8(M816_STA_DP, (u8)(dp + 2));
    emit_inx_n(3);
}

/* Push 24-bit :a from dp..dp+2 onto TOS */
static void emit_push_a_dp(u8 dp) {
    emit_dex_n(3);
    cb_emit8_8(M816_LDA_DP, dp);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    emit_sep_m();
    cb_emit8_8(M816_LDA_DP, (u8)(dp + 2));
    emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
    emit_rep_m();
}

/* Pop 32-bit from TOS into dp..dp+3 */
static void emit_pop32_dp(u8 dp) {
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
    cb_emit8_8(M816_STA_DP, dp);
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
    cb_emit8_8(M816_STA_DP, (u8)(dp + 2));
    emit_inx_n(4);
}

/* Push 32-bit from dp..dp+3 onto TOS */
static void emit_push32_dp(u8 dp) {
    emit_dex_n(4);
    cb_emit8_8(M816_LDA_DP, dp);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    cb_emit8_8(M816_LDA_DP, (u8)(dp + 2));
    emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
}

/* Sign-extend 16-bit A into Y (Y = 0 or $FFFF) */
static void emit_sext_a_to_y(void) {
    emit8_16(M816_LDY_IMM, 0x0000);
    emit8_16(M816_CMP_IMM, 0x8000);
    cb_emit8_8(M816_BCC, 1);
    cb_emit8(M816_DEY);
}

/* ---- Runtime math & memory helpers (emitted on demand, skipped by JML) ---- */

static void ensure_mul16(void) {
    size_t skip, loop_pc, skip_add;
    if (m816_mul16_addr)
        return;
    m816_peep_fence();
    cb_emit8(M816_JML);
    skip = cb_pc();
    cb_emit24(0);
    m816_mul16_addr = cb_cur_addr();
    /* DP_TMP0 * DP_TMP1 -> A (16-bit). Uses Y as bit counter. */
    cb_emit8_8(M816_STZ_DP, M816_DP_TMP2);
    emit8_16(M816_LDY_IMM, 16);
    loop_pc = cb_cur_addr();
    cb_emit8_8(M816_LSR_DP, M816_DP_TMP1);
    skip_add = cb_pc();
    cb_emit8_8(M816_BCC, 0);
    cb_emit8(M816_CLC);
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
    cb_emit8_8(M816_ADC_DP, M816_DP_TMP0);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
    cb_patch8(skip_add + 1, (u8)(cb_pc() - (skip_add + 2)));
    cb_emit8_8(M816_ASL_DP, M816_DP_TMP0);
    cb_emit8(M816_DEY);
    cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
    cb_emit8(M816_RTL);
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ensure_udivmod16(void) {
    size_t skip, loop_pc, skip_sub;
    if (m816_udivmod16_addr)
        return;
    m816_peep_fence();
    cb_emit8(M816_JML);
    skip = cb_pc();
    cb_emit24(0);
    m816_udivmod16_addr = cb_cur_addr();
    /* DP_TMP0 (dividend) / DP_TMP1 (divisor) -> quotient in DP_TMP0 (and A), rem in DP_TMP2 */
    cb_emit8_8(M816_STZ_DP, M816_DP_TMP2);
    emit8_16(M816_LDY_IMM, 16);
    loop_pc = cb_cur_addr();
    cb_emit8_8(M816_ASL_DP, M816_DP_TMP0);
    cb_emit8_8(M816_ROL_DP, M816_DP_TMP2);
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
    cb_emit8(M816_SEC);
    cb_emit8_8(M816_SBC_DP, M816_DP_TMP1);
    skip_sub = cb_pc();
    cb_emit8_8(M816_BCC, 0);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
    cb_emit8_8(M816_INC_DP, M816_DP_TMP0);
    cb_patch8(skip_sub + 1, (u8)(cb_pc() - (skip_sub + 2)));
    cb_emit8(M816_DEY);
    cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    cb_emit8(M816_RTL);
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ensure_sdivmod16(void) {
    size_t skip, b1, b2, b3, b4;
    ensure_udivmod16();
    if (m816_sdivmod16_addr)
        return;
    m816_peep_fence();
    cb_emit8(M816_JML);
    skip = cb_pc();
    cb_emit24(0);
    m816_sdivmod16_addr = cb_cur_addr();
    /* DP_TMP0 = dividend, DP_TMP1 = divisor.
       DP_PTR0[0] = quot sign (bit 15), DP_PTR0[2] = rem sign (bit 15). */
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
    cb_emit8_8(M816_EOR_DP, M816_DP_TMP1);
    cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
    /* Abs DP_TMP0 */
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    b1 = cb_pc();
    cb_emit8_8(M816_BPL, 0);
    emit8_16(M816_EOR_IMM, 0xFFFF);
    cb_emit8(M816_INC_A);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
    cb_patch8(b1 + 1, (u8)(cb_pc() - (b1 + 2)));
    /* Abs DP_TMP1 */
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP1);
    b2 = cb_pc();
    cb_emit8_8(M816_BPL, 0);
    emit8_16(M816_EOR_IMM, 0xFFFF);
    cb_emit8(M816_INC_A);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
    cb_patch8(b2 + 1, (u8)(cb_pc() - (b2 + 2)));
    emit8_24(M816_JSL, (u32)m816_udivmod16_addr);
    /* Negate quotient if DP_PTR0[0] < 0 */
    cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
    b3 = cb_pc();
    cb_emit8_8(M816_BPL, 0);
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    emit8_16(M816_EOR_IMM, 0xFFFF);
    cb_emit8(M816_INC_A);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
    cb_patch8(b3 + 1, (u8)(cb_pc() - (b3 + 2)));
    /* Negate remainder if DP_PTR0[2] < 0 */
    cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
    b4 = cb_pc();
    cb_emit8_8(M816_BPL, 0);
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
    emit8_16(M816_EOR_IMM, 0xFFFF);
    cb_emit8(M816_INC_A);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
    cb_patch8(b4 + 1, (u8)(cb_pc() - (b4 + 2)));
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    cb_emit8(M816_RTL);
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ensure_shift16(IrOp op) {
    size_t *addr = (op == OP_LSHIFT) ? &m816_shl16_addr
                   : (op == OP_RSHIFT) ? &m816_shr16_addr
                                       : &m816_ashr16_addr;
    size_t skip, done_br, loop_pc;
    if (*addr)
        return;
    cb_emit8(M816_JML);
    skip = cb_pc();
    cb_emit24(0);
    *addr = cb_cur_addr();
    /* A = value, Y = count (masked & 15) */
    cb_emit8(M816_CPY_IMM);
    cb_emit16(0);
    done_br = cb_pc();
    cb_emit8_8(M816_BEQ, 0);
    loop_pc = cb_cur_addr();
    if (op == OP_LSHIFT) {
        cb_emit8(M816_ASL_A);
    } else if (op == OP_RSHIFT) {
        cb_emit8(M816_LSR_A);
    } else {
        emit8_16(M816_CMP_IMM, 0x8000);
        cb_emit8(M816_ROR_A);
    }
    cb_emit8(M816_DEY);
    cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));
    cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
    cb_emit8(M816_RTL);
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ensure_bcopy(void) {
    size_t skip, done_br, tail_br, odd_done, loop_pc;
    if (m816_bcopy_addr)
        return;
    m816_peep_fence();
    cb_emit8(M816_JML);
    skip = cb_pc();
    cb_emit24(0);
    m816_bcopy_addr = cb_cur_addr();
    /* DP_PTR0 = src (24-bit), DP_PTR1 = dst (24-bit), DP_TMP0 = count (16-bit) */
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    done_br = cb_pc();
    cb_emit8_8(M816_BEQ, 0);
    emit8_16(M816_LDY_IMM, 0);
    emit8_16(M816_AND_IMM, 0xFFFE);
    tail_br = cb_pc();
    cb_emit8_8(M816_BEQ, 0);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
    loop_pc = cb_cur_addr();
    cb_emit8_8(M816_LDA_IDLY, M816_DP_PTR0);
    cb_emit8_8(M816_STA_IDLY, M816_DP_PTR1);
    cb_emit8(M816_INY);
    cb_emit8(M816_INY);
    cb_emit8_8(0xC4, M816_DP_TMP1); /* CPY DP_TMP1 */
    cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));
    cb_patch8(tail_br + 1, (u8)(cb_pc() - (tail_br + 2)));
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
    cb_emit8(M816_LSR_A);
    odd_done = cb_pc();
    cb_emit8_8(M816_BCC, 0);
    emit_sep_m();
    cb_emit8_8(M816_LDA_IDLY, M816_DP_PTR0);
    cb_emit8_8(M816_STA_IDLY, M816_DP_PTR1);
    emit_rep_m();
    cb_patch8(odd_done + 1, (u8)(cb_pc() - (odd_done + 2)));
    cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
    cb_emit8(M816_RTL);
    cb_patch24(skip, (u32)cb_cur_addr());
}

/* ---- Software IEEE-754 single-precision float runtime (32-bit in DP_TMP0/1) ----
 * Layout: DP_TMP0 (4B) = a, DP_TMP1 (4B) = b; result returned in DP_TMP0 (4B).
 * Uses DP_PTR0 (4B: ma lo/hi), DP_PTR1 (4B: mb lo/hi), DP_TMP2 (4B: prod/rem),
 * DP_RET (exp/sign scratch).
 */
static void m816_emit_fp_shr32(u8 dp_lo) {
    /* Shift 32-bit [dp_lo .. dp_lo+3] right by 1 unsigned */
    cb_emit8_8(M816_LSR_DP, (u8)(dp_lo + 2));
    cb_emit8_8(M816_ROR_DP, dp_lo);
}

static void m816_emit_fp_shl32(u8 dp_lo) {
    /* Shift 32-bit [dp_lo .. dp_lo+3] left by 1 */
    cb_emit8_8(M816_ASL_DP, dp_lo);
    cb_emit8_8(M816_ROL_DP, (u8)(dp_lo + 2));
}

static void emit_fp_conv_cmp(void) {
    /* ---- w2f: DP_TMP0[0..1] (signed 16-bit) -> DP_TMP0 (32-bit f32) ---- */
    {
        size_t nz, pos, loop_pc;
        m816_fp_w2f_addr = cb_cur_addr();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        nz = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
        cb_patch8(nz + 1, (u8)(cb_pc() - (nz + 2)));
        cb_emit8_8(M816_STZ_DP, M816_DP_RET); /* sign = 0 */
        pos = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        emit8_16(M816_EOR_IMM, 0xFFFF);
        cb_emit8(M816_INC_A);
        emit8_16(M816_LDY_IMM, 0x8000);
        cb_emit8_8(M816_STY_DP, M816_DP_RET);
        cb_patch8(pos + 1, (u8)(cb_pc() - (pos + 2)));
        /* A = abs(w), normalize left until bit 15 is 1 */
        emit8_16(M816_LDY_IMM, 127 + 15);
        loop_pc = cb_cur_addr();
        emit8_16(M816_CMP_IMM, 0x8000);
        cb_emit8_8(M816_BCS, 4);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BRA, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));
        /* A[14:0] -> mantissa[22:8]; shift A << 8 into 32-bit [DP_TMP0 .. +2] */
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        /* Low byte of A -> high byte of DP_TMP0, high byte of A -> low byte of DP_TMP0+2 */
        cb_emit8(M816_XBA);
        cb_emit8(M816_PHA);
        emit8_16(M816_AND_IMM, 0xFF00);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8(M816_PLA);
        emit8_16(M816_AND_IMM, 0x007F);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        /* Pack exp (Y << 7) | sign (DP_RET) into DP_TMP0+2 */
        cb_emit8(M816_TYA);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8_8(M816_ORA_DP, M816_DP_RET);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
    }

    /* ---- f2w: DP_TMP0 (32-bit f32) -> A (16-bit signed, trunc toward 0) ---- */
    {
        size_t zero_br, neg_done, loop_pc, shift_done;
        m816_fp_f2w_addr = cb_cur_addr();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_RET); /* save sign */
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF); /* A = exp */
        cb_emit8(M816_SEC);
        emit8_16(M816_SBC_IMM, 127);
        zero_br = cb_pc();
        cb_emit8_8(M816_BCC, 0); /* exp < 127 => 0 */
        /* A = unbiased exp (0..15). mant23 = ((hi & $7F) | $80) << 16 | lo */
        emit8_16(M816_CMP_IMM, 15);
        cb_emit8_8(M816_BCC, 3);
        emit8_16(M816_LDA_IMM, 15);
        /* Y = 15 - unbiased_exp (right shifts from bit 15 of (mant >> 8)) */
        cb_emit8(M816_EOR_IMM);
        cb_emit16(0xFFFF);
        cb_emit8(M816_INC_A);
        cb_emit8(M816_CLC);
        emit8_16(M816_ADC_IMM, 15);
        cb_emit8(M816_TAY);
        /* Build top 16 bits of mantissa (bits 23..8) into A: ((hi & $7F)|$80)<<8 | (lo>>8) */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8(M816_XBA);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        /* Shift right Y times */
        cb_emit8(M816_CPY_IMM);
        cb_emit16(0);
        shift_done = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        loop_pc = cb_cur_addr();
        cb_emit8(M816_LSR_A);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));
        cb_patch8(shift_done + 1, (u8)(cb_pc() - (shift_done + 2)));
        cb_emit8_8(M816_LDY_DP, M816_DP_RET);
        neg_done = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        emit8_16(M816_EOR_IMM, 0xFFFF);
        cb_emit8(M816_INC_A);
        cb_patch8(neg_done + 1, (u8)(cb_pc() - (neg_done + 2)));
        cb_emit8(M816_RTL);
        cb_patch8(zero_br + 1, (u8)(cb_pc() - (zero_br + 2)));
        emit8_16(M816_LDA_IMM, 0);
        cb_emit8(M816_RTL);
    }

    /* ---- fcmp: compare DP_TMP0 vs DP_TMP1 -> A = 0 (eq), 1 (a>b), $FFFF (a<b) ---- */
    {
        size_t not_both_z, same_sign, pos_cmp, a_lt, a_gt, eq_hi, eq_done;
        m816_fp_fcmp_addr = cb_cur_addr();
        /* Check both +/- 0.0 */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_RET);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_RET);
        not_both_z = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        emit8_16(M816_LDA_IMM, 0);
        cb_emit8(M816_RTL);
        cb_patch8(not_both_z + 1, (u8)(cb_pc() - (not_both_z + 2)));

        /* Check if signs differ */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_EOR_DP, M816_DP_TMP1 + 2);
        same_sign = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_BMI, 4); /* a < 0 => a < b */
        emit8_16(M816_LDA_IMM, 1);
        cb_emit8(M816_RTL);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        cb_emit8(M816_RTL);
        cb_patch8(same_sign + 1, (u8)(cb_pc() - (same_sign + 2)));

        /* Same sign: compare magnitude (hi then lo) */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_CMP_DP, M816_DP_TMP1 + 2);
        eq_hi = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        pos_cmp = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        /* |a| > |b| */
        a_gt = cb_cur_addr();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_BMI, 4);
        emit8_16(M816_LDA_IMM, 1);
        cb_emit8(M816_RTL);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        cb_emit8(M816_RTL);

        cb_patch8(eq_hi + 1, (u8)(cb_pc() - (eq_hi + 2)));
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_CMP_DP, M816_DP_TMP1);
        eq_done = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        emit8_16(M816_LDA_IMM, 0);
        cb_emit8(M816_RTL);
        cb_patch8(eq_done + 1, (u8)(cb_pc() - (eq_done + 2)));
        cb_emit8_8(M816_BCS, (u8)((i8)((i32)a_gt - (i32)(cb_cur_addr() + 2))));

        cb_patch8(pos_cmp + 1, (u8)(cb_pc() - (pos_cmp + 2)));
        /* |a| < |b| */
        a_lt = cb_cur_addr();
        (void)a_lt;
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_BMI, 4);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        cb_emit8(M816_RTL);
        emit8_16(M816_LDA_IMM, 1);
        cb_emit8(M816_RTL);
    }
}

static void emit_fp_fadd_fsub(void) {
    /* ---- fsub: negate b sign and fall through to fadd ---- */
    m816_fp_fsub_addr = cb_cur_addr();
    cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
    emit8_16(M816_EOR_IMM, 0x8000);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP1 + 2);

    /* ---- fadd: DP_TMP0 + DP_TMP1 -> DP_TMP0 ----
     * Unpack to: DP_PTR0 = ma (32-bit, bit 23 implicit 1), DP_PTR1 = mb (32-bit),
     * DP_RET+0 = ea (16-bit), DP_RET+2 = sa (bit 15), DP_TMP2+0 = eb, DP_TMP2+2 = sb */
    {
        size_t a_nz, b_nz, no_swap, align_done, align_lp, same_s, do_sub, sub_ok;
        size_t norm_chk, norm_lp, ret_zero, pack_done, ov_chk;
        m816_fp_fadd_addr = cb_cur_addr();
        /* If b == 0 return a */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        b_nz = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_emit8(M816_RTL);
        cb_patch8(b_nz + 1, (u8)(cb_pc() - (b_nz + 2)));
        /* If a == 0 return b */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        a_nz = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
        cb_patch8(a_nz + 1, (u8)(cb_pc() - (a_nz + 2)));

        /* Unpack a -> DP_PTR0 (ma), DP_RET (ea), stack (sa); b -> DP_PTR1 (mb), DP_TMP2 (eb) */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8_8(M816_STA_DP, M816_DP_RET); /* ea */

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1 + 2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP2); /* eb */

        /* Save signs: sa in DP_TMP0+2 bit 15, sb in DP_TMP1+2 bit 15.
           If ea < eb, swap a and b (PTR0 <-> PTR1, ea <-> eb, sa <-> sb). */
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8_8(M816_CMP_DP, M816_DP_TMP2);
        no_swap = cb_pc();
        cb_emit8_8(M816_BCS, 0);
        /* Swap PTR0 <-> PTR1 */
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDY_DP, M816_DP_PTR1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1);
        cb_emit8_8(M816_STY_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_LDY_DP, M816_DP_PTR1 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1 + 2);
        cb_emit8_8(M816_STY_DP, M816_DP_PTR0 + 2);
        /* Swap ea <-> eb */
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8_8(M816_LDY_DP, M816_DP_TMP2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_STY_DP, M816_DP_RET);
        /* Swap DP_TMP0+2 <-> DP_TMP1+2 (signs) */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_LDY_DP, M816_DP_TMP1 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1 + 2);
        cb_emit8_8(M816_STY_DP, M816_DP_TMP0 + 2);
        cb_patch8(no_swap + 1, (u8)(cb_pc() - (no_swap + 2)));

        /* Align mb right by (ea - eb), clamped to 25 */
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8(M816_SEC);
        cb_emit8_8(M816_SBC_DP, M816_DP_TMP2);
        align_done = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        emit8_16(M816_CMP_IMM, 25);
        cb_emit8_8(M816_BCC, 3);
        emit8_16(M816_LDA_IMM, 25);
        cb_emit8(M816_TAY);
        align_lp = cb_cur_addr();
        m816_emit_fp_shr32(M816_DP_PTR1);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)align_lp - (i32)(cb_cur_addr() + 2))));
        cb_patch8(align_done + 1, (u8)(cb_pc() - (align_done + 2)));

        /* Check if sa == sb */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_EOR_DP, M816_DP_TMP1 + 2);
        do_sub = cb_pc();
        cb_emit8_8(M816_BMI, 0);
        /* Same sign: ma += mb */
        cb_emit8(M816_CLC);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_ADC_DP, M816_DP_PTR1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_ADC_DP, M816_DP_PTR1 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        /* Check bit 24 ($0100 in high word) overflow */
        emit8_16(M816_CMP_IMM, 0x0100);
        ov_chk = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        m816_emit_fp_shr32(M816_DP_PTR0);
        cb_emit8_8(M816_INC_DP, M816_DP_RET);
        cb_patch8(ov_chk + 1, (u8)(cb_pc() - (ov_chk + 2)));
        same_s = cb_pc();
        cb_emit8_8(M816_BRA, 0);

        /* Different sign: ma -= mb; if < 0 negate ma and flip sa */
        cb_patch8(do_sub + 1, (u8)(cb_pc() - (do_sub + 2)));
        cb_emit8(M816_SEC);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR1 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        sub_ok = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        /* Negate 32-bit DP_PTR0 */
        cb_emit8(M816_SEC);
        emit8_16(M816_LDA_IMM, 0);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        emit8_16(M816_LDA_IMM, 0);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_EOR_IMM, 0x8000);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        cb_patch8(sub_ok + 1, (u8)(cb_pc() - (sub_ok + 2)));

        /* Normalize DP_PTR0 left until bit 23 ($0080 in high word) is set */
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_ORA_DP, M816_DP_PTR0 + 2);
        ret_zero = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        norm_lp = cb_cur_addr();
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
        emit8_16(M816_AND_IMM, 0x0080);
        norm_chk = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        m816_emit_fp_shl32(M816_DP_PTR0);
        cb_emit8_8(M816_DEC_DP, M816_DP_RET);
        cb_emit8_8(M816_BEQ, 2);
        cb_emit8_8(M816_BRA, (u8)((i8)((i32)norm_lp - (i32)(cb_cur_addr() + 2))));
        /* underflow -> 0 */
        cb_patch8(ret_zero + 1, (u8)(cb_pc() - (ret_zero + 2)));
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);

        cb_patch8(norm_chk + 1, (u8)(cb_pc() - (norm_chk + 2)));
        cb_patch8(same_s + 1, (u8)(cb_pc() - (same_s + 2)));
        /* Pack: sa (DP_TMP0+2 & $8000) | (ea << 7) | (PTR0+2 & $7F) */
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x8000);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        pack_done = cb_pc();
        (void)pack_done;
        cb_emit8(M816_RTL);
    }
}

static void emit_fp_fmul_fdiv(void) {
    /* ---- fmul: DP_TMP0 * DP_TMP1 -> DP_TMP0 ----
     * 24x24 -> top 24 bits via 24-iteration shift-and-add on 32-bit accumulator */
    {
        size_t a_nz, b_nz, zero_out, loop_pc, skip_add, norm_done;
        m816_fp_fmul_addr = cb_cur_addr();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        a_nz = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        zero_out = cb_cur_addr();
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
        cb_patch8(a_nz + 1, (u8)(cb_pc() - (a_nz + 2)));
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        b_nz = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_emit8_8(M816_BRA, (u8)((i8)((i32)zero_out - (i32)(cb_cur_addr() + 2))));
        cb_patch8(b_nz + 1, (u8)(cb_pc() - (b_nz + 2)));

        /* Sign = (sa ^ sb) & $8000 -> DP_RET+2 */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_EOR_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x8000);
        cb_emit8_8(M816_STA_DP, M816_DP_RET + 2);

        /* Exp = ea + eb - 127 -> DP_RET */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8_8(M816_STA_DP, M816_DP_RET);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8(M816_CLC);
        cb_emit8_8(M816_ADC_DP, M816_DP_RET);
        cb_emit8(M816_SEC);
        emit8_16(M816_SBC_IMM, 127);
        cb_emit8_8(M816_STA_DP, M816_DP_RET);

        /* DP_PTR0 = ma (24-bit), DP_PTR1 = mb (24-bit), DP_TMP2 = 0 (32-bit prod) */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1 + 2);

        cb_emit8_8(M816_STZ_DP, M816_DP_TMP2);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP2 + 2);
        emit8_16(M816_LDY_IMM, 24);
        loop_pc = cb_cur_addr();
        m816_emit_fp_shr32(M816_DP_PTR1);
        skip_add = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        cb_emit8(M816_CLC);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_ADC_DP, M816_DP_PTR0);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2 + 2);
        cb_emit8_8(M816_ADC_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP2 + 2);
        cb_patch8(skip_add + 1, (u8)(cb_pc() - (skip_add + 2)));
        m816_emit_fp_shr32(M816_DP_TMP2);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));

        /* Product bit 23 or bit 24 is set; normalize:
           After 24 shr1s, product sits at bit 22..23 (aligned so bit 23 = $0080 in hi if >= 2.0,
           or bit 22 = $0040 if < 2.0!). Wait: 1.0 * 1.0 = (2^23)*(2^23) >> 24 = 2^22 ($0040)! */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2 + 2);
        emit8_16(M816_AND_IMM, 0x0080);
        norm_done = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        cb_emit8_8(M816_INC_DP, M816_DP_RET);
        cb_emit8_8(M816_BRA, 4);
        cb_patch8(norm_done + 1, (u8)(cb_pc() - (norm_done + 2)));
        m816_emit_fp_shl32(M816_DP_TMP2);

        /* Pack DP_TMP2 into DP_TMP0 */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8_8(M816_ORA_DP, M816_DP_RET + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
    }

    /* ---- fdiv: DP_TMP0 / DP_TMP1 -> DP_TMP0 ----
     * 24-iteration restoring division of ma / mb -> 24-bit quotient in DP_TMP2 */
    {
        size_t a_nz, loop_pc, skip_sub, norm_ok;
        m816_fp_fdiv_addr = cb_cur_addr();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        a_nz = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
        cb_patch8(a_nz + 1, (u8)(cb_pc() - (a_nz + 2)));

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_EOR_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x8000);
        cb_emit8_8(M816_STA_DP, M816_DP_RET + 2);

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8_8(M816_STA_DP, M816_DP_RET);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x7FFF);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8(M816_SEC);
        cb_emit8_8(M816_SBC_DP, M816_DP_TMP2);
        cb_emit8(M816_CLC);
        emit8_16(M816_ADC_IMM, 127);
        cb_emit8_8(M816_STA_DP, M816_DP_RET);

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP1 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        emit8_16(M816_ORA_IMM, 0x0080);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1 + 2);

        cb_emit8_8(M816_STZ_DP, M816_DP_TMP2);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP2 + 2);
        emit8_16(M816_LDY_IMM, 24);
        loop_pc = cb_cur_addr();
        m816_emit_fp_shl32(M816_DP_TMP2);
        /* If PTR0 >= PTR1: PTR0 -= PTR1, TMP2 |= 1 */
        cb_emit8(M816_SEC);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR1);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0 + 2);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR1 + 2);
        skip_sub = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        cb_emit8(M816_SEC);
        cb_emit8_8(M816_LDA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_SBC_DP, M816_DP_PTR1);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8_8(M816_INC_DP, M816_DP_TMP2);
        cb_patch8(skip_sub + 1, (u8)(cb_pc() - (skip_sub + 2)));
        m816_emit_fp_shl32(M816_DP_PTR0);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)loop_pc - (i32)(cb_cur_addr() + 2))));

        /* Normalize left if bit 23 = 0 */
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2 + 2);
        emit8_16(M816_AND_IMM, 0x0080);
        norm_ok = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        m816_emit_fp_shl32(M816_DP_TMP2);
        cb_emit8_8(M816_DEC_DP, M816_DP_RET);
        cb_patch8(norm_ok + 1, (u8)(cb_pc() - (norm_ok + 2)));

        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_LDA_DP, M816_DP_RET);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8(M816_ASL_A);
        cb_emit8_8(M816_ORA_DP, M816_DP_RET + 2);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2 + 2);
        emit8_16(M816_AND_IMM, 0x007F);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP1);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
        cb_emit8(M816_RTL);
    }
}

static void ensure_fp(void) {
    size_t skip;
    if (m816_fp_fadd_addr)
        return;

    m816_peep_fence();
    cb_emit8(M816_JML);
    skip = cb_pc();
    cb_emit24(0);
    emit_fp_conv_cmp();
    emit_fp_fadd_fsub();
    emit_fp_fmul_fdiv();
    cb_patch24(skip, (u32)cb_cur_addr());
}

/* ---- Backend interface methods ---- */

static size_t m816_stack_bytes = 0;
static int m816_cur_is_recursive = 0;
static size_t m816_static_base = 0;
static size_t m816_frame_bytes = 0;
static size_t m816_local_off[128];

static int m816_emit_dp_mem_off(u8 dp, IrOp op, u16 uoff);
static void m816_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes);

static void m816_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                          size_t total_locals, Bool is_recursive) {
    size_t i;
    (void)name;
    ensure_rt_data();
    m816_peep_fence();
    m816_fwd_list.len = 0;
    m816_br_idx = 0;
    m816_br_need_relax = 0;
    m816_stack_bytes = 0;
    m816_cur_is_recursive = is_recursive ? 1 : 0;
    m816_frame_bytes = 0;
    m816_static_base = 0;

    for (i = 0; i < total_locals; i++) {
        size_t sz = (locals[i].size > 0) ? locals[i].size : slot_bytes(locals[i].type);
        m816_local_off[i] = m816_frame_bytes;
        m816_frame_bytes += sz;
    }

    if (!is_recursive) {
        if (m816_frame_bytes > 0)
            m816_static_base = cb_alloc_data(m816_frame_bytes + 1, 1);
    } else if (m816_frame_bytes > 0) {
        /* Allocate frame downward so DP_LFP points directly to local offset 0 */
        cb_emit8(M816_SEC);
        cb_emit8_8(M816_LDA_DP, M816_DP_LFP);
        emit8_16(M816_SBC_IMM, (u16)(m816_frame_bytes & 0xFFFF));
        cb_emit8_8(M816_STA_DP, M816_DP_LFP);
        cb_emit8_8(M816_BCS, 6);
        emit_sep_m();
        cb_emit8_8(M816_DEC_DP, M816_DP_LFP + 2);
        emit_rep_m();
    }

    /* Pop incoming parameters from TOS in reverse order (param_cnt-1 .. 0) */
    for (i = param_cnt; i > 0; i--) {
        size_t idx = i - 1;
        StackType pt = (locals[idx].size > 0) ? TY_A : locals[idx].type;
        m816_local_op(LOCAL_SET, idx, pt, 0);
    }
}

static void m816_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
    (void)locals;
    (void)total_locals;
    if (sig->is_recursive && m816_frame_bytes > 0) {
        cb_emit8(M816_CLC);
        cb_emit8_8(M816_LDA_DP, M816_DP_LFP);
        emit8_16(M816_ADC_IMM, (u16)(m816_frame_bytes & 0xFFFF));
        cb_emit8_8(M816_STA_DP, M816_DP_LFP);
        cb_emit8_8(M816_BCC, 6);
        emit_sep_m();
        cb_emit8_8(M816_INC_DP, M816_DP_LFP + 2);
        emit_rep_m();
    }
    cb_emit8(M816_RTL);
}

static void m816_call(size_t target_addr) { emit8_24(M816_JSL, (u32)target_addr); }

static int m816_tail_call(size_t target_addr, const FuncSig *sig, const FuncSig *callee,
                          const LocalVar *locals, size_t total_locals) {
    (void)callee;
    (void)locals;
    (void)total_locals;
    if (sig->is_recursive && m816_frame_bytes > 0) {
        cb_emit8(M816_CLC);
        cb_emit8_8(M816_LDA_DP, M816_DP_LFP);
        emit8_16(M816_ADC_IMM, (u16)(m816_frame_bytes & 0xFFFF));
        cb_emit8_8(M816_STA_DP, M816_DP_LFP);
        cb_emit8_8(M816_BCC, 6);
        emit_sep_m();
        cb_emit8_8(M816_INC_DP, M816_DP_LFP + 2);
        emit_rep_m();
    }
    emit8_24(M816_JML, (u32)target_addr);
    return 1;
}

static void m816_call_extern(const FuncSig *sig) {
    (void)sig;
    ir4_die("extern functions are not supported on 65816");
}

static void m816_push_w(i32 val) {
    u16 u = (u16)(val & 0xFFFF);
    emit_dex_n(2);
    if (u == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    } else {
        emit8_16(M816_LDA_IMM, u);
        emit_sta_tos_w();
    }
}

static void m816_push_a(u32 val) {
    u16 lo = (u16)(val & 0xFFFF);
    u8 bk = (u8)((val >> 16) & 0xFF);
    emit_dex_n(3);
    if (lo == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    } else {
        emit8_16(M816_LDA_IMM, lo);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    }
    emit_sep_m();
    if (bk == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE + 2);
    } else {
        cb_emit8_8(M816_LDA_IMM, bk);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
    }
    emit_rep_m();
}

static void m816_push_f(double val) {
    float f = (float)val;
    u32 bits;
    u16 lo, hi;
    memcpy(&bits, &f, sizeof(bits));
    lo = (u16)(bits & 0xFFFF);
    hi = (u16)((bits >> 16) & 0xFFFF);
    emit_dex_n(4);
    if (lo == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    } else {
        emit8_16(M816_LDA_IMM, lo);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    }
    if (hi == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE + 2);
    } else {
        emit8_16(M816_LDA_IMM, hi);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
    }
}

static void m816_push_i32(i32 val) {
    u32 bits = (u32)val;
    u16 lo = (u16)(bits & 0xFFFF);
    u16 hi = (u16)((bits >> 16) & 0xFFFF);
    emit_dex_n(4);
    if (lo == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    } else {
        emit8_16(M816_LDA_IMM, lo);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    }
    if (hi == 0) {
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE + 2);
    } else {
        emit8_16(M816_LDA_IMM, hi);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
    }
}

static void m816_push_data(size_t data_offset) {
    emit_dex_n(3);
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc(cb_pc(), data_offset);
    cb_emit16(0);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    emit_sep_m();
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc_bank(cb_pc(), data_offset);
    cb_emit8(0);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
    emit_rep_m();
}

static void m816_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
    size_t l_off = m816_local_off[slot];
    (void)stack_bytes;
    if (!m816_cur_is_recursive) {
        size_t addr_off = m816_static_base + l_off;
        /* Static BSS local */
        if (op == LOCAL_ADDR) {
            m816_push_data(addr_off);
        } else if (op == LOCAL_GET) {
            if (type == TY_W) {
                emit_dex_n(2);
                cb_emit8(M816_LDA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
                emit_sta_tos_w();
            } else if (type == TY_A) {
                emit_dex_n(3);
                cb_emit8(M816_LDA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
                emit8_16(M816_STA_ABSX, M816_STACK_BASE);
                emit_sep_m();
                cb_emit8(M816_LDA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off + 2);
                cb_emit24(0);
                emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
                emit_rep_m();
            } else {
                emit_dex_n(4);
                cb_emit8(M816_LDA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
                emit8_16(M816_STA_ABSX, M816_STACK_BASE);
                cb_emit8(M816_LDA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off + 2);
                cb_emit24(0);
                emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
            }
        } else if (op == LOCAL_SET) {
            if (type == TY_W) {
                emit_pop_w_a();
                cb_emit8(M816_STA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
            } else if (type == TY_A) {
                emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
                cb_emit8(M816_STA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
                emit_sep_m();
                emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
                cb_emit8(M816_STA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off + 2);
                cb_emit24(0);
                emit_rep_m();
                emit_inx_n(3);
            } else {
                emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
                cb_emit8(M816_STA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
                emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
                cb_emit8(M816_STA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off + 2);
                cb_emit24(0);
                emit_inx_n(4);
            }
        } else { /* LOCAL_ADD (TY_W or TY_A) */
            emit_pop_w_a();
            if (type == TY_A)
                emit_sext_a_to_y();
            cb_emit8(M816_CLC);
            cb_emit8(0x6F); /* ADC long */
            cb_add_data_reloc24(cb_pc(), addr_off);
            cb_emit24(0);
            cb_emit8(M816_STA_LONG);
            cb_add_data_reloc24(cb_pc(), addr_off);
            cb_emit24(0);
            if (type == TY_A) {
                cb_emit8(M816_TYA);
                emit_sep_m();
                cb_emit8(0x6F); /* ADC long */
                cb_add_data_reloc24(cb_pc(), addr_off + 2);
                cb_emit24(0);
                cb_emit8(M816_STA_LONG);
                cb_add_data_reloc24(cb_pc(), addr_off + 2);
                cb_emit24(0);
                emit_rep_m();
            }
        }
        return;
    }

    /* Recursive local at DP_LFP + l_off (0 <= l_off <= 0xFFF0) */
    {
        u16 uoff = (u16)l_off;
        if (op == LOCAL_ADDR) {
            if (uoff == 0) {
                emit_push_a_dp(M816_DP_LFP);
            } else {
                size_t skip;
                emit_dex_n(3);
                cb_emit8(M816_CLC);
                cb_emit8_8(M816_LDA_DP, M816_DP_LFP);
                emit8_16(M816_ADC_IMM, uoff);
                emit8_16(M816_STA_ABSX, M816_STACK_BASE);
                emit_sep_m();
                cb_emit8_8(M816_LDA_DP, M816_DP_LFP + 2);
                skip = cb_pc();
                cb_emit8_8(M816_BCC, 0);
                cb_emit8(M816_INC_A);
                cb_patch8(skip + 1, (u8)(cb_pc() - (skip + 2)));
                emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
                emit_rep_m();
            }
        } else if (op == LOCAL_GET) {
            IrOp mop = (type == TY_W) ? OP_FETCH : (type == TY_A) ? OP_AFETCH : OP_I32FETCH;
            (void)m816_emit_dp_mem_off(M816_DP_LFP, mop, uoff);
        } else if (op == LOCAL_SET) {
            IrOp mop = (type == TY_W) ? OP_STORE : (type == TY_A) ? OP_ASTORE : OP_I32STORE;
            (void)m816_emit_dp_mem_off(M816_DP_LFP, mop, uoff);
        } else { /* LOCAL_ADD (TY_W or TY_A) */
            emit_pop_w_a();
            if (type == TY_A) {
                emit_sext_a_to_y();
                cb_emit8_8(M816_STY_DP, M816_DP_TMP0);
            }
            if (uoff == 0) {
                cb_emit8(M816_CLC);
                cb_emit8_8(0x67, M816_DP_LFP); /* ADC [DP_LFP] */
                cb_emit8_8(M816_STA_IDL, M816_DP_LFP);
                if (type == TY_A)
                    emit8_16(M816_LDY_IMM, 2);
            } else {
                emit8_16(M816_LDY_IMM, uoff);
                cb_emit8(M816_CLC);
                cb_emit8_8(0x77, M816_DP_LFP); /* ADC [DP_LFP],Y */
                cb_emit8_8(M816_STA_IDLY, M816_DP_LFP);
                if (type == TY_A) {
                    cb_emit8(M816_INY);
                    cb_emit8(M816_INY);
                }
            }
            if (type == TY_A) {
                emit_sep_m();
                cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
                cb_emit8_8(0x77, M816_DP_LFP); /* ADC [DP_LFP],Y */
                cb_emit8_8(M816_STA_IDLY, M816_DP_LFP);
                emit_rep_m();
            }
        }
    }
}

static int m816_local_add_imm(size_t slot, StackType type, size_t stack_bytes, i32 imm) {
    i32 simm = (i32)(i16)(u16)imm;
    (void)stack_bytes;
    if (type != TY_W && type != TY_A)
        return 0;
    if (simm == 0)
        return 1;
    if (!m816_cur_is_recursive) {
        size_t addr_off = m816_static_base + m816_local_off[slot];
        cb_emit8(M816_CLC);
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), addr_off);
        cb_emit24(0);
        emit8_16(M816_ADC_IMM, (u16)(simm & 0xFFFF));
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), addr_off);
        cb_emit24(0);
        if (type == TY_A) {
            size_t skip = cb_pc();
            cb_emit8_8(simm > 0 ? M816_BCC : M816_BCS, 0);
            emit_sep_m();
            cb_emit8(M816_LDA_LONG);
            cb_add_data_reloc24(cb_pc(), addr_off + 2);
            cb_emit24(0);
            cb_emit8(simm > 0 ? M816_INC_A : M816_DEC_A);
            cb_emit8(M816_STA_LONG);
            cb_add_data_reloc24(cb_pc(), addr_off + 2);
            cb_emit24(0);
            emit_rep_m();
            cb_patch8(skip + 1, (u8)(cb_pc() - (skip + 2)));
        }
        return 1;
    }
    {
        u16 uoff = (u16)m816_local_off[slot];
        if (uoff == 0) {
            cb_emit8(M816_CLC);
            cb_emit8_8(M816_LDA_IDL, M816_DP_LFP);
            emit8_16(M816_ADC_IMM, (u16)(simm & 0xFFFF));
            cb_emit8_8(M816_STA_IDL, M816_DP_LFP);
        } else {
            emit8_16(M816_LDY_IMM, uoff);
            cb_emit8(M816_CLC);
            cb_emit8_8(M816_LDA_IDLY, M816_DP_LFP);
            emit8_16(M816_ADC_IMM, (u16)(simm & 0xFFFF));
            cb_emit8_8(M816_STA_IDLY, M816_DP_LFP);
        }
        if (type == TY_A) {
            size_t skip = cb_pc();
            cb_emit8_8(simm > 0 ? M816_BCC : M816_BCS, 0);
            if (uoff == 0) {
                emit8_16(M816_LDY_IMM, 2);
            } else {
                cb_emit8(M816_INY);
                cb_emit8(M816_INY);
            }
            emit_sep_m();
            cb_emit8_8(M816_LDA_IDLY, M816_DP_LFP);
            cb_emit8(simm > 0 ? M816_INC_A : M816_DEC_A);
            cb_emit8_8(M816_STA_IDLY, M816_DP_LFP);
            emit_rep_m();
            cb_patch8(skip + 1, (u8)(cb_pc() - (skip + 2)));
        }
        return 1;
    }
}

static void m816_stack_op(StackOp op, StackType t1, StackType t2) {
    u8 s1 = slot_bytes(t1);
    u8 s2 = slot_bytes(t2);
    if (op == OP_DROP) {
        emit_inx_n(s1);
    } else if (op == OP_DUP) {
        if (s1 == 2) {
            emit_dex_n(2);
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
            emit_sta_tos_w();
        } else if (s1 == 3) {
            emit_dex_n(3);
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 3);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            emit_sep_m();
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 5);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
            emit_rep_m();
        } else {
            emit_dex_n(4);
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        }
    } else { /* OP_SWAP: swap s1 bytes at 0,X with s2 bytes at s1,X */
        u8 i;
        /* Save s1 bytes into DP_TMP0 */
        for (i = 0; i + 1 < s1; i += 2) {
            emit8_16(M816_LDA_ABSX, (u16)(M816_STACK_BASE + i));
            cb_emit8_8(M816_STA_DP, (u8)(M816_DP_TMP0 + i));
        }
        if (s1 & 1) {
            emit_sep_m();
            emit8_16(M816_LDA_ABSX, (u16)(M816_STACK_BASE + s1 - 1));
            cb_emit8_8(M816_STA_DP, (u8)(M816_DP_TMP0 + s1 - 1));
            emit_rep_m();
        }
        /* Copy s2 bytes from s1,X -> 0,X (overlap-safe if s1 < s2? Wait: copying byte/word low-to-high is safe when moving down from s1,X to 0,X!) */
        for (i = 0; i + 1 < s2; i += 2) {
            emit8_16(M816_LDA_ABSX, (u16)(M816_STACK_BASE + s1 + i));
            emit8_16(M816_STA_ABSX, (u16)(M816_STACK_BASE + i));
        }
        if (s2 & 1) {
            emit_sep_m();
            emit8_16(M816_LDA_ABSX, (u16)(M816_STACK_BASE + s1 + s2 - 1));
            emit8_16(M816_STA_ABSX, (u16)(M816_STACK_BASE + s2 - 1));
            emit_rep_m();
        }
        /* Store DP_TMP0 (s1 bytes) -> s2,X */
        for (i = 0; i + 1 < s1; i += 2) {
            cb_emit8_8(M816_LDA_DP, (u8)(M816_DP_TMP0 + i));
            emit8_16(M816_STA_ABSX, (u16)(M816_STACK_BASE + s2 + i));
        }
        if (s1 & 1) {
            emit_sep_m();
            cb_emit8_8(M816_LDA_DP, (u8)(M816_DP_TMP0 + s1 - 1));
            emit8_16(M816_STA_ABSX, (u16)(M816_STACK_BASE + s2 + s1 - 1));
            emit_rep_m();
        }
    }
}

static void emit_cmp16_result(IrOp op) {
    /* Compare NOS (2,X) with TOS (0,X), pop 2, leave 1/0 in 0,X */
    size_t true_br, done_br;
    cb_emit8(M816_SEC);
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
    emit8_16(M816_SBC_ABSX, M816_STACK_BASE);
    if (op == OP_EQ) {
        true_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
    } else if (op == OP_NE) {
        true_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
    } else if (op == OP_ULT) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
    } else if (op == OP_UGE) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCS, 0);
    } else if (op == OP_UGT) {
        /* !(a <= b) => C=1 and Z=0 */
        size_t f_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        true_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
        goto false_path;
    } else if (op == OP_ULE) {
        /* C=0 or Z=1 */
        size_t t1 = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        true_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        emit_inx_n(2);
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
        done_br = cb_pc();
        cb_emit8_8(M816_BRA, 0);
        cb_patch8(t1 + 1, (u8)(cb_pc() - (t1 + 2)));
        cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
        emit_inx_n(2);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
        return;
    } else if (op == OP_LT || op == OP_GE) {
        /* Signed N^V: BVC +2; EOR #$8000; BMI is_lt */
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        true_br = cb_pc();
        {
            u8 opc = (op == OP_LT) ? M816_BMI : M816_BPL;
            cb_emit8_8(opc, 0);
        }
    } else { /* OP_GT or OP_LE */
        size_t z_br = cb_pc();
        if (op == OP_GT) {
            cb_emit8_8(M816_BEQ, 0);
            cb_emit8_8(M816_BVC, 3);
            emit8_16(M816_EOR_IMM, 0x8000);
            true_br = cb_pc();
            cb_emit8_8(M816_BPL, 0);
            cb_patch8(z_br + 1, (u8)(cb_pc() - (z_br + 2)));
        } else {
            size_t t1 = cb_pc();
            cb_emit8_8(M816_BEQ, 0);
            cb_emit8_8(M816_BVC, 3);
            emit8_16(M816_EOR_IMM, 0x8000);
            true_br = cb_pc();
            cb_emit8_8(M816_BMI, 0);
            emit_inx_n(2);
            emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
            done_br = cb_pc();
            cb_emit8_8(M816_BRA, 0);
            cb_patch8(t1 + 1, (u8)(cb_pc() - (t1 + 2)));
            cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
            emit_inx_n(2);
            emit8_16(M816_LDA_IMM, 0xFFFF);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
            return;
        }
    }
false_path:
    emit_inx_n(2);
    emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    done_br = cb_pc();
    cb_emit8_8(M816_BRA, 0);
    cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
    emit_inx_n(2);
    emit8_16(M816_LDA_IMM, 0xFFFF);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
}

static void emit_cmp24_a(IrOp op) {
    /* Compare 24-bit NOS (3,X..5,X) with TOS (0,X..2,X), pop 6, push 2 (-1/0) */
    size_t true_br, done_br, neq_hi;
    if (op == OP_AEQ || op == OP_ANE) {
        size_t f_br = cb_pc();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 3);
        emit8_16(M816_CMP_ABSX, M816_STACK_BASE);
        f_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        emit_sep_m();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 5);
        emit8_16(M816_CMP_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        true_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
        emit_inx_n(4);
        {
            u16 imm = (op == OP_ANE) ? 0xFFFFU : 0U;
            emit8_16(M816_LDA_IMM, imm);
        }
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        done_br = cb_pc();
        cb_emit8_8(M816_BRA, 0);
        cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
        emit_inx_n(4);
        {
            u16 imm = (op == OP_AEQ) ? 0xFFFFU : 0U;
            emit8_16(M816_LDA_IMM, imm);
        }
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
        return;
    }
    /* OP_ALT / OP_AGT: compare bank byte first, then low 16 bits */
    emit_sep_m();
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 5);
    emit8_16(M816_CMP_ABSX, M816_STACK_BASE + 2);
    emit_rep_m();
    neq_hi = cb_pc();
    cb_emit8_8(M816_BNE, 0);
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 3);
    emit8_16(M816_CMP_ABSX, M816_STACK_BASE);
    cb_patch8(neq_hi + 1, (u8)(cb_pc() - (neq_hi + 2)));
    if (op == OP_ALT) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
    } else {
        size_t f_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        true_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
    }
    emit_inx_n(4);
    emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    done_br = cb_pc();
    cb_emit8_8(M816_BRA, 0);
    cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
    emit_inx_n(4);
    emit8_16(M816_LDA_IMM, 0xFFFF);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
}

static void emit_cmp32_result(IrOp op) {
    /* Compare 32-bit NOS (4,X..7,X) with TOS (0,X..3,X), pop 8, push 2 (-1/0) */
    size_t true_br, done_br;
    cb_emit8(M816_SEC);
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
    emit8_16(M816_SBC_ABSX, M816_STACK_BASE);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
    emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
    emit8_16(M816_SBC_ABSX, M816_STACK_BASE + 2);
    cb_emit8_8(M816_STA_DP, M816_DP_TMP0 + 2);
    if (op == OP_EQ32 || op == OP_NE32) {
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        true_br = cb_pc();
        {
            u8 opc = (op == OP_EQ32) ? M816_BEQ : M816_BNE;
            cb_emit8_8(opc, 0);
        }
    } else if (op == OP_ULT32) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
    } else if (op == OP_UGE32) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCS, 0);
    } else if (op == OP_UGT32) {
        size_t f_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        true_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
    } else if (op == OP_ULE32) {
        size_t t1 = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        cb_emit8_8(M816_ORA_DP, M816_DP_TMP0);
        true_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        emit_inx_n(6);
        emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
        done_br = cb_pc();
        cb_emit8_8(M816_BRA, 0);
        cb_patch8(t1 + 1, (u8)(cb_pc() - (t1 + 2)));
        cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
        emit_inx_n(6);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
        return;
    } else if (op == OP_LT32 || op == OP_GE32) {
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        true_br = cb_pc();
        {
            u8 opc = (op == OP_LT32) ? M816_BMI : M816_BPL;
            cb_emit8_8(opc, 0);
        }
    } else { /* OP_GT32 / OP_LE32 */
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        if (op == OP_GT32) {
            size_t t_gt = cb_pc();
            cb_emit8_8(M816_BMI, 0);
            cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
            cb_emit8_8(M816_ORA_DP, M816_DP_TMP0 + 2);
            true_br = cb_pc();
            cb_emit8_8(M816_BNE, 0);
            cb_patch8(t_gt + 1, (u8)(cb_pc() - (t_gt + 2)));
        } else {
            size_t t1 = cb_pc();
            cb_emit8_8(M816_BMI, 0);
            cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
            cb_emit8_8(M816_ORA_DP, M816_DP_TMP0 + 2);
            true_br = cb_pc();
            cb_emit8_8(M816_BEQ, 0);
            emit_inx_n(6);
            emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
            done_br = cb_pc();
            cb_emit8_8(M816_BRA, 0);
            cb_patch8(t1 + 1, (u8)(cb_pc() - (t1 + 2)));
            cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
            emit_inx_n(6);
            emit8_16(M816_LDA_IMM, 0xFFFF);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
            return;
        }
    }
    emit_inx_n(6);
    emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    done_br = cb_pc();
    cb_emit8_8(M816_BRA, 0);
    cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
    emit_inx_n(6);
    emit8_16(M816_LDA_IMM, 0xFFFF);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
}

static int m816_op_mem_conv(IrOp op);
static int m816_op_32_fp(IrOp op);

static void m816_op(IrOp op) {
    switch (op) {
    case OP_ADD:
    case OP_AND:
    case OP_OR:
    case OP_XOR: {
        u8 alu_op = (op == OP_ADD)   ? M816_ADC_ABSX
                    : (op == OP_AND) ? M816_AND_ABSX
                    : (op == OP_OR)  ? M816_ORA_ABSX
                                     : M816_EOR_ABSX;
        if (ir4_get_opt_level() && m816_sta_tos_pc != 0 && cb_pc() == m816_sta_tos_pc) {
            cb_rewind(3);
            m816_sta_tos_pc = 0;
            emit_inx_n(2);
            if (op == OP_ADD)
                cb_emit8(M816_CLC);
            emit8_16(alu_op, M816_STACK_BASE);
            emit_sta_tos_w();
            break;
        }
        if (op == OP_ADD)
            cb_emit8(M816_CLC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        emit8_16(alu_op, M816_STACK_BASE);
        emit_inx_n(2);
        emit_sta_tos_w();
        break;
    }
    case OP_SUB:
        cb_emit8(M816_SEC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        emit8_16(M816_SBC_ABSX, M816_STACK_BASE);
        emit_inx_n(2);
        emit_sta_tos_w();
        break;
    case OP_INVERT:
        emit_load_tos_w_a();
        emit8_16(M816_EOR_IMM, 0xFFFF);
        emit_sta_tos_w();
        break;
    case OP_MUL:
        ensure_mul16();
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit8_24(M816_JSL, (u32)m816_mul16_addr);
        emit_sta_tos_w();
        break;
    case OP_DIV:
    case OP_MOD:
        ensure_sdivmod16();
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit8_24(M816_JSL, (u32)m816_sdivmod16_addr);
        if (op == OP_MOD)
            cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        emit_sta_tos_w();
        break;
    case OP_UDIV:
    case OP_UMOD:
        ensure_udivmod16();
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP1);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit8_24(M816_JSL, (u32)m816_udivmod16_addr);
        if (op == OP_UMOD)
            cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        emit_sta_tos_w();
        break;
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        ensure_shift16(op);
        emit_pop_w_a();
        emit8_16(M816_AND_IMM, 31);
        cb_emit8(M816_TAY);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        {
            u32 target = (u32)(op == OP_LSHIFT   ? m816_shl16_addr
                               : op == OP_RSHIFT ? m816_shr16_addr
                                                 : m816_ashr16_addr);
            emit8_24(M816_JSL, target);
        }
        emit_sta_tos_w();
        break;
    case OP_AADD:
        emit_pop_w_a();
        emit_sext_a_to_y();
        cb_emit8(M816_CLC);
        emit8_16(M816_ADC_ABSX, M816_STACK_BASE);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_emit8(M816_TYA);
        emit_sep_m();
        emit8_16(M816_ADC_ABSX, M816_STACK_BASE + 2);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        break;
    case OP_ASUB:
        emit_pop_w_a();
        emit_sext_a_to_y();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8(M816_SEC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8_8(M816_SBC_DP, M816_DP_TMP0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        emit_sep_m();
        cb_emit8(M816_TYA);
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8_8(M816_SBC_DP, M816_DP_TMP0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        break;
    case OP_ADIFF:
        cb_emit8(M816_SEC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 3);
        emit8_16(M816_SBC_ABSX, M816_STACK_BASE);
        emit_inx_n(4);
        emit_sta_tos_w();
        break;
    case OP_EQ:
    case OP_NE:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE:
    case OP_ULT:
    case OP_UGT:
    case OP_ULE:
    case OP_UGE:
        emit_cmp16_result(op);
        break;
    case OP_AEQ:
    case OP_ANE:
    case OP_ALT:
    case OP_AGT:
        emit_cmp24_a(op);
        break;
    default:
        if (!m816_op_mem_conv(op) && !m816_op_32_fp(op))
            ir4_die("unsupported IR op in 65816 backend");
        break;
    }
}

static int m816_op_mem_conv(IrOp op) {
    switch (op) {
    case OP_I8FETCH:
    case OP_U8FETCH:
        emit_pop_a_dp(M816_DP_PTR0);
        emit_dex_n(2);
        emit_sep_m();
        cb_emit8_8(M816_LDA_IDL, M816_DP_PTR0);
        emit_rep_m();
        emit8_16(M816_AND_IMM, 0x00FF);
        if (op == OP_I8FETCH) {
            emit8_16(M816_CMP_IMM, 0x0080);
            cb_emit8_8(M816_BCC, 3);
            emit8_16(M816_ORA_IMM, 0xFF00);
        }
        emit_sta_tos_w();
        break;
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_FETCH:
        emit_pop_a_dp(M816_DP_PTR0);
        emit_dex_n(2);
        cb_emit8_8(M816_LDA_IDL, M816_DP_PTR0);
        emit_sta_tos_w();
        break;
    case OP_I8STORE:
        emit_pop_a_dp(M816_DP_PTR0);
        emit_pop_w_a();
        emit_sep_m();
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        emit_rep_m();
        break;
    case OP_SI8STORE:
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit_pop_a_dp(M816_DP_PTR0);
        emit_sep_m();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        emit_rep_m();
        break;
    case OP_I16STORE:
    case OP_STORE:
        emit_pop_a_dp(M816_DP_PTR0);
        emit_pop_w_a();
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        break;
    case OP_SI16STORE:
    case OP_SSTORE:
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit_pop_a_dp(M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        break;
    case OP_AFETCH:
        emit_pop_a_dp(M816_DP_PTR0);
        emit_dex_n(3);
        cb_emit8_8(M816_LDA_IDL, M816_DP_PTR0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        emit8_16(M816_LDY_IMM, 2);
        emit_sep_m();
        cb_emit8_8(M816_LDA_IDLY, M816_DP_PTR0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        break;
    case OP_ASTORE:
        emit_pop_a_dp(M816_DP_PTR0);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        emit8_16(M816_LDY_IMM, 2);
        emit_sep_m();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        emit_rep_m();
        emit_inx_n(3);
        break;
    case OP_SASTORE:
        emit_pop_a_dp(M816_DP_TMP0);
        emit_pop_a_dp(M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        emit8_16(M816_LDY_IMM, 2);
        emit_sep_m();
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        emit_rep_m();
        break;
    case OP_FFETCH:
    case OP_I32FETCH:
        emit_pop_a_dp(M816_DP_PTR0);
        emit_dex_n(4);
        cb_emit8_8(M816_LDA_IDL, M816_DP_PTR0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        emit8_16(M816_LDY_IMM, 2);
        cb_emit8_8(M816_LDA_IDLY, M816_DP_PTR0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_FSTORE:
    case OP_I32STORE:
        emit_pop_a_dp(M816_DP_PTR0);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        emit8_16(M816_LDY_IMM, 2);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        emit_inx_n(4);
        break;
    case OP_SFSTORE:
        emit_pop32_dp(M816_DP_TMP0);
        emit_pop_a_dp(M816_DP_PTR0);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STA_IDL, M816_DP_PTR0);
        emit8_16(M816_LDY_IMM, 2);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0 + 2);
        cb_emit8_8(M816_STA_IDLY, M816_DP_PTR0);
        break;
    case OP_BCOPY:
        ensure_bcopy();
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit_pop_a_dp(M816_DP_PTR0);
        emit_pop_a_dp(M816_DP_PTR1);
        emit8_24(M816_JSL, (u32)m816_bcopy_addr);
        break;
    case OP_SEXT:
        emit_load_tos_w_a();
        emit8_16(M816_AND_IMM, 0x00FF);
        emit8_16(M816_CMP_IMM, 0x0080);
        cb_emit8_8(M816_BCC, 3);
        emit8_16(M816_ORA_IMM, 0xFF00);
        emit_sta_tos_w();
        break;
    case OP_W2A:
        emit_load_tos_w_a();
        emit_sext_a_to_y();
        cb_emit8(M816_DEX);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        emit_sep_m();
        cb_emit8(M816_TYA);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        break;
    case OP_A2W:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8(M816_INX);
        emit_sta_tos_w();
        break;
    case OP_W2I32:
    case OP_U2I32:
        emit_load_tos_w_a();
        if (op == OP_W2I32)
            emit_sext_a_to_y();
        else
            emit8_16(M816_LDY_IMM, 0);
        cb_emit8(M816_DEX);
        cb_emit8(M816_DEX);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_emit8(M816_TYA);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_I322W:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        emit_inx_n(2);
        emit_sta_tos_w();
        break;
    default:
        return 0;
    }
    return 1;
}

static int m816_op_32_fp(IrOp op) {
    switch (op) {
    case OP_ADD32:
        cb_emit8(M816_CLC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_ADC_ABSX, M816_STACK_BASE);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
        emit8_16(M816_ADC_ABSX, M816_STACK_BASE + 2);
        emit_inx_n(4);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_SUB32:
        cb_emit8(M816_SEC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_SBC_ABSX, M816_STACK_BASE);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
        emit8_16(M816_SBC_ABSX, M816_STACK_BASE + 2);
        emit_inx_n(4);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_AND32:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_AND_ABSX, M816_STACK_BASE);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
        emit8_16(M816_AND_ABSX, M816_STACK_BASE + 2);
        emit_inx_n(4);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_OR32:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_ORA_ABSX, M816_STACK_BASE);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
        emit8_16(M816_ORA_ABSX, M816_STACK_BASE + 2);
        emit_inx_n(4);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_XOR32:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_EOR_ABSX, M816_STACK_BASE);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 4);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 6);
        emit8_16(M816_EOR_ABSX, M816_STACK_BASE + 2);
        emit_inx_n(4);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_INV32:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        emit8_16(M816_EOR_IMM, 0xFFFF);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        emit8_16(M816_EOR_IMM, 0xFFFF);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_EQ32:
    case OP_NE32:
    case OP_LT32:
    case OP_GT32:
    case OP_LE32:
    case OP_GE32:
    case OP_ULT32:
    case OP_UGT32:
    case OP_ULE32:
    case OP_UGE32:
        emit_cmp32_result(op);
        break;
    case OP_FNEG:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        emit8_16(M816_EOR_IMM, 0x8000);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        break;
    case OP_W2F:
        ensure_fp();
        emit_pop_w_a();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        emit8_24(M816_JSL, (u32)m816_fp_w2f_addr);
        emit_push32_dp(M816_DP_TMP0);
        break;
    case OP_F2W:
        ensure_fp();
        emit_pop32_dp(M816_DP_TMP0);
        emit8_24(M816_JSL, (u32)m816_fp_f2w_addr);
        emit_push_w_a();
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
        ensure_fp();
        emit_pop32_dp(M816_DP_TMP1);
        emit_pop32_dp(M816_DP_TMP0);
        {
            u32 target = (u32)(op == OP_FADD   ? m816_fp_fadd_addr
                               : op == OP_FSUB ? m816_fp_fsub_addr
                               : op == OP_FMUL ? m816_fp_fmul_addr
                                               : m816_fp_fdiv_addr);
            emit8_24(M816_JSL, target);
        }
        emit_push32_dp(M816_DP_TMP0);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT: {
        size_t t_br, done_br;
        ensure_fp();
        emit_pop32_dp(M816_DP_TMP1);
        emit_pop32_dp(M816_DP_TMP0);
        emit8_24(M816_JSL, (u32)m816_fp_fcmp_addr);
        if (op == OP_FEQ) {
            t_br = cb_pc();
            cb_emit8_8(M816_BEQ, 0);
        } else if (op == OP_FNE) {
            t_br = cb_pc();
            cb_emit8_8(M816_BNE, 0);
        } else if (op == OP_FLT) {
            t_br = cb_pc();
            cb_emit8_8(M816_BMI, 0);
        } else { /* OP_FGT: A == 1 => not <= 0 */
            size_t f_br = cb_pc();
            cb_emit8_8(M816_BMI, 0);
            t_br = cb_pc();
            cb_emit8_8(M816_BNE, 0);
            cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
        }
        emit8_16(M816_LDA_IMM, 0);
        done_br = cb_pc();
        cb_emit8_8(M816_BRA, 0);
        cb_patch8(t_br + 1, (u8)(cb_pc() - (t_br + 2)));
        emit8_16(M816_LDA_IMM, 0xFFFF);
        cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
        emit_push_w_a();
        break;
    }
    default:
        return 0;
    }
    return 1;
}

static int m816_add_imm(int is_addr, i32 imm) {
    if (imm == 0)
        return 1;
    if (!is_addr) {
        u16 d = (u16)(imm & 0xFFFF);
        emit_load_tos_w_a();
        cb_emit8(M816_CLC);
        emit8_16(M816_ADC_IMM, d);
        emit_sta_tos_w();
        return 1;
    }
    if (imm < -32768 || imm > 32768)
        return 0;
    {
        size_t skip;
        cb_emit8(M816_CLC);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        emit8_16(M816_ADC_IMM, (u16)(imm & 0xFFFF));
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        skip = cb_pc();
        cb_emit8_8(imm > 0 ? M816_BCC : M816_BCS, 0);
        emit_sep_m();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8(imm > 0 ? M816_INC_A : M816_DEC_A);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        cb_patch8(skip + 1, (u8)(cb_pc() - (skip + 2)));
        return 1;
    }
}

static int m816_cmp_imm(IrOp op, i32 imm) {
    u16 rhs = (u16)(imm & 0xFFFF);
    size_t true_br, done_br;
    size_t true2_br = 0;
    int has_true2 = 0;

    if (op < OP_EQ || op > OP_UGE)
        return 0;
    if (rhs == 0 && op == OP_EQ) {
        size_t nz_br, done_eq;
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        nz_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        done_eq = cb_pc();
        cb_emit8_8(M816_BRA, 0);
        cb_patch8(nz_br + 1, (u8)(cb_pc() - (nz_br + 2)));
        emit8_16(M816_LDA_IMM, 0);
        cb_patch8(done_eq + 1, (u8)(cb_pc() - (done_eq + 2)));
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        return 1;
    }
    if (rhs == 0 && op == OP_NE) {
        size_t z_br;
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        z_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        emit8_16(M816_LDA_IMM, 0xFFFF);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_patch8(z_br + 1, (u8)(cb_pc() - (z_br + 2)));
        return 1;
    }

    emit_load_tos_w_a();
    if (op == OP_LT || op == OP_GE || op == OP_LE || op == OP_GT) {
        cb_emit8(M816_SEC);
        emit8_16(M816_SBC_IMM, rhs);
    } else {
        emit8_16(M816_CMP_IMM, rhs);
    }

    if (op == OP_EQ) {
        true_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
    } else if (op == OP_NE) {
        true_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
    } else if (op == OP_ULT) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
    } else if (op == OP_UGE) {
        true_br = cb_pc();
        cb_emit8_8(M816_BCS, 0);
    } else if (op == OP_ULE) {
        true2_br = cb_pc();
        has_true2 = 1;
        cb_emit8_8(M816_BCC, 0);
        true_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
    } else if (op == OP_UGT) {
        size_t f_br = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        true_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
    } else if (op == OP_LT || op == OP_GE) {
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        true_br = cb_pc();
        cb_emit8_8((op == OP_LT) ? M816_BMI : M816_BPL, 0);
    } else if (op == OP_LE) {
        true2_br = cb_pc();
        has_true2 = 1;
        cb_emit8_8(M816_BEQ, 0);
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        true_br = cb_pc();
        cb_emit8_8(M816_BMI, 0);
    } else { /* OP_GT */
        size_t f_br = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        true_br = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        cb_patch8(f_br + 1, (u8)(cb_pc() - (f_br + 2)));
    }

    emit8_16(M816_STZ_ABSX, M816_STACK_BASE);
    done_br = cb_pc();
    cb_emit8_8(M816_BRA, 0);
    cb_patch8(true_br + 1, (u8)(cb_pc() - (true_br + 2)));
    if (has_true2)
        cb_patch8(true2_br + 1, (u8)(cb_pc() - (true2_br + 2)));
    emit8_16(M816_LDA_IMM, 0xFFFF);
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    cb_patch8(done_br + 1, (u8)(cb_pc() - (done_br + 2)));
    return 1;
}

static int m816_shift_imm(IrOp op, u8 count) {
    u8 k;
    if (op != OP_LSHIFT && op != OP_RSHIFT && op != OP_ARSHIFT)
        return 0;
    if (count == 0)
        return 1;
    if (count == 8 && op == OP_LSHIFT) {
        emit_load_tos_w_a();
        emit8_16(M816_AND_IMM, 0x00FF);
        cb_emit8(M816_XBA);
        emit_sta_tos_w();
        return 1;
    }
    if (count == 8 && op == OP_RSHIFT) {
        emit_load_tos_w_a();
        cb_emit8(M816_XBA);
        emit8_16(M816_AND_IMM, 0x00FF);
        emit_sta_tos_w();
        return 1;
    }
    if (op == OP_ARSHIFT ? (count > 3) : (count > 6))
        return 0;
    emit_load_tos_w_a();
    for (k = 0; k < count; k++) {
        if (op == OP_LSHIFT) {
            cb_emit8(M816_ASL_A);
        } else if (op == OP_RSHIFT) {
            cb_emit8(M816_LSR_A);
        } else {
            emit8_16(M816_CMP_IMM, 0x8000);
            cb_emit8(M816_ROR_A);
        }
    }
    emit_sta_tos_w();
    return 1;
}

static int m816_bit_imm(IrOp op, i32 imm) {
    u16 v = (u16)(imm & 0xFFFF);
    if (op != OP_AND && op != OP_OR && op != OP_XOR)
        return 0;
    emit_load_tos_w_a();
    emit8_16(op == OP_AND ? M816_AND_IMM : (op == OP_OR ? M816_ORA_IMM : M816_EOR_IMM), v);
    emit_sta_tos_w();
    return 1;
}

/* Emit load/store through dp + u16_off (0 <= u16_off <= 0xFFF0) */
static int m816_emit_dp_mem_off(u8 dp, IrOp op, u16 uoff) {
    switch (op) {
    case OP_I8FETCH:
    case OP_U8FETCH:
        emit_dex_n(2);
        if (uoff != 0)
            emit8_16(M816_LDY_IMM, uoff);
        emit_sep_m();
        cb_emit8_8(uoff ? M816_LDA_IDLY : M816_LDA_IDL, dp);
        emit_rep_m();
        emit8_16(M816_AND_IMM, 0x00FF);
        if (op == OP_I8FETCH) {
            emit8_16(M816_CMP_IMM, 0x0080);
            cb_emit8_8(M816_BCC, 3);
            emit8_16(M816_ORA_IMM, 0xFF00);
        }
        emit_sta_tos_w();
        return 1;
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_FETCH:
        emit_dex_n(2);
        if (uoff != 0)
            emit8_16(M816_LDY_IMM, uoff);
        cb_emit8_8(uoff ? M816_LDA_IDLY : M816_LDA_IDL, dp);
        emit_sta_tos_w();
        return 1;
    case OP_I8STORE:
        emit_pop_w_a();
        if (uoff != 0)
            emit8_16(M816_LDY_IMM, uoff);
        emit_sep_m();
        cb_emit8_8(uoff ? M816_STA_IDLY : M816_STA_IDL, dp);
        emit_rep_m();
        return 1;
    case OP_I16STORE:
    case OP_STORE:
        emit_pop_w_a();
        if (uoff != 0)
            emit8_16(M816_LDY_IMM, uoff);
        cb_emit8_8(uoff ? M816_STA_IDLY : M816_STA_IDL, dp);
        return 1;
    case OP_AFETCH:
        emit_dex_n(3);
        if (uoff != 0) {
            emit8_16(M816_LDY_IMM, uoff);
            cb_emit8_8(M816_LDA_IDLY, dp);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            cb_emit8(M816_INY);
            cb_emit8(M816_INY);
        } else {
            cb_emit8_8(M816_LDA_IDL, dp);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            emit8_16(M816_LDY_IMM, 2);
        }
        emit_sep_m();
        cb_emit8_8(M816_LDA_IDLY, dp);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        return 1;
    case OP_ASTORE:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        if (uoff != 0) {
            emit8_16(M816_LDY_IMM, uoff);
            cb_emit8_8(M816_STA_IDLY, dp);
            cb_emit8(M816_INY);
            cb_emit8(M816_INY);
        } else {
            cb_emit8_8(M816_STA_IDL, dp);
            emit8_16(M816_LDY_IMM, 2);
        }
        emit_sep_m();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8_8(M816_STA_IDLY, dp);
        emit_rep_m();
        emit_inx_n(3);
        return 1;
    case OP_FFETCH:
    case OP_I32FETCH:
        emit_dex_n(4);
        if (uoff != 0) {
            emit8_16(M816_LDY_IMM, uoff);
            cb_emit8_8(M816_LDA_IDLY, dp);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            cb_emit8(M816_INY);
            cb_emit8(M816_INY);
        } else {
            cb_emit8_8(M816_LDA_IDL, dp);
            emit8_16(M816_STA_ABSX, M816_STACK_BASE);
            emit8_16(M816_LDY_IMM, 2);
        }
        cb_emit8_8(M816_LDA_IDLY, dp);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        return 1;
    case OP_FSTORE:
    case OP_I32STORE:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        if (uoff != 0) {
            emit8_16(M816_LDY_IMM, uoff);
            cb_emit8_8(M816_STA_IDLY, dp);
            cb_emit8(M816_INY);
            cb_emit8(M816_INY);
        } else {
            cb_emit8_8(M816_STA_IDL, dp);
            emit8_16(M816_LDY_IMM, 2);
        }
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8_8(M816_STA_IDLY, dp);
        emit_inx_n(4);
        return 1;
    default:
        return 0;
    }
}

static int m816_mem_imm(IrOp op, i32 off) {
    if (off < 0 || off > 0x7FF0)
        return 0;
    if (op < OP_I8FETCH || op > OP_I32STORE)
        return 0;
    emit_pop_a_dp(M816_DP_PTR0);
    return m816_emit_dp_mem_off(M816_DP_PTR0, op, (u16)off);
}

static int m816_data_mem(IrOp op, size_t data_offset) {
    switch (op) {
    case OP_I8FETCH:
    case OP_U8FETCH:
        emit_dex_n(2);
        emit_sep_m();
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit_rep_m();
        emit8_16(M816_AND_IMM, 0x00FF);
        if (op == OP_I8FETCH) {
            emit8_16(M816_CMP_IMM, 0x0080);
            cb_emit8_8(M816_BCC, 3);
            emit8_16(M816_ORA_IMM, 0xFF00);
        }
        emit_sta_tos_w();
        return 1;
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_FETCH:
        emit_dex_n(2);
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit_sta_tos_w();
        return 1;
    case OP_I8STORE:
        emit_pop_w_a();
        emit_sep_m();
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit_rep_m();
        return 1;
    case OP_I16STORE:
    case OP_STORE:
        emit_pop_w_a();
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        return 1;
    case OP_AFETCH:
        emit_dex_n(3);
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        emit_sep_m();
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset + 2);
        cb_emit24(0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        emit_rep_m();
        return 1;
    case OP_ASTORE:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit_sep_m();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset + 2);
        cb_emit24(0);
        emit_rep_m();
        emit_inx_n(3);
        return 1;
    case OP_FFETCH:
    case OP_I32FETCH:
        emit_dex_n(4);
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE);
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset + 2);
        cb_emit24(0);
        emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
        return 1;
    case OP_FSTORE:
    case OP_I32STORE:
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE);
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset);
        cb_emit24(0);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE + 2);
        cb_emit8(M816_STA_LONG);
        cb_add_data_reloc24(cb_pc(), data_offset + 2);
        cb_emit24(0);
        emit_inx_n(4);
        return 1;
    default:
        return 0;
    }
}

static int m816_local_mem(size_t slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)stack_bytes;
    if (op < OP_I8FETCH || op > OP_I32STORE)
        return 0;
    if (!m816_cur_is_recursive) {
        long base_off = (long)m816_local_off[slot] + (long)off;
        if (base_off < 0)
            return 0;
        return m816_data_mem(op, m816_static_base + (size_t)base_off);
    }
    {
        long rec_off = (long)m816_local_off[slot] + (long)off;
        if (rec_off < 0 || rec_off > 0xFFF0L)
            return 0;
        return m816_emit_dp_mem_off(M816_DP_LFP, op, (u16)rec_off);
    }
}

static int m816_local_deref(size_t slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)stack_bytes;
    if (off < 0 || off > 0x7FF0)
        return 0;
    if (op < OP_I8FETCH || op > OP_I32STORE)
        return 0;
    if (!m816_cur_is_recursive) {
        size_t addr_off = m816_static_base + m816_local_off[slot];
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), addr_off);
        cb_emit24(0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
        cb_emit8(M816_LDA_LONG);
        cb_add_data_reloc24(cb_pc(), addr_off + 2);
        cb_emit24(0);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
    } else {
        u16 uoff = (u16)m816_local_off[slot];
        if (uoff == 0) {
            cb_emit8_8(M816_LDA_IDL, M816_DP_LFP);
            cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
            emit8_16(M816_LDY_IMM, 2);
            cb_emit8_8(M816_LDA_IDLY, M816_DP_LFP);
            cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        } else {
            emit8_16(M816_LDY_IMM, uoff);
            cb_emit8_8(M816_LDA_IDLY, M816_DP_LFP);
            cb_emit8_8(M816_STA_DP, M816_DP_PTR0);
            cb_emit8(M816_INY);
            cb_emit8(M816_INY);
            cb_emit8_8(M816_LDA_IDLY, M816_DP_LFP);
            cb_emit8_8(M816_STA_DP, M816_DP_PTR0 + 2);
        }
    }
    return m816_emit_dp_mem_off(M816_DP_PTR0, op, (u16)off);
}

static int m816_take_short_fwd(size_t *out_bidx) {
    size_t idx = m816_br_idx++;
    if (idx >= m816_br_long.len)
        VEC_PUSH(m816_br_long, 0);
    *out_bidx = idx;
    return ir4_get_opt_level() && !m816_br_long.data[idx];
}

static size_t m816_emit_fwd_short(u8 op, size_t bidx) {
    size_t pc = cb_pc();
    M816FwdPatch *fp = VEC_ADD(m816_fwd_list);
    fp->patch_pc = pc;
    fp->br_idx = bidx;
    cb_emit8_8(op, 0);
    return pc;
}

static int m816_emit_back_short(u8 op, size_t target_addr) {
    if (ir4_get_opt_level()) {
        i32 rel8 = (i32)target_addr - (i32)(cb_cur_addr() + 2);
        if (rel8 >= -128 && rel8 <= 127) {
            cb_emit8_8(op, (u8)(i8)rel8);
            return 1;
        }
    }
    return 0;
}

static int m816_cmp_br_emit(IrOp op, int has_imm, i32 imm, int is_fwd, size_t target_addr,
                            size_t *patch_offset) {
    size_t skip1 = 0, skip2 = 0;
    size_t bidx = 0;
    int has_skip2 = 0;
    int is_short = 0;

    if (!((!has_imm && (op == OP_AEQ || op == OP_ANE)) || (op >= OP_EQ && op <= OP_UGE)))
        return 0;
    if (is_fwd)
        is_short = m816_take_short_fwd(&bidx);

    if (!has_imm && (op == OP_AEQ || op == OP_ANE)) {
        size_t neq_br;
        emit_inx_n(6);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE - 3);
        emit8_16(M816_CMP_ABSX, M816_STACK_BASE - 6);
        neq_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        emit_sep_m();
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE - 1);
        emit8_16(M816_CMP_ABSX, M816_STACK_BASE - 4);
        emit_rep_m();
        if (op == OP_AEQ) {
            skip1 = cb_pc();
            cb_emit8_8(M816_BEQ, 0);
            cb_patch8(neq_br + 1, (u8)(cb_pc() - (neq_br + 2)));
        } else {
            size_t eq_br = cb_pc();
            cb_emit8_8(M816_BEQ, 0);
            cb_patch8(neq_br + 1, (u8)(cb_pc() - (neq_br + 2)));
            skip1 = cb_pc();
            cb_emit8_8(M816_BRA, 0);
            cb_patch8(eq_br + 1, (u8)(cb_pc() - (eq_br + 2)));
        }
        goto emit_target_brl;
    }

    if (has_imm) {
        u16 rhs = (u16)(imm & 0xFFFF);
        if ((op == OP_LE || op == OP_GT) && (i16)rhs < 32767) {
            rhs = (u16)(rhs + 1U);
            op = (op == OP_LE) ? OP_LT : OP_GE;
        } else if ((op == OP_ULE || op == OP_UGT) && rhs < 0xFFFFU) {
            rhs = (u16)(rhs + 1U);
            op = (op == OP_ULE) ? OP_ULT : OP_UGE;
        }
        if (rhs == 0 && (op == OP_EQ || op == OP_NE || op == OP_LT || op == OP_GE)) {
            u8 false_op = (op == OP_EQ)   ? M816_BNE
                          : (op == OP_NE) ? M816_BEQ
                          : (op == OP_LT) ? M816_BPL
                                          : M816_BMI;
            u8 true_op = (op == OP_EQ)   ? M816_BEQ
                         : (op == OP_NE) ? M816_BNE
                         : (op == OP_LT) ? M816_BMI
                                         : M816_BPL;
            if (ir4_get_opt_level() && m816_sta_tos_pc != 0 && cb_pc() == m816_sta_tos_pc) {
                cb_rewind(3);
                m816_sta_tos_pc = 0;
                emit_inx_n(2);
                emit8_16(M816_CMP_IMM, 0);
            } else {
                emit_inx_n(2);
                emit8_16(M816_LDA_ABSX, M816_STACK_BASE - 2);
            }
            if (is_fwd && is_short) {
                *patch_offset = m816_emit_fwd_short(false_op, bidx);
                return 1;
            }
            if (!is_fwd && m816_emit_back_short(false_op, target_addr))
                return 1;
            skip1 = cb_pc();
            cb_emit8_8(true_op, 0);
            goto emit_target_brl;
        }
        emit_pop_w_a();
        if (op == OP_LT || op == OP_GE || op == OP_LE || op == OP_GT) {
            cb_emit8(M816_SEC);
            emit8_16(M816_SBC_IMM, rhs);
        } else {
            emit8_16(M816_CMP_IMM, rhs);
        }
    } else {
        emit_inx_n(4);
        emit8_16(M816_LDA_ABSX, M816_STACK_BASE - 2);
        if (op == OP_LT || op == OP_GE || op == OP_LE || op == OP_GT) {
            cb_emit8(M816_SEC);
            emit8_16(M816_SBC_ABSX, M816_STACK_BASE - 4);
        } else {
            emit8_16(M816_CMP_ABSX, M816_STACK_BASE - 4);
        }
    }

    if (op == OP_EQ || op == OP_NE || op == OP_ULT || op == OP_UGE || op == OP_LT || op == OP_GE) {
        u8 false_op = (op == OP_EQ)    ? M816_BNE
                      : (op == OP_NE)  ? M816_BEQ
                      : (op == OP_ULT) ? M816_BCS
                      : (op == OP_UGE) ? M816_BCC
                      : (op == OP_LT)  ? M816_BPL
                                       : M816_BMI;
        u8 true_op = (op == OP_EQ)    ? M816_BEQ
                     : (op == OP_NE)  ? M816_BNE
                     : (op == OP_ULT) ? M816_BCC
                     : (op == OP_UGE) ? M816_BCS
                     : (op == OP_LT)  ? M816_BMI
                                      : M816_BPL;
        if (op == OP_LT || op == OP_GE) {
            cb_emit8_8(M816_BVC, 3);
            emit8_16(M816_EOR_IMM, 0x8000);
        }
        if (is_fwd && is_short) {
            *patch_offset = m816_emit_fwd_short(false_op, bidx);
            return 1;
        }
        if (!is_fwd && m816_emit_back_short(false_op, target_addr))
            return 1;
        skip1 = cb_pc();
        cb_emit8_8(true_op, 0);
    } else if (op == OP_ULE) {
        skip2 = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        if (is_fwd && is_short) {
            *patch_offset = m816_emit_fwd_short(M816_BCS, bidx);
            cb_patch8(skip2 + 1, (u8)(cb_pc() - (skip2 + 2)));
            return 1;
        }
        if (!is_fwd && m816_emit_back_short(M816_BCS, target_addr)) {
            cb_patch8(skip2 + 1, (u8)(cb_pc() - (skip2 + 2)));
            return 1;
        }
        has_skip2 = 1;
        skip1 = cb_pc();
        cb_emit8_8(M816_BCC, 0);
    } else if (op == OP_UGT) {
        size_t to_brl = cb_pc();
        cb_emit8_8(M816_BCC, 0);
        skip1 = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_patch8(to_brl + 1, (u8)(cb_pc() - (to_brl + 2)));
    } else if (op == OP_LE) {
        skip2 = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        if (is_fwd && is_short) {
            *patch_offset = m816_emit_fwd_short(M816_BPL, bidx);
            cb_patch8(skip2 + 1, (u8)(cb_pc() - (skip2 + 2)));
            return 1;
        }
        if (!is_fwd && m816_emit_back_short(M816_BPL, target_addr)) {
            cb_patch8(skip2 + 1, (u8)(cb_pc() - (skip2 + 2)));
            return 1;
        }
        has_skip2 = 1;
        skip1 = cb_pc();
        cb_emit8_8(M816_BMI, 0);
    } else { /* OP_GT */
        size_t to_brl = cb_pc();
        cb_emit8_8(M816_BEQ, 0);
        cb_emit8_8(M816_BVC, 3);
        emit8_16(M816_EOR_IMM, 0x8000);
        skip1 = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        cb_patch8(to_brl + 1, (u8)(cb_pc() - (to_brl + 2)));
    }

emit_target_brl:
    if (is_fwd) {
        if (is_short) {
            *patch_offset = m816_emit_fwd_short(M816_BRA, bidx);
        } else {
            *patch_offset = cb_pc();
            cb_emit8_16(M816_BRL, 0);
        }
    } else if (!m816_emit_back_short(M816_BRA, target_addr)) {
        i32 rel = (i32)target_addr - (i32)(cb_cur_addr() + 3);
        cb_emit8_16(M816_BRL, (u16)(rel & 0xFFFF));
    }
    cb_patch8(skip1 + 1, (u8)(cb_pc() - (skip1 + 2)));
    if (has_skip2)
        cb_patch8(skip2 + 1, (u8)(cb_pc() - (skip2 + 2)));
    return 1;
}

static int m816_cmp_br_fwd(IrOp op, int has_imm, i32 imm, size_t *patch_offset) {
    return m816_cmp_br_emit(op, has_imm, imm, 1, 0, patch_offset);
}

static int m816_cmp_br_back(IrOp op, int has_imm, i32 imm, size_t target_addr) {
    return m816_cmp_br_emit(op, has_imm, imm, 0, target_addr, NULL);
}

static size_t m816_emit_br_fwd(BranchCond cond) {
    size_t patch_off, bidx;
    int is_short = m816_take_short_fwd(&bidx);
    if (cond == BR_JZ) {
        size_t nz_br;
        if (ir4_get_opt_level() && m816_sta_tos_pc != 0 && cb_pc() == m816_sta_tos_pc) {
            cb_rewind(3);
            m816_sta_tos_pc = 0;
            emit_inx_n(2);
            emit8_16(M816_CMP_IMM, 0);
        } else {
            emit_inx_n(2);
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE - 2);
        }
        if (is_short)
            return m816_emit_fwd_short(M816_BEQ, bidx);
        nz_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        patch_off = cb_pc();
        cb_emit8_16(M816_BRL, 0);
        cb_patch8(nz_br + 1, (u8)(cb_pc() - (nz_br + 2)));
    } else {
        if (is_short)
            return m816_emit_fwd_short(M816_BRA, bidx);
        patch_off = cb_pc();
        cb_emit8_16(M816_BRL, 0);
    }
    return patch_off;
}

static void m816_emit_br_back(BranchCond cond, size_t target_addr) {
    if (cond == BR_JZ) {
        size_t nz_br;
        i32 rel;
        if (ir4_get_opt_level() && m816_sta_tos_pc != 0 && cb_pc() == m816_sta_tos_pc) {
            cb_rewind(3);
            m816_sta_tos_pc = 0;
            emit_inx_n(2);
            emit8_16(M816_CMP_IMM, 0);
        } else {
            emit_inx_n(2);
            emit8_16(M816_LDA_ABSX, M816_STACK_BASE - 2);
        }
        if (m816_emit_back_short(M816_BEQ, target_addr))
            return;
        nz_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        rel = (i32)target_addr - (i32)(cb_cur_addr() + 3);
        cb_emit8_16(M816_BRL, (u16)(rel & 0xFFFF));
        cb_patch8(nz_br + 1, (u8)(cb_pc() - (nz_br + 2)));
    } else {
        i32 rel;
        if (m816_emit_back_short(M816_BRA, target_addr))
            return;
        rel = (i32)target_addr - (i32)(cb_cur_addr() + 3);
        cb_emit8_16(M816_BRL, (u16)(rel & 0xFFFF));
    }
}

static void m816_patch_fwd(size_t patch_offset) {
    m816_peep_fence();
    if (cb_data()[patch_offset] != M816_BRL) {
        i32 rel = (i32)cb_pc() - (i32)(patch_offset + 2);
        if (rel >= -128 && rel <= 127) {
            cb_patch8(patch_offset + 1, (u8)(i8)rel);
        } else {
            size_t k;
            for (k = 0; k < m816_fwd_list.len; k++) {
                if (m816_fwd_list.data[k].patch_pc == patch_offset) {
                    m816_br_long.data[m816_fwd_list.data[k].br_idx] = 1;
                    break;
                }
            }
            m816_br_need_relax = 1;
        }
        return;
    }
    {
        i32 rel = (i32)cb_pc() - (i32)(patch_offset + 3);
        cb_patch16(patch_offset + 1, (u16)(rel & 0xFFFF));
    }
}

static size_t m816_emit_jtab(size_t count) {
    /* Pop index into A; if u>= count jump to default (patch_offset + 0); else RTL to table[A] */
    size_t def_patch, lda_lo, lda_bk, tbl_addr;
    size_t i;
    emit_pop_w_a();
    emit8_16(M816_CMP_IMM, (u16)count);
    cb_emit8_8(M816_BCC, 3);
    def_patch = cb_pc();
    cb_emit8_16(M816_BRL, 0);
    /* A * 3 -> Y */
    cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
    cb_emit8(M816_ASL_A);
    cb_emit8(M816_CLC);
    cb_emit8_8(M816_ADC_DP, M816_DP_TMP0);
    cb_emit8(M816_TAY);
    /* Read 24-bit entry from PBR:tbl_addr + Y and dispatch via RTL (independent of D & DBR) */
    cb_emit8(M816_PHB);
    cb_emit8(M816_PHK);
    cb_emit8(M816_PLB);
    cb_emit8(0xB9); /* LDA tbl_addr,Y */
    lda_lo = cb_pc();
    cb_emit16(0);
    cb_emit8(M816_DEC_A);
    cb_emit8_8(M816_STA_DP, M816_DP_RET);
    emit_sep_m();
    cb_emit8(0xB9); /* LDA tbl_addr+2,Y */
    lda_bk = cb_pc();
    cb_emit16(0);
    cb_emit8(M816_PLB);
    cb_emit8(M816_PHA);
    emit_rep_m();
    cb_emit8_8(M816_LDA_DP, M816_DP_RET);
    cb_emit8(M816_PHA);
    cb_emit8(M816_RTL);
    tbl_addr = cb_cur_addr();
    cb_patch16(lda_lo, (u16)(tbl_addr & 0xFFFF));
    cb_patch16(lda_bk, (u16)((tbl_addr + 2) & 0xFFFF));
    for (i = 0; i < count; i++)
        cb_emit24(0);
    return def_patch;
}

static void m816_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    if (idx == (size_t)-1) {
        m816_patch_fwd(patch_offset);
    } else {
        /* Table starts 32 bytes after def_patch */
        size_t entry_off = patch_offset + 32 + idx * 3;
        cb_patch24(entry_off, (u32)target_addr);
    }
}

static void m816_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    cb_patch24(call_pc + 1, (u32)target_addr);
}

static size_t m816_push_fn(size_t target_addr) {
    size_t pc;
    emit_dex_n(3);
    cb_emit8(M816_LDA_IMM);
    pc = cb_pc();
    cb_emit16((u16)(target_addr & 0xFFFF));
    emit8_16(M816_STA_ABSX, M816_STACK_BASE);
    emit_sep_m();
    cb_emit8_8(M816_LDA_IMM, (u8)((target_addr >> 16) & 0xFF));
    emit8_16(M816_STA_ABSX, M816_STACK_BASE + 2);
    emit_rep_m();
    return pc;
}

static void m816_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    cb_patch16(patch_pc, (u16)(target_addr & 0xFFFF));
    /* Bank byte is at patch_pc + 8 (imm16=2, STA=3, SEP=2, LDA_IMM_op=1) */
    cb_patch8(patch_pc + 8, (u8)((target_addr >> 16) & 0xFF));
}

static void m816_icall(void) {
    /* Pop 24-bit address into DP_RET; push return addr (next_pc - 1) and dispatch via RTL */
    size_t ret_pc;
    emit_pop_a_dp(M816_DP_RET);
    cb_emit8(M816_PHK);
    cb_emit8(M816_PEA);
    ret_pc = cb_pc();
    cb_emit16(0);
    emit_sep_m();
    cb_emit8_8(M816_LDA_DP, M816_DP_RET + 2);
    cb_emit8(M816_PHA);
    emit_rep_m();
    cb_emit8_8(M816_LDA_DP, M816_DP_RET);
    cb_emit8(M816_DEC_A);
    cb_emit8(M816_PHA);
    cb_emit8(M816_RTL);
    cb_patch16(ret_pc, (u16)((cb_cur_addr() - 1) & 0xFFFF));
}

static size_t m816_stack_data_off(void) { return M816_STACK_BASE; }

const Backend be_65816 = {"65816",
                         2,
                         3,
                         4,
                         m816_cur_addr,
                         m816_fn_begin,
                         m816_fn_end,
                         m816_call,
                         m816_tail_call,
                         m816_call_extern,
                         m816_push_w,
                         m816_push_a,
                         m816_push_f,
                         m816_push_i32,
                         m816_push_data,
                         m816_local_op,
                         m816_local_add_imm,
                         m816_stack_op,
                         m816_op,
                         m816_add_imm,
                         m816_cmp_imm,
                         m816_shift_imm,
                         m816_bit_imm,
                         m816_mem_imm,
                         m816_data_mem,
                         m816_local_mem,
                         m816_local_deref,
                         m816_emit_br_fwd,
                         m816_emit_br_back,
                         m816_cmp_br_fwd,
                         m816_cmp_br_back,
                         m816_patch_fwd,
                         m816_emit_jtab,
                         m816_patch_jtab,
                         m816_patch_call,
                         m816_push_fn,
                         m816_patch_fn,
                         m816_icall,
                         m816_stack_data_off,
                         m816_peep_fence};
