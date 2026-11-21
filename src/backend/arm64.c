/* be_arm64.c - Simplified ARM64 (AArch64) Binary Backend for IR4th (Strict C89)
 * Uniform 64-bit cells (W: and A: are both 8 bytes; no 8-bit stack items).
 * Stack & Frame Convention:
 *   - Evaluation stack: Hardware SP using 16-byte aligned slots (1 cell per slot)
 *   - Frame pointer: X29 (locals at [X29, #-8], [X29, #-16], ...)
 */
#include "codebuf.h"
#include "vec.h"

/* --- Native Hardware-SP Push/Pop (16-byte aligned slots) --- */
static void push_x0(void) { emit32(0xF81F0FE0UL); } /* STR X0, [SP, #-16]! */
static void push_x1(void) { emit32(0xF81F0FE1UL); } /* STR X1, [SP, #-16]! */
static void pop_x0(void) { emit32(0xF84107E0UL); }  /* LDR X0, [SP], #16   */
static void pop_x1(void) { emit32(0xF84107E1UL); }  /* LDR X1, [SP], #16   */
static void push_d0(void) { emit32(0xFC1F0FE0UL); } /* STR D0, [SP, #-16]! */
static void pop_d0(void) { emit32(0xFC4107E0UL); }  /* LDR D0, [SP], #16   */
static void pop_d1(void) { emit32(0xFC4107E1UL); }  /* LDR D1, [SP], #16   */

static void mov_x0_u32(u32 lo, u32 hi) {
    emit32(0xD2800000UL | ((lo & 0xFFFFUL) << 5));
    if ((lo >> 16) != 0 || hi != 0) {
        emit32(0xF2A00000UL | (((lo >> 16) & 0xFFFFUL) << 5));
    }
    if (hi != 0) {
        emit32(0xF2C00000UL | ((hi & 0xFFFFUL) << 5));
        emit32(0xF2E00000UL | (((hi >> 16) & 0xFFFFUL) << 5));
    }
}

static void mov_x0_imm(i32 val) { mov_x0_u32((u32)val, (val < 0) ? 0xFFFFFFFFUL : 0x00000000UL); }

/* --- Backend Interface --- */
static size_t a64_cur_addr(void) { return cb_cur_addr(); }

VEC_OF(U32Vec, u32);
static U32Vec a64_local_off;

static void a64_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                         size_t total_locals, Bool is_recursive) {
    size_t i;
    u32 cur_off = 0;
    u32 frame_bytes;
    (void)is_recursive;
    (void)name;

    VEC_ENSURE(a64_local_off, total_locals);
    for (i = 0; i < total_locals; i++) {
        u32 sz = (locals[i].size > 0) ? ((u32)(locals[i].size + 7) & ~7UL) : 8UL;
        cur_off += sz;
        a64_local_off.data[i] = cur_off;
    }
    frame_bytes = (cur_off + 15UL) & ~15UL;

    /* Note: Incoming parameters were pushed onto caller SP before BL.
     * Save FP/LR, set FP = SP, and copy incoming args from [X29, #16 + k*16] */
    emit32(0xA9BF7BFDUL); /* STP X29, X30, [SP, #-16]! */
    emit32(0x910003FDUL); /* MOV X29, SP */

    if (frame_bytes > 0) {
        if (frame_bytes <= 4095) {
            emit32(0xD10003FFUL | ((frame_bytes & 0xFFFUL) << 10)); /* SUB SP, SP, #frame */
        } else {
            mov_x0_u32(frame_bytes, 0);
            emit32(0xCB2063FFUL); /* SUB SP, SP, X0 */
        }
    }

    for (i = 0; i < param_cnt; i++) {
        u32 caller_off = (u32)(16 + (param_cnt - 1 - i) * 16);
        u32 local_off = a64_local_off.data[i];
        emit32(0xF94003A0UL | ((caller_off >> 3) << 10)); /* LDR X0, [X29, #caller_off] */
        if (local_off <= 255) {
            emit32(0xF80003A0UL |
                   (((0UL - local_off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-local_off] */
        } else {
            mov_x0_u32(local_off, 0);
            emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
            emit32(0xF94003A0UL | ((caller_off >> 3) << 10));
            emit32(0xF9000020UL); /* STR X0, [X1] */
        }
    }

    if (total_locals > param_cnt) {
        emit32(0xD2800000UL); /* MOV X0, #0 */
        for (i = param_cnt; i < total_locals; i++) {
            u32 local_off = a64_local_off.data[i];
            if (locals[i].size > 0)
                continue;
            if (local_off <= 255) {
                emit32(0xF80003A0UL | (((0UL - local_off) & 0x1FFUL) << 12));
            } else {
                mov_x0_u32(local_off, 0);
                emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
                emit32(0xD2800000UL); /* MOV X0, #0 */
                emit32(0xF9000020UL); /* STR X0, [X1] */
            }
        }
    }
}

static void a64_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
    size_t k;
    size_t param_cnt = sig->in_types.len;
    size_t out_count = sig->out_types.len;
    (void)locals;
    (void)total_locals;

    /* Save return values from evaluation SP into scratch X9..X12 */
    for (k = out_count; k > 0; k--) {
        emit32(0xF84107E0UL | (u32)(8 + k)); /* LDR X(8+k), [SP], #16 */
    }

    emit32(0x910003BFUL); /* MOV SP, X29 */
    emit32(0xA8C17BFDUL); /* LDP X29, X30, [SP], #16 */

    /* Pop incoming parameters pushed by caller */
    if (param_cnt > 0) {
        u32 arg_bytes = (u32)param_cnt * 16;
        emit32(0x910003FFUL | ((arg_bytes & 0xFFFUL) << 10)); /* ADD SP, SP, #arg_bytes */
    }

    /* Push return values onto caller's evaluation stack */
    for (k = 1; k <= out_count; k++) {
        emit32(0xF81F0FE0UL | (u32)(8 + k)); /* STR X(8+k), [SP, #-16]! */
    }

    emit32(0xD65F03C0UL); /* RET */
}

static void a64_call(size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    emit32(0x94000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
}

static void a64_call_extern(const FuncSig *sig) {
    u8 int_cnt = 0;
    u8 flt_cnt = 0;
    size_t i;

    if (sig->has_addr) {
        ir4_die("ARM64 extern does not take a fixed address");
    }

    for (i = 0; i < sig->in_types.len; i++) {
        if (sig->in_types.data[i] == TY_F)
            flt_cnt++;
        else
            int_cnt++;
    }
    if (int_cnt > 8 || flt_cnt > 8) {
        ir4_die("ARM64 extern supports at most 8 int and 8 float args");
    }

    for (i = sig->in_types.len; i > 0; i--) {
        if (sig->in_types.data[i - 1] == TY_F) {
            flt_cnt--;
            emit32(0xFC4107E0UL | (u32)flt_cnt); /* LDR D<k>, [SP], #16 */
        } else {
            int_cnt--;
            emit32(0xF84107E0UL | (u32)int_cnt); /* LDR X<k>, [SP], #16 */
        }
    }

    cb_add_extern_reloc(cb_pc(), sig->name);
    emit32(0xD2800009UL); /* MOVZ X9, #0 */
    emit32(0xF2A00009UL); /* MOVK X9, #0, LSL #16 */
    emit32(0xF2C00009UL); /* MOVK X9, #0, LSL #32 */
    emit32(0xF2E00009UL); /* MOVK X9, #0, LSL #48 */
    emit32(0xD63F0120UL); /* BLR X9 */

    if (sig->out_types.len == 1) {
        if (sig->out_types.data[0] == TY_F)
            push_d0();
        else
            push_x0();
    }
}

static void a64_push_w(i32 val) {
    mov_x0_imm(val);
    push_x0();
}
static void a64_push_a(u32 val) {
    mov_x0_u32(val, 0);
    push_x0();
}
static void a64_push_f(double val) {
    u8 bytes[8];
    u32 lo, hi;
    memcpy(bytes, &val, 8);
    lo = (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
    hi = (u32)bytes[4] | ((u32)bytes[5] << 8) | ((u32)bytes[6] << 16) | ((u32)bytes[7] << 24);
    mov_x0_u32(lo, hi);
    push_x0();
}

static void a64_push_data(size_t offset) {
    if (offset <= 4095) {
        emit32(0x91000380UL | (((u32)offset & 0xFFFUL) << 10)); /* ADD X0, X28, #offset */
    } else {
        mov_x0_u32((u32)offset, 0);
        emit32(0x8B000380UL); /* ADD X0, X28, X0 */
    }
    push_x0();
}

static void a64_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
    u32 off = a64_local_off.data[slot];
    (void)type;
    (void)stack_bytes;
    if (op == LOCAL_GET) {
        if (off <= 255) {
            emit32(0xF84003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* LDUR X0, [X29, #-off] */
        } else {
            mov_x0_u32(off, 0);
            emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
            emit32(0xF9400020UL); /* LDR X0, [X1] */
        }
        push_x0();
    } else if (op == LOCAL_ADDR) {
        if (off <= 4095) {
            emit32(0xD10003A0UL | ((off & 0xFFFUL) << 10)); /* SUB X0, X29, #off */
        } else {
            mov_x0_u32(off, 0);
            emit32(0xCB0003A0UL); /* SUB X0, X29, X0 */
        }
        push_x0();
    } else if (op == LOCAL_ADD) {
        pop_x0();
        if (off <= 255) {
            emit32(0xF84003A1UL | (((0UL - off) & 0x1FFUL) << 12)); /* LDUR X1, [X29, #-off] */
            emit32(0x8B000020UL);                                   /* ADD X0, X1, X0 */
            emit32(0xF80003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-off] */
        } else {
            mov_x0_u32(off, 1);
            emit32(0xCB0103A1UL); /* SUB X1, X29, X1 */
            emit32(0xF9400022UL); /* LDR X2, [X1] */
            emit32(0x8B000040UL); /* ADD X0, X2, X0 */
            emit32(0xF9000020UL); /* STR X0, [X1] */
        }
    } else {
        if (off <= 255) {
            pop_x0();
            emit32(0xF80003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-off] */
        } else {
            mov_x0_u32(off, 0);
            emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
            pop_x0();
            emit32(0xF9000020UL); /* STR X0, [X1] */
        }
    }
}

static void a64_stack_op(StackOp op, StackType t1, StackType t2) {
    (void)t1;
    (void)t2;
    switch (op) {
    case OP_DUP:
        emit32(0xF94003E0UL); /* LDR X0, [SP] */
        push_x0();
        break;
    case OP_DROP:
        emit32(0x910043FFUL); /* ADD SP, SP, #16 */
        break;
    case OP_SWAP:
        pop_x0();
        pop_x1();
        push_x0();
        push_x1();
        break;
    default:
        ir4_die("unimplemented ARM64 stack op");
    }
}

static void a64_int_alu(IrOp op) {
    pop_x1(); /* rhs */
    pop_x0(); /* lhs */

    switch (op) {
    case OP_ADD:
    case OP_AADD:
        emit32(0x8B010000UL); /* ADD X0, X0, X1 */
        break;
    case OP_ADD32:
        emit32(0x0B010000UL); /* ADD W0, W0, W1 */
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        emit32(0xCB010000UL); /* SUB X0, X0, X1 */
        break;
    case OP_SUB32:
        emit32(0x4B010000UL); /* SUB W0, W0, W1 */
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_MUL:
        emit32(0x9B017C00UL); /* MUL X0, X0, X1 */
        break;
    case OP_MUL32:
        emit32(0x1B017C00UL); /* MUL W0, W0, W1 */
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_DIV:
        emit32(0x9AC10C00UL); /* SDIV X0, X0, X1 */
        break;
    case OP_DIV32:
        emit32(0x1AC10C00UL); /* SDIV W0, W0, W1 */
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_UDIV:
        emit32(0x9AC10800UL); /* UDIV X0, X0, X1 */
        break;
    case OP_UDIV32:
        emit32(0x1AC10800UL); /* UDIV W0, W0, W1 */
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_MOD:
        emit32(0x9AC10C02UL);
        emit32(0x9B018040UL);
        break;
    case OP_MOD32:
        emit32(0x1AC10C02UL);
        emit32(0x1B018040UL);
        emit32(0x93407C00UL);
        break;
    case OP_UMOD:
        emit32(0x9AC10802UL);
        emit32(0x9B018040UL);
        break;
    case OP_UMOD32:
        emit32(0x1AC10802UL);
        emit32(0x1B018040UL);
        emit32(0x93407C00UL);
        break;
    case OP_AND:
        emit32(0x8A010000UL); /* AND X0, X0, X1 */
        break;
    case OP_AND32:
        emit32(0x0A010000UL);
        emit32(0x93407C00UL);
        break;
    case OP_OR:
        emit32(0xAA010000UL); /* ORR X0, X0, X1 */
        break;
    case OP_OR32:
        emit32(0x2A010000UL);
        emit32(0x93407C00UL);
        break;
    case OP_XOR:
        emit32(0xCA010000UL); /* EOR X0, X0, X1 */
        break;
    case OP_XOR32:
        emit32(0x4A010000UL);
        emit32(0x93407C00UL);
        break;
    case OP_LSHIFT:
        emit32(0x9AC12000UL); /* LSLV X0, X0, X1 */
        break;
    case OP_SHL32:
        emit32(0x1AC12000UL);
        emit32(0x93407C00UL);
        break;
    case OP_RSHIFT:
        emit32(0x9AC12400UL); /* LSRV X0, X0, X1 */
        break;
    case OP_RSHR32:
        emit32(0x1AC12400UL);
        emit32(0x93407C00UL);
        break;
    case OP_ARSHIFT:
        emit32(0x9AC12800UL); /* ASRV X0, X0, X1 */
        break;
    case OP_ASHR32:
        emit32(0x1AC12800UL);
        emit32(0x93407C00UL);
        break;
    default:
        ir4_die("unimplemented ARM64 ALU op");
    }

    push_x0();
}

static u32 a64_cmp_cond(IrOp op) {
    switch (op) {
    case OP_EQ:
    case OP_EQ32:
    case OP_AEQ:
        return 0x0;
    case OP_NE:
    case OP_NE32:
    case OP_ANE:
        return 0x1;
    case OP_LT:
    case OP_LT32:
        return 0xB;
    case OP_GE:
    case OP_GE32:
        return 0xA;
    case OP_GT:
    case OP_GT32:
        return 0xC;
    case OP_LE:
    case OP_LE32:
        return 0xD;
    case OP_ULT:
    case OP_ULT32:
    case OP_ALT:
        return 0x3;
    case OP_UGE:
    case OP_UGE32:
        return 0x2;
    case OP_UGT:
    case OP_UGT32:
    case OP_AGT:
        return 0x8;
    case OP_ULE:
    case OP_ULE32:
        return 0x9;
    default:
        ir4_die("unimplemented ARM64 comparison op");
        return 0;
    }
}

static void a64_int_cmp(IrOp op, int is32) {
    u32 cond = a64_cmp_cond(op);
    pop_x1();
    pop_x0();
    emit32(is32 ? 0x6B01001FUL : 0xEB01001FUL);  /* CMP W0,W1 / X0,X1 */
    emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
    push_x0();
}

static int a64_mem_imm(IrOp op, i32 off) {
    u32 sz, unscaled_op, scaled_op;
    int is_store = 0;
    int swapped = 0;

    switch (op) {
    case OP_U8FETCH:
        sz = 1;
        unscaled_op = 0x38400000UL;
        scaled_op = 0x39400000UL;
        break;
    case OP_I8FETCH:
        sz = 1;
        unscaled_op = 0x38800000UL;
        scaled_op = 0x39800000UL;
        break;
    case OP_SI8STORE:
        swapped = 1; /* fall through */
    case OP_I8STORE:
        sz = 1;
        unscaled_op = 0x38000001UL;
        scaled_op = 0x39000001UL;
        is_store = 1;
        break;
    case OP_U16FETCH:
        sz = 2;
        unscaled_op = 0x78400000UL;
        scaled_op = 0x79400000UL;
        break;
    case OP_I16FETCH:
        sz = 2;
        unscaled_op = 0x78800000UL;
        scaled_op = 0x79800000UL;
        break;
    case OP_SI16STORE:
        swapped = 1; /* fall through */
    case OP_I16STORE:
        sz = 2;
        unscaled_op = 0x78000001UL;
        scaled_op = 0x79000001UL;
        is_store = 1;
        break;
    case OP_I32FETCH:
        sz = 4;
        unscaled_op = 0xB8800000UL;
        scaled_op = 0xB9800000UL;
        break;
    case OP_I32STORE:
        sz = 4;
        unscaled_op = 0xB8000001UL;
        scaled_op = 0xB9000001UL;
        is_store = 1;
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_FFETCH:
        sz = 8;
        unscaled_op = 0xF8400000UL;
        scaled_op = 0xF9400000UL;
        break;
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SFSTORE:
        swapped = 1; /* fall through */
    case OP_STORE:
    case OP_ASTORE:
    case OP_FSTORE:
        sz = 8;
        unscaled_op = 0xF8000001UL;
        scaled_op = 0xF9000001UL;
        is_store = 1;
        break;
    default:
        return 0;
    }

    if (off >= 0 && ((u32)off & (sz - 1)) == 0 && ((u32)off / sz) <= 4095) {
        if (is_store) {
            if (swapped) {
                pop_x1();
                pop_x0();
            } else {
                pop_x0();
                pop_x1();
            }
        } else {
            pop_x0();
        }
        emit32(scaled_op | (((u32)off / sz) << 10));
        if (!is_store)
            push_x0();
        return 1;
    }
    if (off >= -256 && off <= 255) {
        if (is_store) {
            if (swapped) {
                pop_x1();
                pop_x0();
            } else {
                pop_x0();
                pop_x1();
            }
        } else {
            pop_x0();
        }
        emit32(unscaled_op | (((u32)off & 0x1FFUL) << 12));
        if (!is_store)
            push_x0();
        return 1;
    }
    return 0;
}

static void a64_op(IrOp op) {
    switch (op) {
    case OP_ADD:
    case OP_AADD:
    case OP_ADD32:
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
    case OP_SUB32:
    case OP_MUL:
    case OP_DIV:
    case OP_UDIV:
    case OP_MOD:
    case OP_UMOD:
    case OP_MUL32:
    case OP_DIV32:
    case OP_UDIV32:
    case OP_MOD32:
    case OP_UMOD32:
    case OP_AND:
    case OP_OR:
    case OP_XOR:
    case OP_AND32:
    case OP_OR32:
    case OP_XOR32:
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
    case OP_SHL32:
    case OP_RSHR32:
    case OP_ASHR32:
        a64_int_alu(op);
        break;
    case OP_INVERT:
        pop_x0();
        emit32(0xAA2003E0UL); /* MVN X0, X0 */
        push_x0();
        break;
    case OP_INV32:
        pop_x0();
        emit32(0x2A2003E0UL); /* MVN W0, W0 */
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        push_x0();
        break;
    case OP_FNEG:
        pop_d0();
        emit32(0x1E614000UL); /* FNEG D0, D0 */
        push_d0();
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
        pop_d1(); /* rhs */
        pop_d0(); /* lhs */
        if (op == OP_FADD)
            emit32(0x1E612800UL); /* FADD D0, D0, D1 */
        else if (op == OP_FSUB)
            emit32(0x1E613800UL); /* FSUB D0, D0, D1 */
        else if (op == OP_FMUL)
            emit32(0x1E610800UL); /* FMUL D0, D0, D1 */
        else
            emit32(0x1E611800UL); /* FDIV D0, D0, D1 */
        push_d0();
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
        a64_int_cmp(op, 0);
        break;
    case OP_EQ32:
    case OP_NE32:
    case OP_LT32:
    case OP_GE32:
    case OP_GT32:
    case OP_LE32:
    case OP_ULT32:
    case OP_UGE32:
    case OP_UGT32:
    case OP_ULE32:
        a64_int_cmp(op, 1);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT: {
        u32 cond = (op == OP_FEQ) ? 0x0 : (op == OP_FNE) ? 0x1 : (op == OP_FLT) ? 0x4 : 0xC;
        pop_d1();
        pop_d0();
        emit32(0x1E612000UL);                        /* FCMP D0, D1 */
        emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
        push_x0();
        break;
    }
    case OP_U8FETCH:
    case OP_I8FETCH:
    case OP_I8STORE:
    case OP_U16FETCH:
    case OP_I16FETCH:
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
        a64_mem_imm(op, 0);
        break;
    case OP_BCOPY:
        emit32(0xF84107E2UL); /* LDR X2, [SP], #16 (len) */
        pop_x1();             /* src */
        pop_x0();             /* dst */
        emit32(0xB40000A2UL); /* CBZ X2, +20 */
        emit32(0x38401423UL); /* LDRB W3, [X1], #1 */
        emit32(0x38001403UL); /* STRB W3, [X0], #1 */
        emit32(0xF1000442UL); /* SUBS X2, X2, #1 */
        emit32(0x54FFFFA1UL); /* B.NE -12 */
        break;
    case OP_SEXT:
        pop_x0();
        emit32(0x93401C00UL); /* SXTB X0, X0 */
        push_x0();
        break;
    case OP_W2A:
    case OP_A2W:
        /* Uniform 64-bit cells: zero-cost no-op */
        break;
    case OP_W2F:
        pop_x0();
        emit32(0x9E620000UL); /* SCVTF D0, X0 */
        push_d0();
        break;
    case OP_F2W:
        pop_d0();
        emit32(0x9E780000UL); /* FCVTZS X0, D0 */
        push_x0();
        break;
    case OP_W2I32:
    case OP_I322W:
        pop_x0();
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        push_x0();
        break;
    case OP_U2I32:
        pop_x0();
        emit32(0x2A0003E0UL); /* MOV W0, W0 (zero-extend 32->64) */
        push_x0();
        break;
    }
}

static size_t a64_emit_br_fwd(BranchCond cond) {
    u16 pos;
    if (cond == BR_JZ) {
        pop_x0();
        pos = cb_pc();
        emit32(0xB4000000UL); /* CBZ X0, rel19 */
    } else {
        pos = cb_pc();
        emit32(0x14000000UL); /* B rel26 */
    }
    return pos;
}

static void a64_emit_br_back(BranchCond cond, size_t target_addr) {
    i32 diff;
    if (cond == BR_JZ) {
        u32 imm19;
        pop_x0();
        diff = (i32)target_addr - (i32)cb_cur_addr();
        imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        emit32(0xB4000000UL | (imm19 << 5)); /* CBZ X0, rel19 */
    } else {
        diff = (i32)target_addr - (i32)cb_cur_addr();
        emit32(0x14000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
    }
}

static int a64_cmp_br_emit(IrOp op, int has_imm, i32 imm, int is_fwd, size_t target_addr,
                           size_t *patch_offset) {
    u32 cond;
    if (op < OP_EQ || op > OP_AGT)
        return 0;
    if (has_imm && (imm < 0 || imm > 4095))
        return 0;
    cond = a64_cmp_cond(op);
    if (has_imm) {
        pop_x0();
        emit32(0xF100001FUL | ((u32)imm << 10)); /* CMP X0, #imm */
    } else {
        pop_x1();
        pop_x0();
        emit32(0xEB01001FUL); /* CMP X0, X1 */
    }
    if (is_fwd) {
        *patch_offset = cb_pc();
        emit32(0x54000000UL | (cond ^ 1UL)); /* B.(cond^1) rel19 */
    } else {
        i32 diff = (i32)target_addr - (i32)cb_cur_addr();
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        emit32(0x54000000UL | (imm19 << 5) | (cond ^ 1UL));
    }
    return 1;
}

static int a64_cmp_br_fwd(IrOp op, int has_imm, i32 imm, size_t *patch_offset) {
    return a64_cmp_br_emit(op, has_imm, imm, 1, 0, patch_offset);
}

static int a64_cmp_br_back(IrOp op, int has_imm, i32 imm, size_t target_addr) {
    return a64_cmp_br_emit(op, has_imm, imm, 0, target_addr, NULL);
}

static void a64_patch_fwd(size_t patch_offset) {
    i32 diff = (i32)cb_pc() - (i32)patch_offset;
    u8 top = cb_data()[patch_offset + 3];
    if (top == 0xB4) {
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        patch32(patch_offset, 0xB4000000UL | (imm19 << 5));
    } else if (top == 0x54) {
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        u32 cond_bits = (u32)(cb_data()[patch_offset] & 0x0F);
        patch32(patch_offset, 0x54000000UL | (imm19 << 5) | cond_bits);
    } else {
        u32 imm26 = ((u32)(diff >> 2)) & 0x03FFFFFFUL;
        patch32(patch_offset, 0x14000000UL | imm26);
    }
}

static int a64_add_imm(i32 imm) {
    if (imm == 0)
        return 1;
    if (imm > 0 && imm <= 4095) {
        emit32(0xF94003E0UL);                    /* LDR X0, [SP] */
        emit32(0x91000000UL | ((u32)imm << 10)); /* ADD X0, X0, #imm */
        emit32(0xF90003E0UL);                    /* STR X0, [SP] */
        return 1;
    }
    if (imm < 0 && imm >= -4095) {
        emit32(0xF94003E0UL);                       /* LDR X0, [SP] */
        emit32(0xD1000000UL | ((u32)(-imm) << 10)); /* SUB X0, X0, #(-imm) */
        emit32(0xF90003E0UL);                       /* STR X0, [SP] */
        return 1;
    }
    return 0;
}

static int a64_cmp_imm(IrOp op, i32 imm) {
    u32 cond;
    if (imm < 0 || imm > 4095)
        return 0;
    cond = a64_cmp_cond(op);
    emit32(0xF94003E0UL);                        /* LDR X0, [SP] */
    emit32(0xF100001FUL | ((u32)imm << 10));     /* CMP X0, #imm */
    emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
    emit32(0xF90003E0UL);                        /* STR X0, [SP] */
    return 1;
}

static int a64_shift_imm(IrOp op, u8 count) {
    u32 c = (u32)count;
    if (c == 0)
        return 1;
    emit32(0xF94003E0UL); /* LDR X0, [SP] */
    switch (op) {
    case OP_LSHIFT:
        emit32(0xD3400000UL | ((64 - c) << 16) | ((63 - c) << 10));
        break;
    case OP_RSHIFT:
        emit32(0xD340FC00UL | (c << 16));
        break;
    case OP_ARSHIFT:
        emit32(0x9340FC00UL | (c << 16));
        break;
    case OP_SHL32:
        emit32(0x53000000UL | ((32 - c) << 16) | ((31 - c) << 10));
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_RSHR32:
        emit32(0x53007C00UL | (c << 16));
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_ASHR32:
        emit32(0x13007C00UL | (c << 16));
        emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    default:
        return 0;
    }
    emit32(0xF90003E0UL); /* STR X0, [SP] */
    return 1;
}

static void a64_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)call_addr;
    patch32(call_pc, 0x94000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
}

static size_t a64_push_fn(size_t target_addr) {
    size_t pc = cb_pc();
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    u32 imm = (u32)diff;
    emit32(0x10000000UL | ((imm & 0x3UL) << 29) |
           (((imm >> 2) & 0x7FFFFUL) << 5)); /* ADR X0, diff */
    push_x0();
    return pc;
}

static void a64_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)patch_addr;
    u32 imm = (u32)diff;
    patch32(patch_pc, 0x10000000UL | ((imm & 0x3UL) << 29) | (((imm >> 2) & 0x7FFFFUL) << 5));
}

static void a64_icall(void) {
    pop_x0();
    emit32(0xD63F0000UL); /* BLR X0 */
}

const Backend be_arm64 = {"arm64",
                          8,
                          8,
                          8,
                          a64_cur_addr,
                          a64_fn_begin,
                          a64_fn_end,
                          a64_call,
                          a64_call_extern,
                          a64_push_w,
                          a64_push_a,
                          a64_push_f,
                          a64_push_w,
                          a64_push_data,
                          a64_local_op,
                          NULL,
                          a64_stack_op,
                          a64_op,
                          a64_add_imm,
                          a64_cmp_imm,
                          a64_shift_imm,
                          a64_mem_imm,
                          NULL,
                          NULL,
                          a64_emit_br_fwd,
                          a64_emit_br_back,
                          a64_cmp_br_fwd,
                          a64_cmp_br_back,
                          a64_patch_fwd,
                          a64_patch_call,
                          a64_push_fn,
                          a64_patch_fn,
                          a64_icall,
                          NULL};
