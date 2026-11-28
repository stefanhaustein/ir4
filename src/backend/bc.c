/* bc.c - Compact 16-bit Bytecode Backend for IR4th */
#include "backend/bc.h"
#include "codebuf.h"

VEC_OF(SizeVec, size_t);
VEC_OF(U8Vec, u8);

static StrVec extern_names;
static SizeVec bc_local_off;
static U8Vec bc_local_is_static;
static size_t bc_frame_bytes = 0;
static Bool bc_is_rec = false;

u16 bc_extern_count(void) { return (u16)extern_names.len; }

const char *bc_extern_name(u16 id) {
    if (id < extern_names.len)
        return extern_names.data[id];
    return "";
}

static size_t bc_cur_addr(void) { return cb_cur_addr(); }

static u8 type_size_w(StackType t, u8 word_bytes) {
    return (t == TY_F || t == TY_I32 || word_bytes == 4) ? 4 : 2;
}

static u8 type_size(StackType t) { return type_size_w(t, 2); }

static void bc_local_op_w(LocalOp op, int slot, StackType type, size_t stack_bytes,
                          u8 word_bytes) {
    u8 sz = type_size_w(type, word_bytes);
    size_t off = bc_local_off.data[slot];
    (void)stack_bytes;
    if (op == LOCAL_ADD) {
        bc_local_op_w(LOCAL_GET, slot, type, stack_bytes, word_bytes);
        cb_emit8(BC_ADD);
        bc_local_op_w(LOCAL_SET, slot, type, stack_bytes, word_bytes);
        return;
    }
    if (bc_local_is_static.data[slot]) {
        if (op == LOCAL_ADDR) {
            cb_emit8(BC_PUSH_DATA);
        } else if (op == LOCAL_GET) {
            cb_emit8(sz == 4 ? BC_STATIC_GET32 : BC_STATIC_GET);
        } else {
            cb_emit8(sz == 4 ? BC_STATIC_SET32 : BC_STATIC_SET);
        }
        bc_emit_data_reloc(off, word_bytes);
    } else {
        if (op == LOCAL_ADDR) {
            cb_emit8(BC_LOCAL_ADDR);
        } else if (op == LOCAL_GET) {
            cb_emit8(sz == 4 ? BC_LOCAL_GET32 : BC_LOCAL_GET);
        } else {
            cb_emit8(sz == 4 ? BC_LOCAL_SET32 : BC_LOCAL_SET);
        }
        bc_emit_addr(off, word_bytes);
    }
}

static void bc_local_op(LocalOp op, int slot, StackType type, size_t stack_bytes) {
    bc_local_op_w(op, slot, type, stack_bytes, 2);
}

static void bc_fn_begin_w(const char *name, int param_cnt, const LocalVar *locals,
                          int total_locals, Bool is_recursive, u8 word_bytes) {
    int i;
    u8 align = (word_bytes == 4) ? 4 : 2;
    (void)name;
    bc_local_off.len = 0;
    bc_local_is_static.len = 0;
    bc_frame_bytes = 0;
    bc_is_rec = is_recursive;

    for (i = 0; i < total_locals; i++) {
        size_t sz = locals[i].size ? locals[i].size : type_size_w(locals[i].type, word_bytes);
        if (!is_recursive) {
            size_t off = cb_alloc_data(sz, align);
            VEC_PUSH(bc_local_off, off);
            VEC_PUSH(bc_local_is_static, 1);
        } else {
            size_t off = bc_frame_bytes;
            bc_frame_bytes += (sz + align - 1) & ~(size_t)(align - 1);
            VEC_PUSH(bc_local_off, off);
            VEC_PUSH(bc_local_is_static, 0);
        }
    }

    if (is_recursive && bc_frame_bytes > 0) {
        cb_emit8(BC_ENTER);
        bc_emit_addr(bc_frame_bytes, word_bytes);
    }

    for (i = param_cnt; i > 0; i--) {
        int p = i - 1;
        bc_local_op_w(LOCAL_SET, p, locals[p].type, 0, word_bytes);
    }
}

static void bc_fn_begin(const char *name, int param_cnt, const LocalVar *locals,
                        int total_locals, Bool is_recursive) {
    bc_fn_begin_w(name, param_cnt, locals, total_locals, is_recursive, 2);
}

static void bc_fn_end_w(const FuncSig *sig, const LocalVar *locals, int total_locals,
                        u8 word_bytes) {
    (void)sig;
    (void)locals;
    (void)total_locals;
    if (bc_is_rec && bc_frame_bytes > 0) {
        cb_emit8(BC_LEAVE);
        bc_emit_addr(bc_frame_bytes, word_bytes);
    }
    cb_emit8(BC_RET);
}

static void bc_fn_end(const FuncSig *sig, const LocalVar *locals, int total_locals) {
    bc_fn_end_w(sig, locals, total_locals, 2);
}

static void bc_call(size_t target_addr) {
    cb_emit8(BC_CALL);
    cb_emit16((u16)target_addr);
}

static void bc32_call(size_t target_addr) {
    cb_emit8(BC_CALL);
    cb_emit32((u32)target_addr);
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

void bc_emit_addr(size_t val, u8 addr_bytes) {
    if (addr_bytes == 4)
        cb_emit32((u32)val);
    else if (addr_bytes == 3)
        cb_emit24((u32)val);
    else
        cb_emit16((u16)val);
}

void bc_patch_addr(size_t patch_off, size_t target, u8 addr_bytes) {
    if (addr_bytes == 4)
        cb_patch32(patch_off, (u32)target);
    else if (addr_bytes == 3)
        cb_patch24(patch_off, (u32)target);
    else
        cb_patch16(patch_off, (u16)target);
}

void bc_emit_data_reloc(size_t data_off, u8 addr_bytes) {
    if (addr_bytes == 4) {
        cb_add_data_reloc32(cb_pc(), data_off);
        cb_emit32(0);
    } else if (addr_bytes == 3) {
        cb_add_data_reloc24(cb_pc(), data_off);
        cb_emit24(0);
    } else {
        cb_add_data_reloc(cb_pc(), data_off);
        cb_emit16(0);
    }
}

void bc_emit_push_w(i32 val, u8 word_bytes) {
    if (word_bytes == 4) {
        if (val >= 0 && val <= 127) {
            cb_emit8((u8)(BC_PUSH_IMM7_BASE | val));
        } else if (val >= -128 && val < 0) {
            cb_emit8_8(BC_PUSH_I8, (u8)(val & 0xFF));
        } else {
            cb_emit8(BC_PUSH_W);
            cb_emit32((u32)val);
        }
    } else if (word_bytes == 3) {
        u32 u = (u32)val & 0xFFFFFFUL;
        i32 sv = (u & 0x800000UL) ? (i32)(u | 0xFF000000UL) : (i32)u;
        if (sv >= 0 && sv <= 127) {
            cb_emit8((u8)(BC_PUSH_IMM7_BASE | sv));
        } else if (sv >= -128 && sv < 0) {
            cb_emit8_8(BC_PUSH_I8, (u8)(sv & 0xFF));
        } else if (sv >= -32768 && sv <= 32767) {
            cb_emit8_16(BC_PUSH_W, (u16)(sv & 0xFFFF));
        } else {
            cb_emit8(BC_PUSH_DATA);
            cb_emit24(u);
        }
    } else {
        i16 v = (i16)val;
        if (v >= 0 && v <= 127) {
            cb_emit8((u8)(BC_PUSH_IMM7_BASE | v));
        } else if (v >= -128 && v < 0) {
            cb_emit8_8(BC_PUSH_I8, (u8)(v & 0xFF));
        } else {
            cb_emit8_16(BC_PUSH_W, (u16)v);
        }
    }
}

void bc_emit_push_a(u32 val, u8 addr_bytes) {
    if (addr_bytes == 3 || addr_bytes == 4) {
        bc_emit_push_w((i32)val, addr_bytes);
    } else {
        cb_emit8_16(BC_PUSH_W, (u16)val);
    }
}

void bc_emit_push_f(double val) {
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

void bc_emit_push_i32(i32 val) {
    if (val == 0) {
        cb_emit8(BC_PUSH_0_32);
        return;
    }
    cb_emit8(BC_PUSH_I32);
    cb_emit32((u32)val);
}

void bc_emit_push_data(size_t data_offset, u8 addr_bytes) {
    cb_emit8(BC_PUSH_DATA);
    bc_emit_data_reloc(data_offset, addr_bytes);
}

void bc_emit_stack_op(StackOp op, StackType t1, StackType t2) {
    u8 s1 = type_size(t1);
    u8 s2 = type_size(t2);
    switch (op) {
    case OP_DUP:
        cb_emit8(s1 == 4 ? BC_DUP32 : BC_DUP);
        break;
    case OP_DROP:
        cb_emit8(s1 == 4 ? BC_DROP32 : BC_DROP);
        break;
    case OP_SWAP:
        if (s1 == 2 && s2 == 2)
            cb_emit8(BC_SWAP);
        else if (s1 == 4 && s2 == 2)
            cb_emit8(BC_SWAP_W32);
        else if (s1 == 2 && s2 == 4)
            cb_emit8(BC_SWAP_32W);
        else
            cb_emit8(BC_SWAP32);
        break;
    }
}

static void bc_push_w(i32 val) { bc_emit_push_w(val, 2); }
static void bc_push_a(u32 val) { bc_emit_push_a(val, 2); }
static void bc_push_f(double val) { bc_emit_push_f(val); }
static void bc_push_i32(i32 val) { bc_emit_push_i32(val); }
static void bc_push_data(size_t data_offset) { bc_emit_push_data(data_offset, 2); }
static void bc_stack_op(StackOp op, StackType t1, StackType t2) { bc_emit_stack_op(op, t1, t2); }

void bc_emit_op(IrOp op, u8 word_bytes) {
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
        cb_emit8(BC_SWAP);
        cb_emit8(BC_I8STORE);
        break;
    case OP_I16FETCH:
        cb_emit8(word_bytes == 2 ? BC_FETCH : BC_I16FETCH);
        break;
    case OP_U16FETCH:
        cb_emit8(word_bytes == 2 ? BC_FETCH : BC_U16FETCH);
        break;
    case OP_FETCH:
    case OP_AFETCH:
        cb_emit8(BC_FETCH);
        break;
    case OP_I16STORE:
        cb_emit8(word_bytes == 2 ? BC_STORE : BC_STORE16);
        break;
    case OP_STORE:
    case OP_ASTORE:
        cb_emit8(BC_STORE);
        break;
    case OP_SI16STORE:
        cb_emit8(BC_SWAP);
        cb_emit8(word_bytes == 2 ? BC_STORE : BC_STORE16);
        break;
    case OP_SSTORE:
    case OP_SASTORE:
        cb_emit8(BC_SWAP);
        cb_emit8(BC_STORE);
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
        cb_emit8(BC_SWAP_W32);
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
    case OP_U2I32:
    case OP_I322W:
        if (word_bytes == 4)
            return;
        cb_emit8(op == OP_W2I32 ? BC_W2I32 : (op == OP_U2I32 ? BC_U2I32 : BC_I322W));
        break;
    }
}

static void bc_op(IrOp op) { bc_emit_op(op, 2); }

int bc_emit_add_imm(i32 imm) {
    if (imm == 0)
        return 1;
    if (imm == 1) {
        cb_emit8(BC_INC);
        return 1;
    }
    if (imm == -1) {
        cb_emit8(BC_DEC);
        return 1;
    }
    if (imm >= -128 && imm <= 127) {
        cb_emit8(BC_ADD_IMM8);
        cb_emit8((u8)(imm & 0xFF));
        return 1;
    }
    return 0;
}

static int bc_add_imm(int is_addr, i32 imm) {
    (void)is_addr;
    return bc_emit_add_imm((i16)imm);
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

static int bc_local_mem(int slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)slot;
    (void)off;
    (void)op;
    (void)stack_bytes;
    return 0;
}

size_t bc_emit_br_fwd_op(BranchCond cond, u8 addr_bytes) {
    size_t pos;
    cb_emit8(cond == BR_JZ ? BC_JZ : BC_JMP);
    pos = cb_pc();
    bc_emit_addr(0, addr_bytes);
    return pos;
}

void bc_emit_br_back_op(BranchCond cond, size_t target_addr, u8 addr_bytes) {
    cb_emit8(cond == BR_JZ ? BC_JZ : BC_JMP);
    bc_emit_addr(target_addr, addr_bytes);
}

void bc_patch_fwd_op(size_t patch_offset, u8 addr_bytes) {
    bc_patch_addr(patch_offset, cb_cur_addr(), addr_bytes);
}

size_t bc_emit_jtab_op(size_t count, u8 addr_bytes) {
    size_t tbl_off, k;
    cb_emit8(BC_JTAB);
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        bc_emit_addr(0, addr_bytes);
    return tbl_off;
}

void bc_patch_jtab_op(size_t patch_offset, size_t idx, size_t target_addr, u8 addr_bytes) {
    bc_patch_addr(patch_offset + idx * addr_bytes, target_addr, addr_bytes);
}

void bc_patch_call_op(size_t call_pc, size_t target_addr, u8 addr_bytes) {
    bc_patch_addr(call_pc + 1, target_addr, addr_bytes);
}

size_t bc_emit_push_fn_op(size_t target_addr, u8 addr_bytes) {
    size_t pos;
    cb_emit8(BC_PUSH_FN);
    pos = cb_pc();
    bc_emit_addr(target_addr, addr_bytes);
    return pos;
}

void bc_patch_fn_op(size_t patch_pc, size_t target_addr, u8 addr_bytes) {
    bc_patch_addr(patch_pc, target_addr, addr_bytes);
}

static size_t bc_emit_br_fwd(BranchCond cond) { return bc_emit_br_fwd_op(cond, 2); }
static void bc_emit_br_back(BranchCond cond, size_t target_addr) { bc_emit_br_back_op(cond, target_addr, 2); }
static void bc_patch_fwd(size_t patch_offset) { bc_patch_fwd_op(patch_offset, 2); }
static size_t bc_emit_jtab(size_t count) { return bc_emit_jtab_op(count, 2); }
static void bc_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    bc_patch_jtab_op(patch_offset, idx, target_addr, 2);
}
static void bc_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    bc_patch_call_op(call_pc, target_addr, 2);
}
static size_t bc_push_fn(size_t target_addr) { return bc_emit_push_fn_op(target_addr, 2); }
static void bc_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    bc_patch_fn_op(patch_pc, target_addr, 2);
}

static void bc_icall(void) { cb_emit8(BC_ICALL); }

static size_t bc_stack_data_off(void) { return 0; }

static void bc32_fn_begin(const char *name, int param_cnt, const LocalVar *locals,
                          int total_locals, Bool is_recursive) {
    bc_fn_begin_w(name, param_cnt, locals, total_locals, is_recursive, 4);
}
static void bc32_fn_end(const FuncSig *sig, const LocalVar *locals, int total_locals) {
    bc_fn_end_w(sig, locals, total_locals, 4);
}
static void bc32_push_w(i32 val) { bc_emit_push_w(val, 4); }
static void bc32_push_a(u32 val) { bc_emit_push_a(val, 4); }
static void bc32_push_data(size_t data_offset) { bc_emit_push_data(data_offset, 4); }
static void bc32_local_op(LocalOp op, int slot, StackType type, size_t stack_bytes) {
    bc_local_op_w(op, slot, type, stack_bytes, 4);
}
static void bc32_op(IrOp op) { bc_emit_op(op, 4); }
static int bc32_add_imm(int is_addr, i32 imm) {
    (void)is_addr;
    return bc_emit_add_imm(imm);
}
static size_t bc32_emit_br_fwd(BranchCond cond) { return bc_emit_br_fwd_op(cond, 4); }
static void bc32_emit_br_back(BranchCond cond, size_t target_addr) {
    bc_emit_br_back_op(cond, target_addr, 4);
}
static void bc32_patch_fwd(size_t patch_offset) { bc_patch_fwd_op(patch_offset, 4); }
static size_t bc32_emit_jtab(size_t count) { return bc_emit_jtab_op(count, 4); }
static void bc32_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    bc_patch_jtab_op(patch_offset, idx, target_addr, 4);
}
static void bc32_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    bc_patch_call_op(call_pc, target_addr, 4);
}
static size_t bc32_push_fn(size_t target_addr) { return bc_emit_push_fn_op(target_addr, 4); }
static void bc32_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    bc_patch_fn_op(patch_pc, target_addr, 4);
}

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
                       NULL,
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

const Backend be_bc32 = {"bc32",
                         4,
                         4,
                         4,
                         bc_cur_addr,
                         bc32_fn_begin,
                         bc32_fn_end,
                         bc32_call,
                         NULL,
                         bc_call_extern,
                         bc32_push_w,
                         bc32_push_a,
                         bc_push_f,
                         bc_push_i32,
                         bc32_push_data,
                         bc32_local_op,
                         NULL,
                         bc_stack_op,
                         bc32_op,
                         bc32_add_imm,
                         bc_cmp_imm,
                         bc_shift_imm,
                         NULL,
                         bc_mem_imm,
                         bc_data_mem,
                         bc_local_mem,
                         NULL,
                         bc32_emit_br_fwd,
                         bc32_emit_br_back,
                         NULL,
                         NULL,
                         bc32_patch_fwd,
                         bc32_emit_jtab,
                         bc32_patch_jtab,
                         bc32_patch_call,
                         bc32_push_fn,
                         bc32_patch_fn,
                         bc_icall,
                         bc_stack_data_off,
                         NULL};
