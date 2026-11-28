/* rv32.c - RISC-V RV32IM Binary Backend for IR4th
 * Uniform 32-bit cells (W: and A: are both 4 bytes; floats not implemented in v1).
 * Register & Stack Convention:
 *   - sp (x2):  Hardware call/frame stack (ra, fp, locals)
 *   - fp (x8):  Frame pointer (locals at [fp, #-4], [fp, #-8], ...)
 *   - s1 (x9):  Dedicated Forth data stack pointer (4-byte slots, grows down)
 *   - s2 (x18): Data segment base pointer (set by startup stub)
 *   - t0..t2 (x5..x7): Scratch / operands
 *   - a0..a7 (x10..x17): Extern call arguments & return value
 */
#include "codebuf.h"
#include "backend/reg_lower.h"
#include "backend/rv32.h"
#include "backend/rv32_fp.h"

#define RV_LOAD_BASE 0x80000000UL

#define RV_ZERO 0
#define RV_RA 1
#define RV_SP 2
#define RV_T0 5
#define RV_T1 6
#define RV_T2 7
#define RV_FP 8
#define RV_DSP 9
#define RV_A0 10
#define RV_A1 11
#define RV_DB 18

static size_t rv_fp_addr = 0;
static int rv_obj_mode = 0;

void be_rv32_set_obj(int obj) {
    rv_obj_mode = obj;
    rv_fp_addr = 0;
}

static u32 read32(size_t offset) {
    const u8 *d = cb_data();
    return (u32)d[offset] | ((u32)d[offset + 1] << 8) | ((u32)d[offset + 2] << 16) |
           ((u32)d[offset + 3] << 24);
}

static void ensure_rv32_fp(void) {
    size_t skip_pos;
    size_t base_pc;
    size_t i;
    if (rv_fp_addr != 0)
        return;

    skip_pos = cb_pc();
    cb_emit32(0); /* JAL x0, skip */
    base_pc = cb_pc();
    rv_fp_addr = cb_cur_addr();
    for (i = 0; i < sizeof(be_rv32_fp); i++) {
        cb_emit8(be_rv32_fp[i]);
    }
    for (i = 0; i < (size_t)BE_RV32_FP_RELOC32_CNT; i++) {
        u16 off = be_rv32_fp_relocs32[i * 2];
        u16 tgt = be_rv32_fp_relocs32[i * 2 + 1];
        cb_patch32(base_pc + off, (u32)(RV_LOAD_BASE + rv_fp_addr + tgt));
    }
    for (i = 0; i < (size_t)BE_RV32_FP_RELOC_PAIR_CNT; i++) {
        u16 hi_off = be_rv32_fp_reloc_pairs[i * 3];
        u16 lo_off = be_rv32_fp_reloc_pairs[i * 3 + 1];
        u16 tgt = be_rv32_fp_reloc_pairs[i * 3 + 2];
        u32 addr = (u32)(RV_LOAD_BASE + rv_fp_addr + tgt);
        u32 hi = (addr + 0x800UL) & 0xFFFFF000UL;
        u32 lo = (addr - hi) & 0xFFFUL;
        u32 hi_ins = read32(base_pc + hi_off);
        u32 lo_ins = read32(base_pc + lo_off);
        cb_patch32(base_pc + hi_off, (hi_ins & 0x00000FFFUL) | hi);
        cb_patch32(base_pc + lo_off, (lo_ins & 0x000FFFFFUL) | (lo << 20));
    }
    cb_patch32(skip_pos, ((((u32)((i32)cb_pc() - (i32)skip_pos) >> 20) & 1UL) << 31) |
                          ((((u32)((i32)cb_pc() - (i32)skip_pos) >> 1) & 0x3FFUL) << 21) |
                          ((((u32)((i32)cb_pc() - (i32)skip_pos) >> 11) & 1UL) << 20) |
                          ((((u32)((i32)cb_pc() - (i32)skip_pos) >> 12) & 0xFFUL) << 12) | 0x6FUL);
}

u32 enc_r(u32 f7, u32 rs2, u32 rs1, u32 f3, u32 rd, u32 op) {
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}

u32 enc_i(i32 imm, u32 rs1, u32 f3, u32 rd, u32 op) {
    return (((u32)imm & 0xFFFUL) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}

u32 enc_s(i32 imm, u32 rs2, u32 rs1, u32 f3) {
    u32 u = (u32)imm;
    return (((u >> 5) & 0x7FUL) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) |
           ((u & 0x1FUL) << 7) | 0x23UL;
}

u32 enc_b(i32 imm, u32 rs2, u32 rs1, u32 f3) {
    u32 u = (u32)imm;
    return (((u >> 12) & 1UL) << 31) | (((u >> 5) & 0x3FUL) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (((u >> 1) & 0xFUL) << 8) | (((u >> 11) & 1UL) << 7) | 0x63UL;
}

u32 enc_u(u32 imm20, u32 rd, u32 op) { return (imm20 & 0xFFFFF000UL) | (rd << 7) | op; }

u32 enc_j(i32 imm, u32 rd) {
    u32 u = (u32)imm;
    return (((u >> 20) & 1UL) << 31) | (((u >> 1) & 0x3FFUL) << 21) | (((u >> 11) & 1UL) << 20) |
           (((u >> 12) & 0xFFUL) << 12) | (rd << 7) | 0x6FUL;
}

static void rv_addi(u32 rd, u32 rs1, i32 imm) { cb_emit32(enc_i(imm, rs1, 0, rd, 0x13UL)); }
static void rv_lw(u32 rd, i32 off, u32 rs1) { cb_emit32(enc_i(off, rs1, 2, rd, 0x03UL)); }
static void rv_sw(u32 rs2, i32 off, u32 rs1) { cb_emit32(enc_s(off, rs2, rs1, 2)); }

static void rv_li(u32 rd, u32 val) {
    i32 sval = (i32)val;
    if (sval >= -2048 && sval <= 2047) {
        rv_addi(rd, RV_ZERO, sval);
    } else {
        u32 hi = (val + 0x800UL) & 0xFFFFF000UL;
        i32 lo = (i32)(val - hi);
        cb_emit32(enc_u(hi, rd, 0x37UL)); /* LUI */
        if (lo != 0)
            rv_addi(rd, rd, lo);
    }
}

static void push_reg(u32 reg) {
    rv_addi(RV_DSP, RV_DSP, -4);
    rv_sw(reg, 0, RV_DSP);
}

static void pop_reg(u32 reg) {
    rv_lw(reg, 0, RV_DSP);
    rv_addi(RV_DSP, RV_DSP, 4);
}

/* --- Backend Interface --- */
static size_t rv_cur_addr(void) { return cb_cur_addr(); }

VEC_OF(I32Vec, i32);
static I32Vec rv_local_off;

static void rv_fn_begin(const char *name, int param_cnt, const LocalVar *locals,
                        int total_locals, Bool is_recursive) {
    int i;
    i32 cur_off = 0;
    (void)name;
    (void)is_recursive;

    VEC_ENSURE(rv_local_off, (size_t)total_locals);
    for (i = 0; i < total_locals; i++) {
        i32 sz = (locals[i].size > 0) ? (((i32)locals[i].size + 3) & ~3) : 4;
        cur_off += sz;
        rv_local_off.data[i] = -cur_off;
    }

    rv_addi(RV_SP, RV_SP, -8);
    rv_sw(RV_RA, 4, RV_SP);
    rv_sw(RV_FP, 0, RV_SP);
    rv_addi(RV_FP, RV_SP, 0);

    if (cur_off > 0) {
        if (cur_off <= 2047) {
            rv_addi(RV_SP, RV_SP, -cur_off);
        } else {
            rv_li(RV_T0, (u32)(-cur_off));
            cb_emit32(enc_r(0x00, RV_T0, RV_SP, 0, RV_SP, 0x33UL)); /* ADD sp, sp, t0 */
        }
    }

    /* Pop incoming arguments from data stack (arg param_cnt-1 is at 0(s1)) */
    if (param_cnt > 0) {
        for (i = param_cnt; i > 0; i--) {
            size_t slot = i - 1;
            pop_reg(RV_T0);
            rv_sw(RV_T0, rv_local_off.data[slot], RV_FP);
        }
    }

    for (i = param_cnt; i < total_locals; i++) {
        if (locals[i].size == 0) {
            rv_sw(RV_ZERO, rv_local_off.data[i], RV_FP);
        }
    }
}

static void rv_fn_end(const FnSig *sig, const LocalVar *locals, int total_locals) {
    (void)sig;
    (void)locals;
    (void)total_locals;
    /* Return values are already on the dedicated data stack (s1) */
    rv_addi(RV_SP, RV_FP, 0);
    rv_lw(RV_FP, 0, RV_SP);
    rv_lw(RV_RA, 4, RV_SP);
    rv_addi(RV_SP, RV_SP, 8);
    cb_emit32(enc_i(0, RV_RA, 0, RV_ZERO, 0x67UL)); /* JALR x0, 0(ra) */
}

static void rv_call(size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    cb_emit32(enc_j(diff, RV_RA));
}

static int rv_tail_call(size_t target_addr, const FnSig *sig, const FnSig *callee,
                        const LocalVar *locals, int total_locals) {
    i32 diff;
    (void)sig;
    (void)callee;
    (void)locals;
    (void)total_locals;
    rv_addi(RV_SP, RV_FP, 0);
    rv_lw(RV_FP, 0, RV_SP);
    rv_lw(RV_RA, 4, RV_SP);
    rv_addi(RV_SP, RV_SP, 8);
    diff = (i32)target_addr - (i32)cb_cur_addr();
    cb_emit32(enc_j(diff, RV_ZERO));
    return 1;
}

static void rv_emit_extern_call(const char *name) {
    cb_add_extern_reloc(cb_pc(), name);
    cb_emit32(enc_u(0, RV_T0, 0x37UL));           /* LUI t0, 0 */
    rv_addi(RV_T0, RV_T0, 0);                  /* ADDI t0, t0, 0 */
    cb_emit32(enc_i(0, RV_T0, 0, RV_RA, 0x67UL)); /* JALR ra, 0(t0) */
}

static void rv_call_extern(const FnSig *sig) {
    size_t i;
    if (sig->in_cnt > 8 || sig->out_cnt > 1) {
        ir4_die("RV32 extern supports at most 8 args and 1 return");
    }

    for (i = sig->in_cnt; i > 0; i--) {
        pop_reg(RV_A0 + (u32)(i - 1));
    }

    if (FN_FLAG(sig, FN_FL_HAS_ADDR)) {
        rv_li(RV_T0, (u32)sig->addr);
        cb_emit32(enc_i(0, RV_T0, 0, RV_RA, 0x67UL)); /* JALR ra, 0(t0) */
    } else {
        rv_emit_extern_call(sig->name);
    }

    if (sig->out_cnt == 1) {
        push_reg(RV_A0);
    }
}

static void rv_push_w(i32 val) {
    rv_li(RV_T0, (u32)val);
    push_reg(RV_T0);
}
static void rv_push_a(u32 val) {
    rv_li(RV_T0, val);
    push_reg(RV_T0);
}
static void rv_push_f(double val) {
    float f = (float)val;
    u32 bits;
    memcpy(&bits, &f, 4);
    rv_li(RV_T0, bits);
    push_reg(RV_T0);
}

static void rv_push_data(size_t offset) {
    if (offset <= 2047) {
        rv_addi(RV_T0, RV_DB, (i32)offset);
    } else {
        rv_li(RV_T0, (u32)offset);
        cb_emit32(enc_r(0x00, RV_T0, RV_DB, 0, RV_T0, 0x33UL)); /* ADD t0, s2, t0 */
    }
    push_reg(RV_T0);
}

static void rv_local_op(LocalOp op, int slot, StackType type, size_t stack_bytes) {
    i32 off = rv_local_off.data[slot];
    (void)type;
    (void)stack_bytes;
    if (op == LOCAL_GET) {
        rv_lw(RV_T0, off, RV_FP);
        push_reg(RV_T0);
    } else if (op == LOCAL_ADDR) {
        if (off >= -2048 && off <= 2047) {
            rv_addi(RV_T0, RV_FP, off);
        } else {
            rv_li(RV_T0, (u32)off);
            cb_emit32(enc_r(0x00, RV_T0, RV_FP, 0, RV_T0, 0x33UL)); /* ADD t0, fp, t0 */
        }
        push_reg(RV_T0);
    } else if (op == LOCAL_ADD) {
        pop_reg(RV_T0);
        rv_lw(RV_T1, off, RV_FP);
        cb_emit32(enc_r(0x00, RV_T0, RV_T1, 0, RV_T0, 0x33UL)); /* ADD t0, t1, t0 */
        rv_sw(RV_T0, off, RV_FP);
    } else {
        pop_reg(RV_T0);
        rv_sw(RV_T0, off, RV_FP);
    }
}

static void rv_stack_op(StackOp op, StackType t1, StackType t2) {
    (void)t1;
    (void)t2;
    switch (op) {
    case OP_DUP:
        rv_lw(RV_T0, 0, RV_DSP);
        push_reg(RV_T0);
        break;
    case OP_DROP:
        rv_addi(RV_DSP, RV_DSP, 4);
        break;
    case OP_SWAP:
        rv_lw(RV_T0, 0, RV_DSP);
        rv_lw(RV_T1, 4, RV_DSP);
        rv_sw(RV_T1, 0, RV_DSP);
        rv_sw(RV_T0, 4, RV_DSP);
        break;
    default:
        ir4_die("unimplemented RV32 stack op");
    }
}

static void rv_int_alu(IrOp op) {
    pop_reg(RV_T1); /* rhs */
    pop_reg(RV_T0); /* lhs */

    switch (op) {
    case OP_ADD:
    case OP_AADD:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 0, RV_T0, 0x33UL));
        break; /* ADD */
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit32(enc_r(0x20, RV_T1, RV_T0, 0, RV_T0, 0x33UL));
        break; /* SUB */
    case OP_MUL:
        cb_emit32(enc_r(0x01, RV_T1, RV_T0, 0, RV_T0, 0x33UL));
        break; /* MUL */
    case OP_DIV:
        cb_emit32(enc_r(0x01, RV_T1, RV_T0, 4, RV_T0, 0x33UL));
        break; /* DIV */
    case OP_UDIV:
        cb_emit32(enc_r(0x01, RV_T1, RV_T0, 5, RV_T0, 0x33UL));
        break; /* DIVU */
    case OP_MOD:
        cb_emit32(enc_r(0x01, RV_T1, RV_T0, 6, RV_T0, 0x33UL));
        break; /* REM */
    case OP_UMOD:
        cb_emit32(enc_r(0x01, RV_T1, RV_T0, 7, RV_T0, 0x33UL));
        break; /* REMU */
    case OP_AND:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 7, RV_T0, 0x33UL));
        break; /* AND */
    case OP_OR:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 6, RV_T0, 0x33UL));
        break; /* OR */
    case OP_XOR:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 4, RV_T0, 0x33UL));
        break; /* XOR */
    case OP_LSHIFT:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 1, RV_T0, 0x33UL));
        break; /* SLL */
    case OP_RSHIFT:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 5, RV_T0, 0x33UL));
        break; /* SRL */
    case OP_ARSHIFT:
        cb_emit32(enc_r(0x20, RV_T1, RV_T0, 5, RV_T0, 0x33UL));
        break; /* SRA */
    default:
        ir4_die("unimplemented RV32 ALU op");
    }

    push_reg(RV_T0);
}

static void rv_int_cmp(IrOp op) {
    pop_reg(RV_T1); /* rhs */
    pop_reg(RV_T0); /* lhs */

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 4, RV_T0, 0x33UL)); /* XOR */
        cb_emit32(enc_i(1, RV_T0, 3, RV_T0, 0x13UL));           /* SLTIU t0, t0, 1 */
        break;
    case OP_NE:
    case OP_ANE:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 4, RV_T0, 0x33UL));   /* XOR */
        cb_emit32(enc_r(0x00, RV_T0, RV_ZERO, 3, RV_T0, 0x33UL)); /* SLTU t0, x0, t0 */
        break;
    case OP_LT:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 2, RV_T0, 0x33UL)); /* SLT t0, t0, t1 */
        break;
    case OP_GT:
        cb_emit32(enc_r(0x00, RV_T0, RV_T1, 2, RV_T0, 0x33UL)); /* SLT t0, t1, t0 */
        break;
    case OP_LE:
        cb_emit32(enc_r(0x00, RV_T0, RV_T1, 2, RV_T0, 0x33UL)); /* SLT t0, t1, t0 */
        cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL));           /* XORI t0, t0, 1 */
        break;
    case OP_GE:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 2, RV_T0, 0x33UL)); /* SLT t0, t0, t1 */
        cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL));           /* XORI t0, t0, 1 */
        break;
    case OP_ULT:
    case OP_ALT:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 3, RV_T0, 0x33UL)); /* SLTU t0, t0, t1 */
        break;
    case OP_UGT:
    case OP_AGT:
        cb_emit32(enc_r(0x00, RV_T0, RV_T1, 3, RV_T0, 0x33UL)); /* SLTU t0, t1, t0 */
        break;
    case OP_ULE:
        cb_emit32(enc_r(0x00, RV_T0, RV_T1, 3, RV_T0, 0x33UL)); /* SLTU t0, t1, t0 */
        cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL));           /* XORI t0, t0, 1 */
        break;
    case OP_UGE:
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 3, RV_T0, 0x33UL)); /* SLTU t0, t0, t1 */
        cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL));           /* XORI t0, t0, 1 */
        break;
    default:
        ir4_die("unimplemented RV32 cmp op");
    }

    /* Convert 0/1 to Forth 0/-1 */
    cb_emit32(enc_r(0x20, RV_T0, RV_ZERO, 0, RV_T0, 0x33UL)); /* SUB t0, x0, t0 */
    push_reg(RV_T0);
}

static int rv_bit_imm(IrOp op, i32 imm) {
    u32 f3;
    if (imm < -2048 || imm > 2047)
        return 0;
    if (op == OP_AND)
        f3 = 7;
    else if (op == OP_OR)
        f3 = 6;
    else if (op == OP_XOR)
        f3 = 4;
    else
        return 0;
    rv_lw(RV_T0, 0, RV_DSP);
    cb_emit32(enc_i(imm, RV_T0, f3, RV_T0, 0x13UL)); /* ANDI/ORI/XORI t0, t0, imm */
    rv_sw(RV_T0, 0, RV_DSP);
    return 1;
}

static int rv_mem_imm(IrOp op, i32 off) {
    u32 funct3;
    int is_store = 0;
    int swapped = 0;

    if (off < -2048 || off > 2047)
        return 0;
    switch (op) {
    case OP_I8FETCH:
        funct3 = 0;
        break;
    case OP_U8FETCH:
        funct3 = 4;
        break;
    case OP_SI8STORE:
        swapped = 1; /* fall through */
    case OP_I8STORE:
        funct3 = 0;
        is_store = 1;
        break;
    case OP_I16FETCH:
        funct3 = 1;
        break;
    case OP_U16FETCH:
        funct3 = 5;
        break;
    case OP_SI16STORE:
        swapped = 1; /* fall through */
    case OP_I16STORE:
        funct3 = 1;
        is_store = 1;
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_FFETCH:
    case OP_I32FETCH:
        funct3 = 2;
        break;
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SFSTORE:
        swapped = 1; /* fall through */
    case OP_STORE:
    case OP_ASTORE:
    case OP_FSTORE:
    case OP_I32STORE:
        funct3 = 2;
        is_store = 1;
        break;
    default:
        return 0;
    }

    if (is_store) {
        if (swapped) {
            pop_reg(RV_T1);
            pop_reg(RV_T0);
        } else {
            pop_reg(RV_T0);
            pop_reg(RV_T1);
        }
        cb_emit32(enc_s(off, RV_T1, RV_T0, funct3));
    } else {
        pop_reg(RV_T0);
        cb_emit32(enc_i(off, RV_T0, funct3, RV_T0, 0x03UL));
        push_reg(RV_T0);
    }
    return 1;
}

static IrOp rv_norm32(IrOp op) {
    switch (op) {
    case OP_ADD32:
        return OP_ADD;
    case OP_SUB32:
        return OP_SUB;
    case OP_MUL32:
        return OP_MUL;
    case OP_DIV32:
        return OP_DIV;
    case OP_MOD32:
        return OP_MOD;
    case OP_UDIV32:
        return OP_UDIV;
    case OP_UMOD32:
        return OP_UMOD;
    case OP_AND32:
        return OP_AND;
    case OP_OR32:
        return OP_OR;
    case OP_XOR32:
        return OP_XOR;
    case OP_SHL32:
        return OP_LSHIFT;
    case OP_ASHR32:
        return OP_ARSHIFT;
    case OP_RSHR32:
        return OP_RSHIFT;
    case OP_EQ32:
        return OP_EQ;
    case OP_NE32:
        return OP_NE;
    case OP_LT32:
        return OP_LT;
    case OP_GT32:
        return OP_GT;
    case OP_LE32:
        return OP_LE;
    case OP_GE32:
        return OP_GE;
    case OP_ULT32:
        return OP_ULT;
    case OP_UGT32:
        return OP_UGT;
    case OP_ULE32:
        return OP_ULE;
    case OP_UGE32:
        return OP_UGE;
    default:
        return op;
    }
}

static void rv_op(IrOp op) {
    op = rv_norm32(op);
    switch (op) {
    case OP_ADD:
    case OP_AADD:
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
    case OP_MUL:
    case OP_DIV:
    case OP_UDIV:
    case OP_MOD:
    case OP_UMOD:
    case OP_AND:
    case OP_OR:
    case OP_XOR:
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        rv_int_alu(op);
        break;
    case OP_INVERT:
    case OP_INV32:
        pop_reg(RV_T0);
        cb_emit32(enc_i(-1, RV_T0, 4, RV_T0, 0x13UL)); /* XORI t0, t0, -1 */
        push_reg(RV_T0);
        break;
    case OP_EQ:
    case OP_AEQ:
    case OP_NE:
    case OP_ANE:
    case OP_LT:
    case OP_GE:
    case OP_GT:
    case OP_LE:
    case OP_ULT:
    case OP_ALT:
    case OP_UGE:
    case OP_UGT:
    case OP_AGT:
    case OP_ULE:
        rv_int_cmp(op);
        break;
    case OP_I8FETCH:
    case OP_U8FETCH:
    case OP_I8STORE:
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_I16STORE:
    case OP_FETCH:
    case OP_AFETCH:
    case OP_FFETCH:
    case OP_STORE:
    case OP_ASTORE:
    case OP_FSTORE:
    case OP_SI8STORE:
    case OP_SI16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SFSTORE:
    case OP_I32FETCH:
    case OP_I32STORE:
        rv_mem_imm(op, 0);
        break;
    case OP_BCOPY:
        pop_reg(RV_T2);                             /* len */
        pop_reg(RV_T1);                             /* src */
        pop_reg(RV_T0);                             /* dst */
        cb_emit32(enc_b(24, RV_ZERO, RV_T2, 0));       /* BEQ t2, x0, +24 */
        cb_emit32(enc_i(0, RV_T1, 4, RV_A0, 0x03UL));  /* LBU a0, 0(t1) */
        cb_emit32(enc_s(0, RV_A0, RV_T0, 0));          /* SB a0, 0(t0) */
        cb_emit32(enc_i(1, RV_T1, 0, RV_T1, 0x13UL));  /* ADDI t1, t1, 1 */
        cb_emit32(enc_i(1, RV_T0, 0, RV_T0, 0x13UL));  /* ADDI t0, t0, 1 */
        cb_emit32(enc_i(-1, RV_T2, 0, RV_T2, 0x13UL)); /* ADDI t2, t2, -1 */
        cb_emit32(enc_b(-20, RV_ZERO, RV_T2, 1));      /* BNE t2, x0, -20 */
        break;
    case OP_FNEG:
        pop_reg(RV_T0);
        cb_emit32(enc_u(0x80000000UL, RV_T1, 0x37UL));          /* LUI t1, 0x80000 */
        cb_emit32(enc_r(0x00, RV_T1, RV_T0, 4, RV_T0, 0x33UL)); /* XOR t0, t0, t1 */
        push_reg(RV_T0);
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV: {
        pop_reg(RV_A1);
        pop_reg(RV_A0);
        if (rv_obj_mode) {
            const char *fn = (op == OP_FADD)   ? "__addsf3"
                             : (op == OP_FSUB) ? "__subsf3"
                             : (op == OP_FMUL) ? "__mulsf3"
                                               : "__divsf3";
            rv_emit_extern_call(fn);
        } else {
            u16 entry = (op == OP_FADD)   ? RT_FP_ADD
                        : (op == OP_FSUB) ? RT_FP_SUB
                        : (op == OP_FMUL) ? RT_FP_MUL
                                          : RT_FP_DIV;
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + entry) - (i32)cb_cur_addr(), RV_RA));
        }
        push_reg(RV_A0);
        break;
    }
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT:
        pop_reg(RV_A1);
        pop_reg(RV_A0);
        if (rv_obj_mode) {
            const char *fn = (op == OP_FEQ)   ? "__eqsf2"
                             : (op == OP_FNE) ? "__nesf2"
                             : (op == OP_FLT) ? "__ltsf2"
                                              : "__gtsf2";
            rv_emit_extern_call(fn);
        } else {
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + RT_FP_CMP) - (i32)cb_cur_addr(), RV_RA));
        }
        if (op == OP_FEQ) {
            cb_emit32(enc_i(1, RV_A0, 3, RV_T0, 0x13UL)); /* SLTIU t0, a0, 1 */
        } else if (op == OP_FNE) {
            cb_emit32(enc_r(0x00, RV_A0, RV_ZERO, 3, RV_T0, 0x33UL)); /* SLTU t0, x0, a0 */
        } else if (op == OP_FLT) {
            cb_emit32(enc_r(0x00, RV_ZERO, RV_A0, 2, RV_T0, 0x33UL)); /* SLT t0, a0, x0 */
        } else {
            cb_emit32(enc_r(0x00, RV_A0, RV_ZERO, 2, RV_T0, 0x33UL)); /* SLT t0, x0, a0 */
        }
        cb_emit32(enc_r(0x20, RV_T0, RV_ZERO, 0, RV_T0, 0x33UL)); /* SUB t0, x0, t0 */
        push_reg(RV_T0);
        break;
    case OP_W2F:
        pop_reg(RV_A0);
        if (rv_obj_mode) {
            rv_emit_extern_call("__floatsisf");
        } else {
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + RT_FP_ITOF) - (i32)cb_cur_addr(), RV_RA));
        }
        push_reg(RV_A0);
        break;
    case OP_F2W:
        pop_reg(RV_A0);
        if (rv_obj_mode) {
            rv_emit_extern_call("__fixsfsi");
        } else {
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + RT_FP_FTOI) - (i32)cb_cur_addr(), RV_RA));
        }
        push_reg(RV_A0);
        break;
    case OP_SEXT:
        pop_reg(RV_T0);
        cb_emit32(enc_i(24, RV_T0, 1, RV_T0, 0x13UL));         /* SLLI t0, t0, 24 */
        cb_emit32(enc_i(0x400 | 24, RV_T0, 5, RV_T0, 0x13UL)); /* SRAI t0, t0, 24 */
        push_reg(RV_T0);
        break;
    case OP_W2A:
    case OP_A2W:
    case OP_W2I32:
    case OP_U2I32:
    case OP_I322W:
        /* Uniform 32-bit cells: zero-cost no-op */
        break;
    default:
        ir4_die("unimplemented or float RV32 op");
    }
}

static size_t rv_emit_br_fwd(BranchCond cond) {
    size_t pos;
    if (cond == BR_JZ) {
        pop_reg(RV_T0);
        cb_emit32(enc_b(8, RV_ZERO, RV_T0, 1)); /* BNE t0, x0, +8 */
    }
    pos = cb_pc();
    cb_emit32(enc_j(0, RV_ZERO)); /* JAL x0, rel21 */
    return pos;
}

static void rv_emit_br_back(BranchCond cond, size_t target_addr) {
    i32 diff;
    if (cond == BR_JZ) {
        pop_reg(RV_T0);
        diff = (i32)target_addr - (i32)cb_cur_addr();
        if (diff >= -4096 && diff <= 4094) {
            cb_emit32(enc_b(diff, RV_ZERO, RV_T0, 0));
        } else {
            cb_emit32(enc_b(8, RV_ZERO, RV_T0, 1)); /* BNE t0, x0, +8 */
            diff = (i32)target_addr - (i32)cb_cur_addr();
            cb_emit32(enc_j(diff, RV_ZERO));
        }
    } else {
        diff = (i32)target_addr - (i32)cb_cur_addr();
        cb_emit32(enc_j(diff, RV_ZERO));
    }
}

static int rv_cmp_br_emit(IrOp op, int has_imm, i32 imm, int is_fwd, size_t target_addr,
                          size_t *patch_offset) {
    u32 rs1 = RV_T0;
    u32 rs2 = (has_imm && imm == 0) ? RV_ZERO : RV_T1;
    u32 f3_true, f3_false;
    int swap_ops = 0;

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        f3_true = 0;
        f3_false = 1;
        break;
    case OP_NE:
    case OP_ANE:
        f3_true = 1;
        f3_false = 0;
        break;
    case OP_LT:
        f3_true = 4;
        f3_false = 5;
        break;
    case OP_GE:
        f3_true = 5;
        f3_false = 4;
        break;
    case OP_GT:
        swap_ops = 1;
        f3_true = 4;
        f3_false = 5;
        break;
    case OP_LE:
        swap_ops = 1;
        f3_true = 5;
        f3_false = 4;
        break;
    case OP_ULT:
    case OP_ALT:
        f3_true = 6;
        f3_false = 7;
        break;
    case OP_UGE:
        f3_true = 7;
        f3_false = 6;
        break;
    case OP_UGT:
    case OP_AGT:
        swap_ops = 1;
        f3_true = 6;
        f3_false = 7;
        break;
    case OP_ULE:
        swap_ops = 1;
        f3_true = 7;
        f3_false = 6;
        break;
    default:
        return 0;
    }

    if (has_imm) {
        if (imm != 0)
            rv_li(RV_T1, (u32)imm);
        pop_reg(RV_T0);
    } else {
        pop_reg(RV_T1);
        pop_reg(RV_T0);
    }
    if (swap_ops) {
        u32 tmp = rs1;
        rs1 = rs2;
        rs2 = tmp;
    }
    if (is_fwd) {
        cb_emit32(enc_b(8, rs2, rs1, f3_true));
        *patch_offset = cb_pc();
        cb_emit32(enc_j(0, RV_ZERO));
    } else {
        i32 diff = (i32)target_addr - (i32)cb_cur_addr();
        if (diff >= -4096 && diff <= 4094) {
            cb_emit32(enc_b(diff, rs2, rs1, f3_false));
        } else {
            cb_emit32(enc_b(8, rs2, rs1, f3_true));
            diff = (i32)target_addr - (i32)cb_cur_addr();
            cb_emit32(enc_j(diff, RV_ZERO));
        }
    }
    return 1;
}

static int rv_cmp_br_fwd(IrOp op, int has_imm, i32 imm, size_t *patch_offset) {
    return rv_cmp_br_emit(op, has_imm, imm, 1, 0, patch_offset);
}

static int rv_cmp_br_back(IrOp op, int has_imm, i32 imm, size_t target_addr) {
    return rv_cmp_br_emit(op, has_imm, imm, 0, target_addr, NULL);
}

static size_t rv_emit_jtab(size_t count) {
    size_t tbl_off, k;
    pop_reg(RV_T0);
    cb_emit32(enc_i(2, RV_T0, 1, RV_T0, 0x13UL));          /* SLLI t0, t0, 2 */
    cb_emit32(0x00000317UL);                                /* AUIPC t1, 0 */
    cb_emit32(enc_i(24, RV_T1, 0, RV_T1, 0x13UL));          /* ADDI t1, t1, 24 */
    cb_emit32(enc_r(0x00, RV_T0, RV_T1, 0, RV_T2, 0x33UL)); /* ADD t2, t1, t0 */
    rv_lw(RV_T0, 0, RV_T2);                              /* LW t0, 0(t2) */
    cb_emit32(enc_r(0x00, RV_T0, RV_T1, 0, RV_T0, 0x33UL)); /* ADD t0, t1, t0 */
    cb_emit32(enc_i(0, RV_T0, 0, RV_ZERO, 0x67UL));         /* JALR x0, 0(t0) */
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit32(0);
    return tbl_off;
}

static void rv_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    cb_patch32(patch_offset + idx * 4, (u32)((i32)target_addr - (i32)patch_offset));
}

static void rv_patch_fwd(size_t patch_offset) {
    i32 diff = (i32)cb_pc() - (i32)patch_offset;
    cb_patch32(patch_offset, enc_j(diff, RV_ZERO));
}

static int rv_add_imm(int is_addr, i32 imm) {
    (void)is_addr;
    if (imm == 0)
        return 1;
    if (imm < -2048 || imm > 2047)
        return 0;
    rv_lw(RV_T0, 0, RV_DSP);
    rv_addi(RV_T0, RV_T0, imm);
    rv_sw(RV_T0, 0, RV_DSP);
    return 1;
}

static int rv_cmp_imm(IrOp op, i32 imm) {
    if (imm < -2048 || imm > 2047)
        return 0;
    if ((op == OP_ULT || op == OP_UGE) && imm < 0)
        return 0;

    switch (op) {
    case OP_EQ:
    case OP_NE:
        if (imm == -2048)
            return 0;
        rv_lw(RV_T0, 0, RV_DSP);
        if (imm != 0)
            rv_addi(RV_T0, RV_T0, -imm);
        if (op == OP_EQ) {
            cb_emit32(enc_i(1, RV_T0, 3, RV_T0, 0x13UL)); /* SLTIU t0, t0, 1 */
        } else {
            cb_emit32(enc_r(0x00, RV_T0, RV_ZERO, 3, RV_T0, 0x33UL)); /* SLTU t0, x0, t0 */
        }
        break;
    case OP_LT:
    case OP_GE:
        rv_lw(RV_T0, 0, RV_DSP);
        cb_emit32(enc_i(imm, RV_T0, 2, RV_T0, 0x13UL)); /* SLTI t0, t0, imm */
        if (op == OP_GE)
            cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL)); /* XORI t0, t0, 1 */
        break;
    case OP_ULT:
    case OP_UGE:
        rv_lw(RV_T0, 0, RV_DSP);
        cb_emit32(enc_i(imm, RV_T0, 3, RV_T0, 0x13UL)); /* SLTIU t0, t0, imm */
        if (op == OP_UGE)
            cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL)); /* XORI t0, t0, 1 */
        break;
    case OP_LE:
    case OP_GT:
        if (imm == 2047)
            return 0;
        rv_lw(RV_T0, 0, RV_DSP);
        cb_emit32(enc_i(imm + 1, RV_T0, 2, RV_T0, 0x13UL)); /* SLTI t0, t0, imm+1 */
        if (op == OP_GT)
            cb_emit32(enc_i(1, RV_T0, 4, RV_T0, 0x13UL)); /* XORI t0, t0, 1 */
        break;
    default:
        return 0;
    }
    cb_emit32(enc_r(0x20, RV_T0, RV_ZERO, 0, RV_T0, 0x33UL)); /* SUB t0, x0, t0 */
    rv_sw(RV_T0, 0, RV_DSP);
    return 1;
}

static int rv_shift_imm(IrOp op, u8 count) {
    u32 c = (u32)(count & 31);
    if (c == 0)
        return 1;
    op = rv_norm32(op);
    rv_lw(RV_T0, 0, RV_DSP);
    switch (op) {
    case OP_LSHIFT:
        cb_emit32(enc_i(c, RV_T0, 1, RV_T0, 0x13UL));
        break; /* SLLI */
    case OP_RSHIFT:
        cb_emit32(enc_i(c, RV_T0, 5, RV_T0, 0x13UL));
        break; /* SRLI */
    case OP_ARSHIFT:
        cb_emit32(enc_i(0x400 | c, RV_T0, 5, RV_T0, 0x13UL));
        break; /* SRAI */
    default:
        return 0;
    }
    rv_sw(RV_T0, 0, RV_DSP);
    return 1;
}

static void rv_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)call_addr;
    cb_patch32(call_pc, enc_j(diff, RV_RA));
}

static size_t rv_push_fn(size_t target_addr) {
    size_t pc = cb_pc();
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    u32 hi = (u32)((diff + 0x800) & 0xFFFFF000L);
    i32 lo = diff - (i32)hi;
    cb_emit32(enc_u(hi, RV_T0, 0x17UL));           /* AUIPC t0, hi */
    cb_emit32(enc_i(lo, RV_T0, 0, RV_T0, 0x13UL)); /* ADDI t0, t0, lo */
    push_reg(RV_T0);
    return pc;
}

static void rv_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)patch_addr;
    u32 hi = (u32)((diff + 0x800) & 0xFFFFF000L);
    i32 lo = diff - (i32)hi;
    u32 rd = (read32(patch_pc) >> 7) & 0x1FUL;
    cb_patch32(patch_pc, enc_u(hi, rd, 0x17UL));
    cb_patch32(patch_pc + 4, enc_i(lo, rd, 0, rd, 0x13UL));
}

static void rv_icall(void) {
    pop_reg(RV_T0);
    cb_emit32(enc_i(0, RV_T0, 0, RV_RA, 0x67UL)); /* JALR ra, t0, 0 */
}

const Backend be_rv32 = {"rv32",
                         4,
                         4,
                         4,
                         rv_cur_addr,
                         rv_fn_begin,
                         rv_fn_end,
                         rv_call,
                         rv_tail_call,
                         rv_call_extern,
                         rv_push_w,
                         rv_push_a,
                         rv_push_f,
                         rv_push_w,
                         rv_push_data,
                         rv_local_op,
                         NULL,
                         rv_stack_op,
                         rv_op,
                         rv_add_imm,
                         rv_cmp_imm,
                         rv_shift_imm,
                         rv_bit_imm,
                         rv_mem_imm,
                         NULL,
                         NULL,
                         NULL,
                         rv_emit_br_fwd,
                         rv_emit_br_back,
                         rv_cmp_br_fwd,
                         rv_cmp_br_back,
                         rv_patch_fwd,
                         rv_emit_jtab,
                         rv_patch_jtab,
                         rv_patch_call,
                         rv_push_fn,
                         rv_patch_fn,
                         rv_icall,
                         NULL,
                         NULL};

/* --- Direct Register-Stack Backend (reg_be_rv32) --- */
static const u8 rvr_stk_regs[12] = {10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31};
static const u8 rvr_loc_regs[9] = {19, 20, 21, 22, 23, 24, 25, 26, 27};

static void rvr_mov_reg(u8 dst, u8 src, StackType type) {
    (void)type;
    if (dst != src)
        rv_addi(dst, src, 0);
}

static void rvr_mov_imm(u8 dst, i32 val) { rv_li(dst, (u32)val); }
static void rvr_mov_uimm32(u8 dst, u32 val) { rv_li(dst, val); }

static void rvr_mov_fconst(u8 dst, double val) {
    float f = (float)val;
    u32 bits;
    memcpy(&bits, &f, 4);
    rv_li(dst, bits);
}

static void rvr_spill_store(u8 reg, StackType type, i32 frame_off) {
    i32 off = -frame_off;
    (void)type;
    if (off >= -2048 && off <= 2047) {
        rv_sw(reg, off, RV_FP);
    } else {
        u32 tmp = (reg != RV_T2) ? RV_T2 : RV_T1;
        rv_li(tmp, (u32)off);
        cb_emit32(enc_r(0x00, tmp, RV_FP, 0, tmp, 0x33UL));
        rv_sw(reg, 0, tmp);
    }
}

static void rvr_spill_load(u8 reg, StackType type, i32 frame_off) {
    i32 off = -frame_off;
    (void)type;
    if (off >= -2048 && off <= 2047) {
        rv_lw(reg, off, RV_FP);
    } else {
        u32 tmp = (reg != RV_T2) ? RV_T2 : RV_T1;
        rv_li(tmp, (u32)off);
        cb_emit32(enc_r(0x00, tmp, RV_FP, 0, tmp, 0x33UL));
        rv_lw(reg, 0, tmp);
    }
}

static void rvr_local_addr(u8 dst, i32 frame_off) {
    i32 off = -frame_off;
    if (off >= -2048 && off <= 2047) {
        rv_addi(dst, RV_FP, off);
    } else {
        rv_li(dst, (u32)off);
        cb_emit32(enc_r(0x00, dst, RV_FP, 0, dst, 0x33UL));
    }
}

static void rvr_load_data_addr(u8 dst, size_t offset) {
    if (offset <= 2047) {
        rv_addi(dst, RV_DB, (i32)offset);
    } else {
        rv_li(dst, (u32)offset);
        cb_emit32(enc_r(0x00, dst, RV_DB, 0, dst, 0x33UL));
    }
}

static size_t rvr_load_fn_addr(u8 dst, size_t target_addr) {
    size_t pc = cb_pc();
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    u32 hi = (u32)((diff + 0x800) & 0xFFFFF000L);
    i32 lo = diff - (i32)hi;
    cb_emit32(enc_u(hi, dst, 0x17UL));
    cb_emit32(enc_i(lo, dst, 0, dst, 0x13UL));
    return pc;
}

static void rvr_sp_adj(i32 delta) {
    if (delta == 0)
        return;
    if (delta >= -2048 && delta <= 2047) {
        rv_addi(RV_SP, RV_SP, delta);
    } else {
        rv_li(RV_T2, (u32)delta);
        cb_emit32(enc_r(0x00, RV_T2, RV_SP, 0, RV_SP, 0x33UL));
    }
}

static void rvr_store_out_arg(u8 src_reg, StackType type, u32 sp_off) {
    (void)type;
    if (sp_off <= 2047) {
        rv_sw(src_reg, (i32)sp_off, RV_SP);
    } else {
        u32 tmp = (src_reg != RV_T2) ? RV_T2 : RV_T1;
        rv_li(tmp, sp_off);
        cb_emit32(enc_r(0x00, tmp, RV_SP, 0, tmp, 0x33UL));
        rv_sw(src_reg, 0, tmp);
    }
}

static void rvr_fn_begin(const char *name, u8 used_loc_regs, u32 frame_bytes, size_t param_cnt,
                         const LocalVar *locals, const u8 *loc_is_reg, const u8 *loc_reg,
                         const i32 *loc_off, size_t total_locals) {
    u32 save_bytes = (u32)used_loc_regs * 4U;
    u32 total_frame = frame_bytes + save_bytes;
    size_t i;
    (void)name;

    rv_addi(RV_SP, RV_SP, -8);
    rv_sw(RV_RA, 4, RV_SP);
    rv_sw(RV_FP, 0, RV_SP);
    rv_addi(RV_FP, RV_SP, 0);

    if (total_frame > 0) {
        if (total_frame <= 2047) {
            rv_addi(RV_SP, RV_SP, -(i32)total_frame);
        } else {
            rv_li(RV_T0, (u32)(-(i32)total_frame));
            cb_emit32(enc_r(0x00, RV_T0, RV_SP, 0, RV_SP, 0x33UL));
        }
    }

    for (i = 0; i < used_loc_regs; i++) {
        rvr_spill_store(rvr_loc_regs[i], TY_W, (i32)(frame_bytes + (i + 1) * 4));
    }

    for (i = 0; i < param_cnt; i++) {
        if (i < 12) {
            u8 src = rvr_stk_regs[i];
            if (loc_is_reg[i])
                rvr_mov_reg(loc_reg[i], src, locals[i].type);
            else
                rvr_spill_store(src, locals[i].type, loc_off[i]);
        } else {
            i32 caller_frame_off = -(i32)(8 + (i - 12) * 4);
            if (loc_is_reg[i]) {
                rvr_spill_load(loc_reg[i], locals[i].type, caller_frame_off);
            } else {
                rvr_spill_load(RV_T0, locals[i].type, caller_frame_off);
                rvr_spill_store(RV_T0, locals[i].type, loc_off[i]);
            }
        }
    }

    for (i = param_cnt; i < total_locals; i++) {
        if (locals[i].size > 0)
            continue;
        if (loc_is_reg[i])
            rv_addi(loc_reg[i], RV_ZERO, 0);
        else
            rvr_spill_store(RV_ZERO, locals[i].type, loc_off[i]);
    }
}

static void rvr_fn_end(u8 used_loc_regs, u32 frame_bytes) {
    u8 i;
    for (i = 0; i < used_loc_regs; i++) {
        rvr_spill_load(rvr_loc_regs[i], TY_W, (i32)(frame_bytes + (i + 1) * 4));
    }
    rv_addi(RV_SP, RV_FP, 0);
    rv_lw(RV_FP, 0, RV_SP);
    rv_lw(RV_RA, 4, RV_SP);
    rv_addi(RV_SP, RV_SP, 8);
    cb_emit32(enc_i(0, RV_RA, 0, RV_ZERO, 0x67UL));
}

static void rvr_alu_reg(IrOp op, u8 dst, u8 lhs, u8 rhs) {
    op = rv_norm32(op);
    switch (op) {
    case OP_ADD:
    case OP_AADD:
        cb_emit32(enc_r(0x00, rhs, lhs, 0, dst, 0x33UL));
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit32(enc_r(0x20, rhs, lhs, 0, dst, 0x33UL));
        break;
    case OP_MUL:
        cb_emit32(enc_r(0x01, rhs, lhs, 0, dst, 0x33UL));
        break;
    case OP_DIV:
        cb_emit32(enc_r(0x01, rhs, lhs, 4, dst, 0x33UL));
        break;
    case OP_UDIV:
        cb_emit32(enc_r(0x01, rhs, lhs, 5, dst, 0x33UL));
        break;
    case OP_MOD:
        cb_emit32(enc_r(0x01, rhs, lhs, 6, dst, 0x33UL));
        break;
    case OP_UMOD:
        cb_emit32(enc_r(0x01, rhs, lhs, 7, dst, 0x33UL));
        break;
    case OP_AND:
        cb_emit32(enc_r(0x00, rhs, lhs, 7, dst, 0x33UL));
        break;
    case OP_OR:
        cb_emit32(enc_r(0x00, rhs, lhs, 6, dst, 0x33UL));
        break;
    case OP_XOR:
        cb_emit32(enc_r(0x00, rhs, lhs, 4, dst, 0x33UL));
        break;
    case OP_LSHIFT:
        cb_emit32(enc_r(0x00, rhs, lhs, 1, dst, 0x33UL));
        break;
    case OP_RSHIFT:
        cb_emit32(enc_r(0x00, rhs, lhs, 5, dst, 0x33UL));
        break;
    case OP_ARSHIFT:
        cb_emit32(enc_r(0x20, rhs, lhs, 5, dst, 0x33UL));
        break;
    case OP_INVERT:
    case OP_INV32:
        cb_emit32(enc_i(-1, lhs, 4, dst, 0x13UL));
        break;
    case OP_SEXT:
        cb_emit32(enc_i(24, lhs, 1, dst, 0x13UL));
        cb_emit32(enc_i(0x400 | 24, dst, 5, dst, 0x13UL));
        break;
    case OP_W2A:
    case OP_A2W:
    case OP_W2I32:
    case OP_U2I32:
    case OP_I322W:
        rvr_mov_reg(dst, lhs, TY_W);
        break;
    case OP_EQ:
    case OP_AEQ:
        cb_emit32(enc_r(0x00, rhs, lhs, 4, dst, 0x33UL));
        cb_emit32(enc_i(1, dst, 3, dst, 0x13UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_NE:
    case OP_ANE:
        cb_emit32(enc_r(0x00, rhs, lhs, 4, dst, 0x33UL));
        cb_emit32(enc_r(0x00, dst, RV_ZERO, 3, dst, 0x33UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_LT:
        cb_emit32(enc_r(0x00, rhs, lhs, 2, dst, 0x33UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_GT:
        cb_emit32(enc_r(0x00, lhs, rhs, 2, dst, 0x33UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_LE:
        cb_emit32(enc_r(0x00, lhs, rhs, 2, dst, 0x33UL));
        cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_GE:
        cb_emit32(enc_r(0x00, rhs, lhs, 2, dst, 0x33UL));
        cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_ULT:
    case OP_ALT:
        cb_emit32(enc_r(0x00, rhs, lhs, 3, dst, 0x33UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_UGT:
    case OP_AGT:
        cb_emit32(enc_r(0x00, lhs, rhs, 3, dst, 0x33UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_ULE:
        cb_emit32(enc_r(0x00, lhs, rhs, 3, dst, 0x33UL));
        cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    case OP_UGE:
        cb_emit32(enc_r(0x00, rhs, lhs, 3, dst, 0x33UL));
        cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
        break;
    default:
        ir4_die("unimplemented RV32 reg ALU op");
    }
}

static int rvr_alu_imm(IrOp op, u8 dst, u8 lhs, i32 imm) {
    op = rv_norm32(op);
    if (op == OP_ADD || op == OP_AADD || op == OP_SUB || op == OP_ASUB) {
        i32 delta = (op == OP_SUB || op == OP_ASUB) ? -imm : imm;
        if (delta == 0) {
            rvr_mov_reg(dst, lhs, TY_W);
            return 1;
        }
        if (delta < -2048 || delta > 2047)
            return 0;
        rv_addi(dst, lhs, delta);
        return 1;
    }
    if (op == OP_AND || op == OP_OR || op == OP_XOR) {
        u32 f3 = (op == OP_AND) ? 7 : (op == OP_OR) ? 6 : 4;
        if (imm < -2048 || imm > 2047)
            return 0;
        cb_emit32(enc_i(imm, lhs, f3, dst, 0x13UL));
        return 1;
    }
    if (op == OP_LSHIFT || op == OP_RSHIFT || op == OP_ARSHIFT) {
        u32 c;
        if (imm < 0 || imm > 31)
            return 0;
        c = (u32)imm;
        if (c == 0) {
            rvr_mov_reg(dst, lhs, TY_W);
            return 1;
        }
        if (op == OP_LSHIFT)
            cb_emit32(enc_i((i32)c, lhs, 1, dst, 0x13UL));
        else if (op == OP_RSHIFT)
            cb_emit32(enc_i((i32)c, lhs, 5, dst, 0x13UL));
        else
            cb_emit32(enc_i((i32)(0x400U | c), lhs, 5, dst, 0x13UL));
        return 1;
    }
    if (imm >= -2048 && imm <= 2047) {
        if ((op == OP_ULT || op == OP_UGE) && imm < 0)
            return 0;
        switch (op) {
        case OP_EQ:
        case OP_NE:
            if (imm == -2048)
                return 0;
            if (imm != 0)
                rv_addi(dst, lhs, -imm);
            else
                rvr_mov_reg(dst, lhs, TY_W);
            if (op == OP_EQ)
                cb_emit32(enc_i(1, dst, 3, dst, 0x13UL));
            else
                cb_emit32(enc_r(0x00, dst, RV_ZERO, 3, dst, 0x33UL));
            cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
            return 1;
        case OP_LT:
        case OP_GE:
            cb_emit32(enc_i(imm, lhs, 2, dst, 0x13UL));
            if (op == OP_GE)
                cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
            cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
            return 1;
        case OP_ULT:
        case OP_UGE:
            cb_emit32(enc_i(imm, lhs, 3, dst, 0x13UL));
            if (op == OP_UGE)
                cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
            cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
            return 1;
        case OP_LE:
        case OP_GT:
            if (imm == 2047)
                return 0;
            cb_emit32(enc_i(imm + 1, lhs, 2, dst, 0x13UL));
            if (op == OP_GT)
                cb_emit32(enc_i(1, dst, 4, dst, 0x13UL));
            cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
            return 1;
        default:
            break;
        }
    }
    return 0;
}

static void rvr_fp_op(IrOp op, u8 dst, u8 lhs, u8 rhs) {
    if (op == OP_FNEG) {
        cb_emit32(enc_u(0x80000000UL, RV_T2, 0x37UL));
        cb_emit32(enc_r(0x00, RV_T2, lhs, 4, dst, 0x33UL));
        return;
    }
    if (lhs == RV_A1 && rhs == RV_A0) {
        rvr_mov_reg(RV_T2, lhs, TY_W);
        rvr_mov_reg(RV_A1, rhs, TY_W);
        rvr_mov_reg(RV_A0, RV_T2, TY_W);
    } else if (rhs == RV_A0 && op != OP_W2F && op != OP_F2W) {
        rvr_mov_reg(RV_A1, rhs, TY_W);
        rvr_mov_reg(RV_A0, lhs, TY_W);
    } else {
        rvr_mov_reg(RV_A0, lhs, TY_W);
        if (op != OP_W2F && op != OP_F2W)
            rvr_mov_reg(RV_A1, rhs, TY_W);
    }

    if (op >= OP_FADD && op <= OP_FDIV) {
        if (rv_obj_mode) {
            const char *fn = (op == OP_FADD)   ? "__addsf3"
                             : (op == OP_FSUB) ? "__subsf3"
                             : (op == OP_FMUL) ? "__mulsf3"
                                               : "__divsf3";
            rv_emit_extern_call(fn);
        } else {
            u16 entry = (op == OP_FADD)   ? RT_FP_ADD
                        : (op == OP_FSUB) ? RT_FP_SUB
                        : (op == OP_FMUL) ? RT_FP_MUL
                                          : RT_FP_DIV;
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + entry) - (i32)cb_cur_addr(), RV_RA));
        }
        rvr_mov_reg(dst, RV_A0, TY_W);
    } else if (op >= OP_FEQ && op <= OP_FGT) {
        if (rv_obj_mode) {
            const char *fn = (op == OP_FEQ)   ? "__eqsf2"
                             : (op == OP_FNE) ? "__nesf2"
                             : (op == OP_FLT) ? "__ltsf2"
                                              : "__gtsf2";
            rv_emit_extern_call(fn);
        } else {
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + RT_FP_CMP) - (i32)cb_cur_addr(), RV_RA));
        }
        if (op == OP_FEQ)
            cb_emit32(enc_i(1, RV_A0, 3, dst, 0x13UL));
        else if (op == OP_FNE)
            cb_emit32(enc_r(0x00, RV_A0, RV_ZERO, 3, dst, 0x33UL));
        else if (op == OP_FLT)
            cb_emit32(enc_r(0x00, RV_ZERO, RV_A0, 2, dst, 0x33UL));
        else
            cb_emit32(enc_r(0x00, RV_A0, RV_ZERO, 2, dst, 0x33UL));
        cb_emit32(enc_r(0x20, dst, RV_ZERO, 0, dst, 0x33UL));
    } else if (op == OP_W2F) {
        if (rv_obj_mode) {
            rv_emit_extern_call("__floatsisf");
        } else {
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + RT_FP_ITOF) - (i32)cb_cur_addr(), RV_RA));
        }
        rvr_mov_reg(dst, RV_A0, TY_W);
    } else if (op == OP_F2W) {
        if (rv_obj_mode) {
            rv_emit_extern_call("__fixsfsi");
        } else {
            ensure_rv32_fp();
            cb_emit32(enc_j((i32)(rv_fp_addr + RT_FP_FTOI) - (i32)cb_cur_addr(), RV_RA));
        }
        rvr_mov_reg(dst, RV_A0, TY_W);
    }
}

static u32 rvr_mem_f3(IrOp op) {
    switch (op) {
    case OP_I8FETCH:
    case OP_I8STORE:
    case OP_SI8STORE:
        return 0;
    case OP_U8FETCH:
        return 4;
    case OP_I16FETCH:
    case OP_I16STORE:
    case OP_SI16STORE:
        return 1;
    case OP_U16FETCH:
        return 5;
    default:
        return 2;
    }
}

static void rvr_load_mem(IrOp op, u8 dst, u8 base, i32 off) {
    u32 f3 = rvr_mem_f3(op);
    if (off >= -2048 && off <= 2047) {
        cb_emit32(enc_i(off, base, f3, dst, 0x03UL));
    } else {
        rv_li(RV_T2, (u32)off);
        cb_emit32(enc_r(0x00, RV_T2, base, 0, RV_T2, 0x33UL));
        cb_emit32(enc_i(0, RV_T2, f3, dst, 0x03UL));
    }
}

static void rvr_store_mem(IrOp op, u8 src, u8 base, i32 off) {
    u32 f3 = rvr_mem_f3(op);
    if (off >= -2048 && off <= 2047) {
        cb_emit32(enc_s(off, src, base, f3));
    } else {
        rv_li(RV_T2, (u32)off);
        cb_emit32(enc_r(0x00, RV_T2, base, 0, RV_T2, 0x33UL));
        cb_emit32(enc_s(0, src, RV_T2, f3));
    }
}

static void rvr_bcopy(u8 dst, u8 src, u8 len) {
    rvr_mov_reg(RV_T0, dst, TY_W);
    rvr_mov_reg(RV_T1, src, TY_W);
    rvr_mov_reg(RV_T2, len, TY_W);
    cb_emit32(enc_b(24, RV_ZERO, RV_T2, 0));
    cb_emit32(enc_i(0, RV_T1, 4, RV_A0, 0x03UL));
    cb_emit32(enc_s(0, RV_A0, RV_T0, 0));
    cb_emit32(enc_i(1, RV_T1, 0, RV_T1, 0x13UL));
    cb_emit32(enc_i(1, RV_T0, 0, RV_T0, 0x13UL));
    cb_emit32(enc_i(-1, RV_T2, 0, RV_T2, 0x13UL));
    cb_emit32(enc_b(-20, RV_ZERO, RV_T2, 1));
}

static size_t rvr_br_fwd(BranchCond cond, u8 reg) {
    size_t pos;
    if (cond == BR_JZ)
        cb_emit32(enc_b(8, RV_ZERO, reg, 1));
    pos = cb_pc();
    cb_emit32(enc_j(0, RV_ZERO));
    return pos;
}

static void rvr_br_back(BranchCond cond, u8 reg, size_t target_addr) {
    i32 diff;
    if (cond == BR_JZ) {
        diff = (i32)target_addr - (i32)cb_cur_addr();
        if (diff >= -4096 && diff <= 4094) {
            cb_emit32(enc_b(diff, RV_ZERO, reg, 0));
        } else {
            cb_emit32(enc_b(8, RV_ZERO, reg, 1));
            diff = (i32)target_addr - (i32)cb_cur_addr();
            cb_emit32(enc_j(diff, RV_ZERO));
        }
    } else {
        diff = (i32)target_addr - (i32)cb_cur_addr();
        cb_emit32(enc_j(diff, RV_ZERO));
    }
}

static int rvr_cmp_br_emit(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, int is_fwd,
                           size_t target_addr, size_t *patch_off) {
    u32 rs1 = lhs;
    u32 rs2 = rhs;
    u32 f3_true, f3_false;
    int swap_ops = 0;

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        f3_true = 0; f3_false = 1; break;
    case OP_NE:
    case OP_ANE:
        f3_true = 1; f3_false = 0; break;
    case OP_LT:
        f3_true = 4; f3_false = 5; break;
    case OP_GE:
        f3_true = 5; f3_false = 4; break;
    case OP_GT:
        swap_ops = 1; f3_true = 4; f3_false = 5; break;
    case OP_LE:
        swap_ops = 1; f3_true = 5; f3_false = 4; break;
    case OP_ULT:
    case OP_ALT:
        f3_true = 6; f3_false = 7; break;
    case OP_UGE:
        f3_true = 7; f3_false = 6; break;
    case OP_UGT:
    case OP_AGT:
        swap_ops = 1; f3_true = 6; f3_false = 7; break;
    case OP_ULE:
        swap_ops = 1; f3_true = 7; f3_false = 6; break;
    default:
        return 0;
    }

    if (has_imm) {
        if (imm == 0) {
            rs2 = RV_ZERO;
        } else {
            rv_li(RV_T2, (u32)imm);
            rs2 = RV_T2;
        }
    }
    if (swap_ops) {
        u32 tmp = rs1;
        rs1 = rs2;
        rs2 = tmp;
    }
    if (is_fwd) {
        cb_emit32(enc_b(8, rs2, rs1, f3_true));
        *patch_off = cb_pc();
        cb_emit32(enc_j(0, RV_ZERO));
    } else {
        i32 diff = (i32)target_addr - (i32)cb_cur_addr();
        if (diff >= -4096 && diff <= 4094) {
            cb_emit32(enc_b(diff, rs2, rs1, f3_false));
        } else {
            cb_emit32(enc_b(8, rs2, rs1, f3_true));
            diff = (i32)target_addr - (i32)cb_cur_addr();
            cb_emit32(enc_j(diff, RV_ZERO));
        }
    }
    return 1;
}

static int rvr_cmp_br_fwd(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, size_t *patch_off) {
    return rvr_cmp_br_emit(op, has_imm, imm, lhs, rhs, 1, 0, patch_off);
}

static int rvr_cmp_br_back(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, size_t target_addr) {
    return rvr_cmp_br_emit(op, has_imm, imm, lhs, rhs, 0, target_addr, NULL);
}

static size_t rvr_emit_jtab(u8 reg, size_t count) {
    size_t tbl_off, k;
    cb_emit32(enc_i(2, reg, 1, RV_T0, 0x13UL));
    cb_emit32(0x00000317UL); /* AUIPC t1, 0 */
    cb_emit32(enc_i(24, RV_T1, 0, RV_T1, 0x13UL));
    cb_emit32(enc_r(0x00, RV_T0, RV_T1, 0, RV_T2, 0x33UL));
    rv_lw(RV_T0, 0, RV_T2);
    cb_emit32(enc_r(0x00, RV_T0, RV_T1, 0, RV_T0, 0x33UL));
    cb_emit32(enc_i(0, RV_T0, 0, RV_ZERO, 0x67UL));
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit32(0);
    return tbl_off;
}

static void rvr_icall(u8 reg) { cb_emit32(enc_i(0, reg, 0, RV_RA, 0x67UL)); }

static void rvr_call_extern(const FnSig *sig, const u8 *arg_regs) {
    (void)arg_regs; /* stk_regs[0..7] are already a0..a7 */
    if (sig->in_cnt > 8 || sig->out_cnt > 1)
        ir4_die("RV32 extern supports at most 8 args and 1 return");
    if (FN_FLAG(sig, FN_FL_HAS_ADDR)) {
        rv_li(RV_T0, (u32)sig->addr);
        cb_emit32(enc_i(0, RV_T0, 0, RV_RA, 0x67UL));
    } else {
        rv_emit_extern_call(sig->name);
    }
}

const RegBackend reg_be_rv32 = {
    4, 0, 12, rvr_stk_regs, NULL, 9, rvr_loc_regs, 5, 6, 7, 5, 6,
    rv_cur_addr, rvr_fn_begin, rvr_fn_end, rvr_spill_store, rvr_spill_load,
    rvr_local_addr, rvr_sp_adj, rvr_store_out_arg, rvr_mov_reg, rvr_mov_imm,
    rvr_mov_uimm32, rvr_mov_fconst, rvr_load_data_addr, rvr_load_fn_addr,
    rvr_alu_reg, rvr_alu_imm, rvr_fp_op, rvr_load_mem, rvr_store_mem, rvr_bcopy,
    rvr_br_fwd, rvr_br_back, rvr_cmp_br_fwd, rvr_cmp_br_back, rv_patch_fwd,
    rvr_emit_jtab, rv_patch_jtab, rv_call, rvr_icall, rvr_call_extern};
