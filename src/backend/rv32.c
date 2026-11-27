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
#include "backend/rv32.h"
#include "vec.h"
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

static void rv_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                        size_t total_locals, Bool is_recursive) {
    size_t i;
    i32 cur_off = 0;
    (void)name;
    (void)is_recursive;

    VEC_ENSURE(rv_local_off, total_locals);
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

static void rv_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
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

static int rv_tail_call(size_t target_addr, const FuncSig *sig, const FuncSig *callee,
                        const LocalVar *locals, size_t total_locals) {
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

static void rv_call_extern(const FuncSig *sig) {
    size_t i;
    if (sig->in_types.len > 8 || sig->out_types.len > 1) {
        ir4_die("RV32 extern supports at most 8 args and 1 return");
    }

    for (i = sig->in_types.len; i > 0; i--) {
        pop_reg(RV_A0 + (u32)(i - 1));
    }

    if (sig->has_addr) {
        rv_li(RV_T0, (u32)sig->addr);
        cb_emit32(enc_i(0, RV_T0, 0, RV_RA, 0x67UL)); /* JALR ra, 0(t0) */
    } else {
        rv_emit_extern_call(sig->name);
    }

    if (sig->out_types.len == 1) {
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

static void rv_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
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
    cb_patch32(patch_pc, enc_u(hi, RV_T0, 0x17UL));
    cb_patch32(patch_pc + 4, enc_i(lo, RV_T0, 0, RV_T0, 0x13UL));
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
