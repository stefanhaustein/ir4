/* bc.c - Compact 16-bit Bytecode Backend for IR4th */
#include "backend/bc.h"
#include "codebuf.h"
#include "vec.h"

VEC_OF(SizeVec, size_t);
VEC_OF(U8Vec, u8);

static StrVec extern_names;
static SizeVec bc_local_off;
static U8Vec bc_local_is_static;
static size_t bc_frame_bytes = 0;
static Bool bc_is_rec = false;

const char *bc_extern_name(u16 id) {
    if (id < extern_names.len)
        return extern_names.data[id];
    return "";
}

static size_t bc_cur_addr(void) { return cb_cur_addr(); }

static u8 type_size(StackType t) { return (t == TY_F || t == TY_I32) ? 4 : 2; }

static void bc_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
    u8 sz = type_size(type);
    u16 off = (u16)bc_local_off.data[slot];
    (void)stack_bytes;
    if (op == LOCAL_ADD) {
        bc_local_op(LOCAL_GET, slot, type, stack_bytes);
        cb_emit8(BC_ADD);
        bc_local_op(LOCAL_SET, slot, type, stack_bytes);
        return;
    }
    if (bc_local_is_static.data[slot]) {
        if (op == LOCAL_ADDR) {
            cb_emit8(BC_PUSH_DATA);
            cb_add_data_reloc(cb_pc(), off);
            cb_emit16(0);
        } else if (op == LOCAL_GET) {
            cb_emit8(sz == 4 ? BC_STATIC_GET4 : BC_STATIC_GET2);
            cb_add_data_reloc(cb_pc(), off);
            cb_emit16(0);
        } else {
            cb_emit8(sz == 4 ? BC_STATIC_SET4 : BC_STATIC_SET2);
            cb_add_data_reloc(cb_pc(), off);
            cb_emit16(0);
        }
    } else {
        if (op == LOCAL_ADDR) {
            cb_emit8(BC_LOCAL_ADDR);
            cb_emit16(off);
        } else if (op == LOCAL_GET) {
            cb_emit8(sz == 4 ? BC_LOCAL_GET4 : BC_LOCAL_GET2);
            cb_emit16(off);
        } else {
            cb_emit8(sz == 4 ? BC_LOCAL_SET4 : BC_LOCAL_SET2);
            cb_emit16(off);
        }
    }
}

static void bc_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                        size_t total_locals, Bool is_recursive) {
    size_t i;
    (void)name;
    bc_local_off.len = 0;
    bc_local_is_static.len = 0;
    bc_frame_bytes = 0;
    bc_is_rec = is_recursive;

    for (i = 0; i < total_locals; i++) {
        size_t sz = locals[i].size ? locals[i].size : type_size(locals[i].type);
        if (!is_recursive) {
            size_t off = cb_alloc_data(sz, 2);
            VEC_PUSH(bc_local_off, off);
            VEC_PUSH(bc_local_is_static, 1);
        } else {
            size_t off = bc_frame_bytes;
            bc_frame_bytes += (sz + 1) & ~(size_t)1;
            VEC_PUSH(bc_local_off, off);
            VEC_PUSH(bc_local_is_static, 0);
        }
    }

    if (is_recursive && bc_frame_bytes > 0) {
        cb_emit8(BC_ENTER);
        cb_emit16((u16)bc_frame_bytes);
    }

    for (i = param_cnt; i > 0; i--) {
        size_t p = i - 1;
        bc_local_op(LOCAL_SET, p, locals[p].type, 0);
    }
}

static void bc_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
    (void)sig;
    (void)locals;
    (void)total_locals;
    if (bc_is_rec && bc_frame_bytes > 0) {
        cb_emit8(BC_LEAVE);
        cb_emit16((u16)bc_frame_bytes);
    }
    cb_emit8(BC_RET);
}

static void bc_call(size_t target_addr) {
    cb_emit8(BC_CALL);
    cb_emit16((u16)target_addr);
}

static void bc_call_extern(const FuncSig *sig) {
    size_t i;
    for (i = 0; i < extern_names.len; i++) {
        if (strcmp(extern_names.data[i], sig->name) == 0) {
            cb_emit8(BC_CALL_EXTERN);
            cb_emit16((u16)i);
            return;
        }
    }
    VEC_PUSH(extern_names, sig->name);
    cb_emit8(BC_CALL_EXTERN);
    cb_emit16((u16)(extern_names.len - 1));
}

static void bc_push_w(i32 val) {
    i16 v = (i16)val;
    if (v >= 0 && v <= 127) {
        cb_emit8((u8)(BC_PUSH_IMM7_BASE | v));
    } else if (v >= -128 && v < 0) {
        cb_emit8(BC_PUSH_I8);
        cb_emit8((u8)(v & 0xFF));
    } else {
        cb_emit8(BC_PUSH_I16);
        cb_emit16((u16)v);
    }
}

static void bc_push_a(u32 val) {
    cb_emit8(BC_PUSH_I16);
    cb_emit16((u16)val);
}

static void bc_push_f(double val) {
    float f = (float)val;
    u32 bits;
    memcpy(&bits, &f, 4);
    if (bits == 0) {
        cb_emit8(BC_PUSH_0_32);
        return;
    }
    cb_emit8(BC_PUSH_F);
    cb_emit32(bits);
}

static void bc_push_i32(i32 val) {
    if (val == 0) {
        cb_emit8(BC_PUSH_0_32);
        return;
    }
    cb_emit8(BC_PUSH_I32);
    cb_emit32((u32)val);
}

static void bc_push_data(size_t data_offset) {
    cb_emit8(BC_PUSH_DATA);
    cb_add_data_reloc(cb_pc(), data_offset);
    cb_emit16(0);
}

static void bc_stack_op(StackOp op, StackType t1, StackType t2) {
    u8 s1 = type_size(t1);
    u8 s2 = type_size(t2);
    switch (op) {
    case OP_DUP:
        cb_emit8(s1 == 4 ? BC_DUP4 : BC_DUP2);
        break;
    case OP_DROP:
        cb_emit8(s1 == 4 ? BC_DROP4 : BC_DROP2);
        break;
    case OP_SWAP:
        if (s1 == 2 && s2 == 2)
            cb_emit8(BC_SWAP22);
        else if (s1 == 4 && s2 == 2)
            cb_emit8(BC_SWAP24);
        else if (s1 == 2 && s2 == 4)
            cb_emit8(BC_SWAP42);
        else
            cb_emit8(BC_SWAP44);
        break;
    }
}

static void bc_op(IrOp op) {
    switch (op) {
    case OP_ADD:
    case OP_AADD:
        cb_emit8(BC_ADD);
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit8(BC_SUB);
        break;
    case OP_MUL:
        cb_emit8(BC_MUL);
        break;
    case OP_DIV:
        cb_emit8(BC_DIV);
        break;
    case OP_MOD:
        cb_emit8(BC_MOD);
        break;
    case OP_UDIV:
        cb_emit8(BC_UDIV);
        break;
    case OP_UMOD:
        cb_emit8(BC_UMOD);
        break;
    case OP_AND:
        cb_emit8(BC_AND);
        break;
    case OP_OR:
        cb_emit8(BC_OR);
        break;
    case OP_XOR:
        cb_emit8(BC_XOR);
        break;
    case OP_INVERT:
        cb_emit8(BC_INVERT);
        break;
    case OP_LSHIFT:
        cb_emit8(BC_LSHIFT);
        break;
    case OP_ARSHIFT:
        cb_emit8(BC_ARSHIFT);
        break;
    case OP_RSHIFT:
        cb_emit8(BC_RSHIFT);
        break;

    case OP_FADD:
        cb_emit8(BC_FADD);
        break;
    case OP_FSUB:
        cb_emit8(BC_FSUB);
        break;
    case OP_FMUL:
        cb_emit8(BC_FMUL);
        break;
    case OP_FDIV:
        cb_emit8(BC_FDIV);
        break;
    case OP_FNEG:
        cb_emit8(BC_FNEG);
        break;

    case OP_ADD32:
        cb_emit8(BC_ADD32);
        break;
    case OP_SUB32:
        cb_emit8(BC_SUB32);
        break;
    case OP_MUL32:
        cb_emit8(BC_MUL32);
        break;
    case OP_DIV32:
        cb_emit8(BC_DIV32);
        break;
    case OP_MOD32:
        cb_emit8(BC_MOD32);
        break;
    case OP_UDIV32:
        cb_emit8(BC_UDIV32);
        break;
    case OP_UMOD32:
        cb_emit8(BC_UMOD32);
        break;
    case OP_AND32:
        cb_emit8(BC_AND32);
        break;
    case OP_OR32:
        cb_emit8(BC_OR32);
        break;
    case OP_XOR32:
        cb_emit8(BC_XOR32);
        break;
    case OP_INV32:
        cb_emit8(BC_INV32);
        break;
    case OP_SHL32:
        cb_emit8(BC_SHL32);
        break;
    case OP_ASHR32:
        cb_emit8(BC_ASHR32);
        break;
    case OP_RSHR32:
        cb_emit8(BC_RSHR32);
        break;

    case OP_EQ:
    case OP_AEQ:
        cb_emit8(BC_EQ);
        break;
    case OP_NE:
    case OP_ANE:
        cb_emit8(BC_NE);
        break;
    case OP_LT:
        cb_emit8(BC_LT);
        break;
    case OP_GT:
        cb_emit8(BC_GT);
        break;
    case OP_LE:
        cb_emit8(BC_LE);
        break;
    case OP_GE:
        cb_emit8(BC_GE);
        break;
    case OP_ULT:
    case OP_ALT:
        cb_emit8(BC_ULT);
        break;
    case OP_UGT:
    case OP_AGT:
        cb_emit8(BC_UGT);
        break;
    case OP_ULE:
        cb_emit8(BC_ULE);
        break;
    case OP_UGE:
        cb_emit8(BC_UGE);
        break;

    case OP_FEQ:
        cb_emit8(BC_FEQ);
        break;
    case OP_FNE:
        cb_emit8(BC_FNE);
        break;
    case OP_FLT:
        cb_emit8(BC_FLT);
        break;
    case OP_FGT:
        cb_emit8(BC_FGT);
        break;

    case OP_EQ32:
        cb_emit8(BC_EQ32);
        break;
    case OP_NE32:
        cb_emit8(BC_NE32);
        break;
    case OP_LT32:
        cb_emit8(BC_LT32);
        break;
    case OP_GT32:
        cb_emit8(BC_GT32);
        break;
    case OP_LE32:
        cb_emit8(BC_LE32);
        break;
    case OP_GE32:
        cb_emit8(BC_GE32);
        break;
    case OP_ULT32:
        cb_emit8(BC_ULT32);
        break;
    case OP_UGT32:
        cb_emit8(BC_UGT32);
        break;
    case OP_ULE32:
        cb_emit8(BC_ULE32);
        break;
    case OP_UGE32:
        cb_emit8(BC_UGE32);
        break;

    case OP_I8FETCH:
        cb_emit8(BC_I8FETCH);
        break;
    case OP_U8FETCH:
        cb_emit8(BC_U8FETCH);
        break;
    case OP_I8STORE:
        cb_emit8(BC_I8STORE);
        break;
    case OP_SI8STORE:
        cb_emit8(BC_SWAP22);
        cb_emit8(BC_I8STORE);
        break;
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_FETCH:
    case OP_AFETCH:
        cb_emit8(BC_FETCH16);
        break;
    case OP_I16STORE:
    case OP_STORE:
    case OP_ASTORE:
        cb_emit8(BC_STORE16);
        break;
    case OP_SI16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
        cb_emit8(BC_SWAP22);
        cb_emit8(BC_STORE16);
        break;
    case OP_FFETCH:
    case OP_I32FETCH:
        cb_emit8(BC_FETCH32);
        break;
    case OP_FSTORE:
    case OP_I32STORE:
        cb_emit8(BC_STORE32);
        break;
    case OP_SFSTORE:
        cb_emit8(BC_SWAP24);
        cb_emit8(BC_STORE32);
        break;
    case OP_BCOPY:
        cb_emit8(BC_BCOPY);
        break;

    case OP_SEXT:
        cb_emit8(BC_SEXT);
        break;
    case OP_W2A:
    case OP_A2W:
        break;
    case OP_W2F:
        cb_emit8(BC_W2F);
        break;
    case OP_F2W:
        cb_emit8(BC_F2W);
        break;
    case OP_W2I32:
        cb_emit8(BC_W2I32);
        break;
    case OP_U2I32:
        cb_emit8(BC_U2I32);
        break;
    case OP_I322W:
        cb_emit8(BC_I322W);
        break;
    }
}

static int bc_add_imm(i32 imm) {
    i16 d = (i16)imm;
    if (d == 0)
        return 1;
    if (d == 1) {
        cb_emit8(BC_INC);
        return 1;
    }
    if (d == -1) {
        cb_emit8(BC_DEC);
        return 1;
    }
    if (d >= -128 && d <= 127) {
        cb_emit8(BC_ADD_IMM8);
        cb_emit8((u8)(d & 0xFF));
        return 1;
    }
    return 0;
}

static int bc_cmp_imm(IrOp op, i32 imm) {
    (void)op;
    (void)imm;
    return 0;
}

static int bc_shift_imm(IrOp op, u8 count) {
    (void)op;
    (void)count;
    return 0;
}

static int bc_mem_imm(IrOp op, i32 off) {
    (void)op;
    (void)off;
    return 0;
}

static int bc_data_mem(IrOp op, size_t data_offset) {
    (void)op;
    (void)data_offset;
    return 0;
}

static int bc_local_mem(size_t slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)slot;
    (void)off;
    (void)op;
    (void)stack_bytes;
    return 0;
}

static size_t bc_emit_br_fwd(BranchCond cond) {
    size_t pos;
    cb_emit8(cond == BR_JZ ? BC_JZ : BC_JMP);
    pos = cb_pc();
    cb_emit16(0);
    return pos;
}

static void bc_emit_br_back(BranchCond cond, size_t target_addr) {
    cb_emit8(cond == BR_JZ ? BC_JZ : BC_JMP);
    cb_emit16((u16)target_addr);
}

static void bc_patch_fwd(size_t patch_offset) { cb_patch16(patch_offset, (u16)cb_cur_addr()); }

static size_t bc_emit_jtab(size_t count) {
    size_t tbl_off, k;
    cb_emit8(BC_JTAB);
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit16(0);
    return tbl_off;
}

static void bc_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    cb_patch16(patch_offset + idx * 2, (u16)target_addr);
}

static void bc_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    cb_patch16(call_pc + 1, (u16)target_addr);
}

static size_t bc_push_fn(size_t target_addr) {
    size_t pos;
    cb_emit8(BC_PUSH_FN);
    pos = cb_pc();
    cb_emit16((u16)target_addr);
    return pos;
}

static void bc_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    cb_patch16(patch_pc, (u16)target_addr);
}

static void bc_icall(void) { cb_emit8(BC_ICALL); }

static size_t bc_stack_data_off(void) { return 0; }

const Backend be_bc = {"bc",
                       2,
                       2,
                       4,
                       bc_cur_addr,
                       bc_fn_begin,
                       bc_fn_end,
                       bc_call,
                       NULL,
                       bc_call_extern,
                       bc_push_w,
                       bc_push_a,
                       bc_push_f,
                       bc_push_i32,
                       bc_push_data,
                       bc_local_op,
                       NULL,
                       bc_stack_op,
                       bc_op,
                       bc_add_imm,
                       bc_cmp_imm,
                       bc_shift_imm,
                       NULL,
                       bc_mem_imm,
                       bc_data_mem,
                       bc_local_mem,
                       bc_emit_br_fwd,
                       bc_emit_br_back,
                       NULL,
                       NULL,
                       bc_patch_fwd,
                       bc_emit_jtab,
                       bc_patch_jtab,
                       bc_patch_call,
                       bc_push_fn,
                       bc_patch_fn,
                       bc_icall,
                       bc_stack_data_off,
                       NULL};
