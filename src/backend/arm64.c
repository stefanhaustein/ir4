/* arm64.c - Simplified ARM64 (AArch64) Binary Backend for IR4th
 * Uniform 64-bit cells (W: and A: are both 8 bytes; no 8-bit stack items).
 * Stack & Frame Convention:
 *   - Evaluation stack: Hardware SP using 16-byte aligned slots (1 cell per slot)
 *   - Frame pointer: X29 (locals at [X29, #-8], [X29, #-16], ...)
 */
#include "codebuf.h"
#include "backend/reg_lower.h"

/* --- Native Hardware-SP Push/Pop (16-byte aligned slots) --- */
static void push_x0(void) { cb_emit32(0xF81F0FE0UL); } /* STR X0, [SP, #-16]! */
static void push_x1(void) { cb_emit32(0xF81F0FE1UL); } /* STR X1, [SP, #-16]! */
static void pop_x0(void) { cb_emit32(0xF84107E0UL); }  /* LDR X0, [SP], #16   */
static void pop_x1(void) { cb_emit32(0xF84107E1UL); }  /* LDR X1, [SP], #16   */
static void push_d0(void) { cb_emit32(0xFC1F0FE0UL); } /* STR D0, [SP, #-16]! */
static void pop_d0(void) { cb_emit32(0xFC4107E0UL); }  /* LDR D0, [SP], #16   */
static void pop_d1(void) { cb_emit32(0xFC4107E1UL); }  /* LDR D1, [SP], #16   */

static void mov_x0_u32(u32 lo, u32 hi) {
    cb_emit32(0xD2800000UL | ((lo & 0xFFFFUL) << 5));
    if ((lo >> 16) != 0 || hi != 0) {
        cb_emit32(0xF2A00000UL | (((lo >> 16) & 0xFFFFUL) << 5));
    }
    if (hi != 0) {
        cb_emit32(0xF2C00000UL | ((hi & 0xFFFFUL) << 5));
        cb_emit32(0xF2E00000UL | (((hi >> 16) & 0xFFFFUL) << 5));
    }
}

static void mov_x0_imm(i32 val) { mov_x0_u32((u32)val, (val < 0) ? 0xFFFFFFFFUL : 0x00000000UL); }

/* --- Backend Interface --- */
static size_t a64_cur_addr(void) { return cb_cur_addr(); }

VEC_OF(U32Vec, u32);
static U32Vec a64_local_off;

static void a64_fn_begin(const char *name, int param_cnt, const LocalVar *locals,
                         int total_locals, Bool is_recursive) {
    int i;
    u32 cur_off = 0;
    u32 frame_bytes;
    (void)is_recursive;
    (void)name;

    VEC_ENSURE(a64_local_off, (size_t)total_locals);
    for (i = 0; i < total_locals; i++) {
        u32 sz = (locals[i].size > 0) ? ((u32)(locals[i].size + 7) & ~7UL) : 8UL;
        cur_off += sz;
        a64_local_off.data[i] = cur_off;
    }
    frame_bytes = (cur_off + 15UL) & ~15UL;

    /* Note: Incoming parameters were pushed onto caller SP before BL.
     * Save FP/LR, set FP = SP, and copy incoming args from [X29, #16 + k*16] */
    cb_emit32(0xA9BF7BFDUL); /* STP X29, X30, [SP, #-16]! */
    cb_emit32(0x910003FDUL); /* MOV X29, SP */

    if (frame_bytes > 0) {
        if (frame_bytes <= 4095) {
            cb_emit32(0xD10003FFUL | ((frame_bytes & 0xFFFUL) << 10)); /* SUB SP, SP, #frame */
        } else {
            mov_x0_u32(frame_bytes, 0);
            cb_emit32(0xCB2063FFUL); /* SUB SP, SP, X0 */
        }
    }

    for (i = 0; i < param_cnt; i++) {
        u32 caller_off = (u32)(16 + (param_cnt - 1 - i) * 16);
        u32 local_off = a64_local_off.data[i];
        cb_emit32(0xF94003A0UL | ((caller_off >> 3) << 10)); /* LDR X0, [X29, #caller_off] */
        if (local_off <= 255) {
            cb_emit32(0xF80003A0UL |
                   (((0UL - local_off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-local_off] */
        } else {
            mov_x0_u32(local_off, 0);
            cb_emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
            cb_emit32(0xF94003A0UL | ((caller_off >> 3) << 10));
            cb_emit32(0xF9000020UL); /* STR X0, [X1] */
        }
    }

    if (total_locals > param_cnt) {
        cb_emit32(0xD2800000UL); /* MOV X0, #0 */
        for (i = param_cnt; i < total_locals; i++) {
            u32 local_off = a64_local_off.data[i];
            if (locals[i].size > 0)
                continue;
            if (local_off <= 255) {
                cb_emit32(0xF80003A0UL | (((0UL - local_off) & 0x1FFUL) << 12));
            } else {
                mov_x0_u32(local_off, 0);
                cb_emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
                cb_emit32(0xD2800000UL); /* MOV X0, #0 */
                cb_emit32(0xF9000020UL); /* STR X0, [X1] */
            }
        }
    }
}

typedef char ir4_arm64_max_outs_check[(IR4_MAX_OUTS <= 4) ? 1 : -1];

static void a64_fn_end(const FuncSig *sig, const LocalVar *locals, int total_locals) {
    size_t k;
    size_t param_cnt = sig->in_types.len;
    size_t out_count = sig->out_types.len;
    (void)locals;
    (void)total_locals;

    if (out_count > 4)
        ir4_die("ARM64 return values exceed 4");

    /* Save return values from evaluation SP into scratch X9..X12 */
    for (k = out_count; k > 0; k--) {
        cb_emit32(0xF84107E0UL | (u32)(8 + k)); /* LDR X(8+k), [SP], #16 */
    }

    cb_emit32(0x910003BFUL); /* MOV SP, X29 */
    cb_emit32(0xA8C17BFDUL); /* LDP X29, X30, [SP], #16 */

    /* Pop incoming parameters pushed by caller */
    if (param_cnt > 0) {
        u32 arg_bytes = (u32)param_cnt * 16;
        cb_emit32(0x910003FFUL | ((arg_bytes & 0xFFFUL) << 10)); /* ADD SP, SP, #arg_bytes */
    }

    /* Push return values onto caller's evaluation stack */
    for (k = 1; k <= out_count; k++) {
        cb_emit32(0xF81F0FE0UL | (u32)(8 + k)); /* STR X(8+k), [SP, #-16]! */
    }

    cb_emit32(0xD65F03C0UL); /* RET */
}

static void a64_call(size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    cb_emit32(0x94000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
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
            cb_emit32(0xFC4107E0UL | (u32)flt_cnt); /* LDR D<k>, [SP], #16 */
        } else {
            int_cnt--;
            cb_emit32(0xF84107E0UL | (u32)int_cnt); /* LDR X<k>, [SP], #16 */
        }
    }

    cb_add_extern_reloc(cb_pc(), sig->name);
    cb_emit32(0xD2800009UL); /* MOVZ X9, #0 */
    cb_emit32(0xF2A00009UL); /* MOVK X9, #0, LSL #16 */
    cb_emit32(0xF2C00009UL); /* MOVK X9, #0, LSL #32 */
    cb_emit32(0xF2E00009UL); /* MOVK X9, #0, LSL #48 */
    cb_emit32(0xD63F0120UL); /* BLR X9 */

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
        cb_emit32(0x91000380UL | (((u32)offset & 0xFFFUL) << 10)); /* ADD X0, X28, #offset */
    } else {
        mov_x0_u32((u32)offset, 0);
        cb_emit32(0x8B000380UL); /* ADD X0, X28, X0 */
    }
    push_x0();
}

static void a64_local_op(LocalOp op, int slot, StackType type, size_t stack_bytes) {
    u32 off = a64_local_off.data[slot];
    (void)type;
    (void)stack_bytes;
    if (op == LOCAL_GET) {
        if (off <= 255) {
            cb_emit32(0xF84003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* LDUR X0, [X29, #-off] */
        } else {
            mov_x0_u32(off, 0);
            cb_emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
            cb_emit32(0xF9400020UL); /* LDR X0, [X1] */
        }
        push_x0();
    } else if (op == LOCAL_ADDR) {
        if (off <= 4095) {
            cb_emit32(0xD10003A0UL | ((off & 0xFFFUL) << 10)); /* SUB X0, X29, #off */
        } else {
            mov_x0_u32(off, 0);
            cb_emit32(0xCB0003A0UL); /* SUB X0, X29, X0 */
        }
        push_x0();
    } else if (op == LOCAL_ADD) {
        pop_x0();
        if (off <= 255) {
            cb_emit32(0xF84003A1UL | (((0UL - off) & 0x1FFUL) << 12)); /* LDUR X1, [X29, #-off] */
            cb_emit32(0x8B000020UL);                                   /* ADD X0, X1, X0 */
            cb_emit32(0xF80003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-off] */
        } else {
            cb_emit32(0xD2800001UL | ((off & 0xFFFFUL) << 5));
            if ((off >> 16) != 0) {
                cb_emit32(0xF2A00001UL | (((off >> 16) & 0xFFFFUL) << 5));
            }
            cb_emit32(0xCB0103A1UL); /* SUB X1, X29, X1 */
            cb_emit32(0xF9400022UL); /* LDR X2, [X1] */
            cb_emit32(0x8B000040UL); /* ADD X0, X2, X0 */
            cb_emit32(0xF9000020UL); /* STR X0, [X1] */
        }
    } else {
        if (off <= 255) {
            pop_x0();
            cb_emit32(0xF80003A0UL | (((0UL - off) & 0x1FFUL) << 12)); /* STUR X0, [X29, #-off] */
        } else {
            mov_x0_u32(off, 0);
            cb_emit32(0xCB0003A1UL); /* SUB X1, X29, X0 */
            pop_x0();
            cb_emit32(0xF9000020UL); /* STR X0, [X1] */
        }
    }
}

static void a64_stack_op(StackOp op, StackType t1, StackType t2) {
    (void)t1;
    (void)t2;
    switch (op) {
    case OP_DUP:
        cb_emit32(0xF94003E0UL); /* LDR X0, [SP] */
        push_x0();
        break;
    case OP_DROP:
        cb_emit32(0x910043FFUL); /* ADD SP, SP, #16 */
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
        cb_emit32(0x8B010000UL); /* ADD X0, X0, X1 */
        break;
    case OP_ADD32:
        cb_emit32(0x0B010000UL); /* ADD W0, W0, W1 */
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit32(0xCB010000UL); /* SUB X0, X0, X1 */
        break;
    case OP_SUB32:
        cb_emit32(0x4B010000UL); /* SUB W0, W0, W1 */
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_MUL:
        cb_emit32(0x9B017C00UL); /* MUL X0, X0, X1 */
        break;
    case OP_MUL32:
        cb_emit32(0x1B017C00UL); /* MUL W0, W0, W1 */
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_DIV:
        cb_emit32(0x9AC10C00UL); /* SDIV X0, X0, X1 */
        break;
    case OP_DIV32:
        cb_emit32(0x1AC10C00UL); /* SDIV W0, W0, W1 */
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_UDIV:
        cb_emit32(0x9AC10800UL); /* UDIV X0, X0, X1 */
        break;
    case OP_UDIV32:
        cb_emit32(0x1AC10800UL); /* UDIV W0, W0, W1 */
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_MOD:
        cb_emit32(0x9AC10C02UL);
        cb_emit32(0x9B018040UL);
        break;
    case OP_MOD32:
        cb_emit32(0x1AC10C02UL);
        cb_emit32(0x1B018040UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_UMOD:
        cb_emit32(0x9AC10802UL);
        cb_emit32(0x9B018040UL);
        break;
    case OP_UMOD32:
        cb_emit32(0x1AC10802UL);
        cb_emit32(0x1B018040UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_AND:
        cb_emit32(0x8A010000UL); /* AND X0, X0, X1 */
        break;
    case OP_AND32:
        cb_emit32(0x0A010000UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_OR:
        cb_emit32(0xAA010000UL); /* ORR X0, X0, X1 */
        break;
    case OP_OR32:
        cb_emit32(0x2A010000UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_XOR:
        cb_emit32(0xCA010000UL); /* EOR X0, X0, X1 */
        break;
    case OP_XOR32:
        cb_emit32(0x4A010000UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_LSHIFT:
        cb_emit32(0x9AC12000UL); /* LSLV X0, X0, X1 */
        break;
    case OP_SHL32:
        cb_emit32(0x1AC12000UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_RSHIFT:
        cb_emit32(0x9AC12400UL); /* LSRV X0, X0, X1 */
        break;
    case OP_RSHR32:
        cb_emit32(0x1AC12400UL);
        cb_emit32(0x93407C00UL);
        break;
    case OP_ARSHIFT:
        cb_emit32(0x9AC12800UL); /* ASRV X0, X0, X1 */
        break;
    case OP_ASHR32:
        cb_emit32(0x1AC12800UL);
        cb_emit32(0x93407C00UL);
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
    cb_emit32(is32 ? 0x6B01001FUL : 0xEB01001FUL);  /* CMP W0,W1 / X0,X1 */
    cb_emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
    push_x0();
}

static int a64_bit_imm(IrOp op, i32 imm) {
    u32 alu;
    if (imm < 0 || imm > 65535)
        return 0;
    if (op == OP_AND)
        alu = 0x8A010000UL;
    else if (op == OP_OR)
        alu = 0xAA010000UL;
    else if (op == OP_XOR)
        alu = 0xCA010000UL;
    else
        return 0;
    cb_emit32(0xF94003E0UL);                        /* LDR X0, [SP] */
    cb_emit32(0xD2800001UL | ((u32)imm << 5));      /* MOVZ X1, #imm */
    cb_emit32(alu);                                 /* AND/ORR/EOR X0, X0, X1 */
    cb_emit32(0xF90003E0UL);                        /* STR X0, [SP] */
    return 1;
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
        cb_emit32(scaled_op | (((u32)off / sz) << 10));
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
        cb_emit32(unscaled_op | (((u32)off & 0x1FFUL) << 12));
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
        cb_emit32(0xAA2003E0UL); /* MVN X0, X0 */
        push_x0();
        break;
    case OP_INV32:
        pop_x0();
        cb_emit32(0x2A2003E0UL); /* MVN W0, W0 */
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        push_x0();
        break;
    case OP_FNEG:
        pop_d0();
        cb_emit32(0x1E614000UL); /* FNEG D0, D0 */
        push_d0();
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV:
        pop_d1(); /* rhs */
        pop_d0(); /* lhs */
        if (op == OP_FADD)
            cb_emit32(0x1E612800UL); /* FADD D0, D0, D1 */
        else if (op == OP_FSUB)
            cb_emit32(0x1E613800UL); /* FSUB D0, D0, D1 */
        else if (op == OP_FMUL)
            cb_emit32(0x1E610800UL); /* FMUL D0, D0, D1 */
        else
            cb_emit32(0x1E611800UL); /* FDIV D0, D0, D1 */
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
        cb_emit32(0x1E612000UL);                        /* FCMP D0, D1 */
        cb_emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
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
        cb_emit32(0xF84107E2UL); /* LDR X2, [SP], #16 (len) */
        pop_x1();             /* src */
        pop_x0();             /* dst */
        cb_emit32(0xB40000A2UL); /* CBZ X2, +20 */
        cb_emit32(0x38401423UL); /* LDRB W3, [X1], #1 */
        cb_emit32(0x38001403UL); /* STRB W3, [X0], #1 */
        cb_emit32(0xF1000442UL); /* SUBS X2, X2, #1 */
        cb_emit32(0x54FFFFA1UL); /* B.NE -12 */
        break;
    case OP_SEXT:
        pop_x0();
        cb_emit32(0x93401C00UL); /* SXTB X0, X0 */
        push_x0();
        break;
    case OP_W2A:
    case OP_A2W:
        /* Uniform 64-bit cells: zero-cost no-op */
        break;
    case OP_W2F:
        pop_x0();
        cb_emit32(0x9E620000UL); /* SCVTF D0, X0 */
        push_d0();
        break;
    case OP_F2W:
        pop_d0();
        cb_emit32(0x9E780000UL); /* FCVTZS X0, D0 */
        push_x0();
        break;
    case OP_W2I32:
    case OP_I322W:
        pop_x0();
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        push_x0();
        break;
    case OP_U2I32:
        pop_x0();
        cb_emit32(0x2A0003E0UL); /* MOV W0, W0 (zero-extend 32->64) */
        push_x0();
        break;
    }
}

static size_t a64_emit_br_fwd(BranchCond cond) {
    size_t pos;
    if (cond == BR_JZ) {
        pop_x0();
        pos = cb_pc();
        cb_emit32(0xB4000000UL); /* CBZ X0, rel19 */
    } else {
        pos = cb_pc();
        cb_emit32(0x14000000UL); /* B rel26 */
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
        cb_emit32(0xB4000000UL | (imm19 << 5)); /* CBZ X0, rel19 */
    } else {
        diff = (i32)target_addr - (i32)cb_cur_addr();
        cb_emit32(0x14000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
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
        cb_emit32(0xF100001FUL | ((u32)imm << 10)); /* CMP X0, #imm */
    } else {
        pop_x1();
        pop_x0();
        cb_emit32(0xEB01001FUL); /* CMP X0, X1 */
    }
    if (is_fwd) {
        *patch_offset = cb_pc();
        cb_emit32(0x54000000UL | (cond ^ 1UL)); /* B.(cond^1) rel19 */
    } else {
        i32 diff = (i32)target_addr - (i32)cb_cur_addr();
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        cb_emit32(0x54000000UL | (imm19 << 5) | (cond ^ 1UL));
    }
    return 1;
}

static int a64_cmp_br_fwd(IrOp op, int has_imm, i32 imm, size_t *patch_offset) {
    return a64_cmp_br_emit(op, has_imm, imm, 1, 0, patch_offset);
}

static int a64_cmp_br_back(IrOp op, int has_imm, i32 imm, size_t target_addr) {
    return a64_cmp_br_emit(op, has_imm, imm, 0, target_addr, NULL);
}

static size_t a64_emit_jtab(size_t count) {
    size_t tbl_off, k;
    pop_x0();
    cb_emit32(0x10000081UL); /* ADR X1, +16 */
    cb_emit32(0xB8A07820UL); /* LDRSW X0, [X1, X0, LSL #2] */
    cb_emit32(0x8B000020UL); /* ADD X0, X1, X0 */
    cb_emit32(0xD61F0000UL); /* BR X0 */
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit32(0);
    return tbl_off;
}

static void a64_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    cb_patch32(patch_offset + idx * 4, (u32)((i32)target_addr - (i32)patch_offset));
}

static void a64_patch_fwd(size_t patch_offset) {
    i32 diff = (i32)cb_pc() - (i32)patch_offset;
    u8 top = cb_data()[patch_offset + 3];
    if (top == 0xB4) {
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        u32 rt = (u32)(cb_data()[patch_offset] & 0x1FUL);
        cb_patch32(patch_offset, 0xB4000000UL | (imm19 << 5) | rt);
    } else if (top == 0x54) {
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        u32 cond_bits = (u32)(cb_data()[patch_offset] & 0x0F);
        cb_patch32(patch_offset, 0x54000000UL | (imm19 << 5) | cond_bits);
    } else {
        u32 imm26 = ((u32)(diff >> 2)) & 0x03FFFFFFUL;
        cb_patch32(patch_offset, 0x14000000UL | imm26);
    }
}

static int a64_add_imm(int is_addr, i32 imm) {
    (void)is_addr;
    if (imm == 0)
        return 1;
    if (imm > 0 && imm <= 4095) {
        cb_emit32(0xF94003E0UL);                    /* LDR X0, [SP] */
        cb_emit32(0x91000000UL | ((u32)imm << 10)); /* ADD X0, X0, #imm */
        cb_emit32(0xF90003E0UL);                    /* STR X0, [SP] */
        return 1;
    }
    if (imm < 0 && imm >= -4095) {
        cb_emit32(0xF94003E0UL);                       /* LDR X0, [SP] */
        cb_emit32(0xD1000000UL | ((u32)(-imm) << 10)); /* SUB X0, X0, #(-imm) */
        cb_emit32(0xF90003E0UL);                       /* STR X0, [SP] */
        return 1;
    }
    return 0;
}

static int a64_cmp_imm(IrOp op, i32 imm) {
    u32 cond;
    if (imm < 0 || imm > 4095)
        return 0;
    cond = a64_cmp_cond(op);
    cb_emit32(0xF94003E0UL);                        /* LDR X0, [SP] */
    cb_emit32(0xF100001FUL | ((u32)imm << 10));     /* CMP X0, #imm */
    cb_emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12)); /* CSETM X0, cond */
    cb_emit32(0xF90003E0UL);                        /* STR X0, [SP] */
    return 1;
}

static int a64_shift_imm(IrOp op, u8 count) {
    u32 c = (u32)count;
    if (c == 0)
        return 1;
    cb_emit32(0xF94003E0UL); /* LDR X0, [SP] */
    switch (op) {
    case OP_LSHIFT:
        cb_emit32(0xD3400000UL | ((64 - c) << 16) | ((63 - c) << 10));
        break;
    case OP_RSHIFT:
        cb_emit32(0xD340FC00UL | (c << 16));
        break;
    case OP_ARSHIFT:
        cb_emit32(0x9340FC00UL | (c << 16));
        break;
    case OP_SHL32:
        cb_emit32(0x53000000UL | ((32 - c) << 16) | ((31 - c) << 10));
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_RSHR32:
        cb_emit32(0x53007C00UL | (c << 16));
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    case OP_ASHR32:
        cb_emit32(0x13007C00UL | (c << 16));
        cb_emit32(0x93407C00UL); /* SXTW X0, W0 */
        break;
    default:
        return 0;
    }
    cb_emit32(0xF90003E0UL); /* STR X0, [SP] */
    return 1;
}

static void a64_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)call_addr;
    cb_patch32(call_pc, 0x94000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
}

static size_t a64_push_fn(size_t target_addr) {
    size_t pc = cb_pc();
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    u32 imm = (u32)diff;
    cb_emit32(0x10000000UL | ((imm & 0x3UL) << 29) |
           (((imm >> 2) & 0x7FFFFUL) << 5)); /* ADR X0, diff */
    push_x0();
    return pc;
}

static void a64_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)patch_addr;
    u32 imm = (u32)diff;
    u32 rd = (u32)(cb_data()[patch_pc] & 0x1FUL);
    cb_patch32(patch_pc, 0x10000000UL | ((imm & 0x3UL) << 29) |
                             (((imm >> 2) & 0x7FFFFUL) << 5) | rd);
}

static void a64_icall(void) {
    pop_x0();
    cb_emit32(0xD63F0000UL); /* BLR X0 */
}

const Backend be_arm64 = {"arm64",
                          8,
                          8,
                          8,
                          a64_cur_addr,
                          a64_fn_begin,
                          a64_fn_end,
                          a64_call,
                          NULL,
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
                          a64_bit_imm,
                          a64_mem_imm,
                          NULL,
                          NULL,
                          NULL,
                          a64_emit_br_fwd,
                          a64_emit_br_back,
                          a64_cmp_br_fwd,
                          a64_cmp_br_back,
                          a64_patch_fwd,
                          a64_emit_jtab,
                          a64_patch_jtab,
                          a64_patch_call,
                          a64_push_fn,
                          a64_patch_fn,
                          a64_icall,
                          NULL,
                          NULL};

/* --- Direct Register-Stack Backend (reg_be_arm64) --- */
static const u8 a64_stk_regs[12] = {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13};
static const u8 a64_fp_stk_regs[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
static const u8 a64_loc_regs[9] = {19, 20, 21, 22, 23, 24, 25, 26, 27};

static void a64r_mov_u64(u8 dst, u32 lo, u32 hi) {
    cb_emit32(0xD2800000UL | ((lo & 0xFFFFUL) << 5) | (u32)dst);
    if ((lo >> 16) != 0 || hi != 0)
        cb_emit32(0xF2A00000UL | (((lo >> 16) & 0xFFFFUL) << 5) | (u32)dst);
    if (hi != 0) {
        cb_emit32(0xF2C00000UL | ((hi & 0xFFFFUL) << 5) | (u32)dst);
        cb_emit32(0xF2E00000UL | (((hi >> 16) & 0xFFFFUL) << 5) | (u32)dst);
    }
}

static void a64r_mov_imm(u8 dst, i32 val) {
    a64r_mov_u64(dst, (u32)val, (val < 0) ? 0xFFFFFFFFUL : 0x00000000UL);
}

static void a64r_mov_uimm32(u8 dst, u32 val) { a64r_mov_u64(dst, val, 0); }

static void a64r_mov_fconst(u8 dst, double val) {
    u8 bytes[8];
    u32 lo, hi;
    memcpy(bytes, &val, 8);
    lo = (u32)bytes[0] | ((u32)bytes[1] << 8) | ((u32)bytes[2] << 16) | ((u32)bytes[3] << 24);
    hi = (u32)bytes[4] | ((u32)bytes[5] << 8) | ((u32)bytes[6] << 16) | ((u32)bytes[7] << 24);
    a64r_mov_u64(15, lo, hi);
    cb_emit32(0x9E670000UL | (15UL << 5) | (u32)dst); /* FMOV Dd, X15 */
}

static void a64r_mov_reg(u8 dst, u8 src, StackType type) {
    if (dst == src)
        return;
    if (type == TY_F)
        cb_emit32(0x1E604000UL | ((u32)src << 5) | (u32)dst); /* FMOV Dd, Dn */
    else
        cb_emit32(0xAA0003E0UL | ((u32)src << 16) | (u32)dst); /* MOV Xd, Xm */
}

static void a64r_spill_store(u8 reg, StackType type, i32 frame_off) {
    u32 op_unscaled = (type == TY_F) ? 0xFC0003A0UL : 0xF80003A0UL;
    u32 op_zero = (type == TY_F) ? 0xFD000000UL : 0xF9000000UL;
    if (frame_off <= 255) {
        cb_emit32(op_unscaled | (((0UL - (u32)frame_off) & 0x1FFUL) << 12) | (u32)reg);
    } else {
        a64r_mov_u64(15, (u32)frame_off, 0);
        cb_emit32(0xCB0F03AFUL); /* SUB X15, X29, X15 */
        cb_emit32(op_zero | (15UL << 5) | (u32)reg);
    }
}

static void a64r_spill_load(u8 reg, StackType type, i32 frame_off) {
    u32 op_unscaled = (type == TY_F) ? 0xFC4003A0UL : 0xF84003A0UL;
    u32 op_zero = (type == TY_F) ? 0xFD400000UL : 0xF9400000UL;
    if (frame_off <= 255) {
        cb_emit32(op_unscaled | (((0UL - (u32)frame_off) & 0x1FFUL) << 12) | (u32)reg);
    } else {
        a64r_mov_u64(15, (u32)frame_off, 0);
        cb_emit32(0xCB0F03AFUL); /* SUB X15, X29, X15 */
        cb_emit32(op_zero | (15UL << 5) | (u32)reg);
    }
}

static void a64r_local_addr(u8 dst, i32 frame_off) {
    if (frame_off <= 4095) {
        cb_emit32(0xD10003A0UL | (((u32)frame_off & 0xFFFUL) << 10) | (u32)dst);
    } else {
        a64r_mov_u64(15, (u32)frame_off, 0);
        cb_emit32(0xCB0F03A0UL | (u32)dst); /* SUB Xd, X29, X15 */
    }
}

static void a64r_load_data_addr(u8 dst, size_t offset) {
    if (offset <= 4095) {
        cb_emit32(0x91000380UL | (((u32)offset & 0xFFFUL) << 10) | (u32)dst);
    } else {
        a64r_mov_u64(15, (u32)offset, 0);
        cb_emit32(0x8B0F0380UL | (u32)dst); /* ADD Xd, X28, X15 */
    }
}

static size_t a64r_load_fn_addr(u8 dst, size_t target_addr) {
    size_t pc = cb_pc();
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    u32 imm = (u32)diff;
    cb_emit32(0x10000000UL | ((imm & 0x3UL) << 29) |
              (((imm >> 2) & 0x7FFFFUL) << 5) | (u32)dst);
    return pc;
}

static void a64r_load_frame_arg(u8 dst, StackType type, u32 pos_off) {
    u32 scaled_op = (type == TY_F) ? 0xFD400000UL : 0xF9400000UL;
    if ((pos_off / 8) <= 4095) {
        cb_emit32(scaled_op | ((pos_off / 8) << 10) | (29UL << 5) | (u32)dst);
    } else {
        a64r_mov_u64(15, pos_off, 0);
        cb_emit32(0x8B0F03AFUL); /* ADD X15, X29, X15 */
        cb_emit32(scaled_op | (15UL << 5) | (u32)dst);
    }
}

static void a64r_sp_adj(i32 delta) {
    if (delta == 0)
        return;
    if (delta < 0) {
        u32 mag = (u32)(-delta);
        if (mag <= 4095) {
            cb_emit32(0xD10003FFUL | ((mag & 0xFFFUL) << 10)); /* SUB SP, SP, #mag */
        } else {
            a64r_mov_u64(15, mag, 0);
            cb_emit32(0xCB2F63FFUL); /* SUB SP, SP, X15 */
        }
    } else {
        u32 mag = (u32)delta;
        if (mag <= 4095) {
            cb_emit32(0x910003FFUL | ((mag & 0xFFFUL) << 10)); /* ADD SP, SP, #mag */
        } else {
            a64r_mov_u64(15, mag, 0);
            cb_emit32(0x8B2F63FFUL); /* ADD SP, SP, X15 */
        }
    }
}

static void a64r_store_out_arg(u8 src_reg, StackType type, u32 sp_off) {
    u32 scaled_op = (type == TY_F) ? 0xFD000000UL : 0xF9000000UL;
    if ((sp_off / 8) <= 4095) {
        cb_emit32(scaled_op | ((sp_off / 8) << 10) | (31UL << 5) | (u32)src_reg);
    } else {
        a64r_mov_u64(15, sp_off, 0);
        cb_emit32(0x8B2F63EFUL); /* ADD X15, SP, X15 */
        cb_emit32(scaled_op | (15UL << 5) | (u32)src_reg);
    }
}

static void a64r_fn_begin(const char *name, u8 used_loc_regs, u32 frame_bytes, size_t param_cnt,
                          const LocalVar *locals, const u8 *loc_is_reg, const u8 *loc_reg,
                          const i32 *loc_off, size_t total_locals) {
    u32 save_bytes = ((u32)(used_loc_regs + 1U) & ~1U) * 8U;
    u32 total_frame = frame_bytes + save_bytes;
    size_t i;
    (void)name;

    cb_emit32(0xA9BF7BFDUL); /* STP X29, X30, [SP, #-16]! */
    cb_emit32(0x910003FDUL); /* MOV X29, SP */

    if (total_frame > 0) {
        if (total_frame <= 4095) {
            cb_emit32(0xD10003FFUL | ((total_frame & 0xFFFUL) << 10));
        } else {
            a64r_mov_u64(15, total_frame, 0);
            cb_emit32(0xCB2F63FFUL); /* SUB SP, SP, X15 */
        }
    }

    for (i = 0; i < used_loc_regs; i++) {
        a64r_spill_store(a64_loc_regs[i], TY_W, (i32)(frame_bytes + (i + 1) * 8));
    }

    for (i = 0; i < param_cnt; i++) {
        StackType ty = locals[i].type;
        if (i < 12) {
            u8 src = (ty == TY_F) ? a64_fp_stk_regs[i] : a64_stk_regs[i];
            if (loc_is_reg[i]) {
                a64r_mov_reg(loc_reg[i], src, ty);
            } else {
                a64r_spill_store(src, ty, loc_off[i]);
            }
        } else {
            u32 caller_off = (u32)(16 + (i - 12) * 8);
            if (loc_is_reg[i]) {
                a64r_load_frame_arg(loc_reg[i], ty, caller_off);
            } else {
                u8 tmp = (ty == TY_F) ? 16 : 8;
                a64r_load_frame_arg(tmp, ty, caller_off);
                a64r_spill_store(tmp, ty, loc_off[i]);
            }
        }
    }

    for (i = param_cnt; i < total_locals; i++) {
        if (locals[i].size > 0)
            continue;
        if (loc_is_reg[i]) {
            cb_emit32(0xD2800000UL | (u32)loc_reg[i]);
        } else if (locals[i].type == TY_F) {
            a64r_spill_store(31, TY_W, loc_off[i]); /* XZR via STUR XZR */
        } else {
            a64r_spill_store(31, TY_W, loc_off[i]);
        }
    }
}

static void a64r_fn_end(u8 used_loc_regs, u32 frame_bytes) {
    u8 i;
    for (i = 0; i < used_loc_regs; i++) {
        a64r_spill_load(a64_loc_regs[i], TY_W, (i32)(frame_bytes + (i + 1) * 8));
    }
    cb_emit32(0x910003BFUL); /* MOV SP, X29 */
    cb_emit32(0xA8C17BFDUL); /* LDP X29, X30, [SP], #16 */
    cb_emit32(0xD65F03C0UL); /* RET */
}

static void a64r_alu_reg(IrOp op, u8 dst, u8 lhs, u8 rhs) {
    if (op >= OP_EQ && op <= OP_UGE32) {
        int is32 = (op >= OP_EQ32 && op <= OP_UGE32);
        u32 cond = a64_cmp_cond(op);
        cb_emit32((is32 ? 0x6B00001FUL : 0xEB00001FUL) | ((u32)rhs << 16) | ((u32)lhs << 5));
        cb_emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12) | (u32)dst);
        return;
    }
    switch (op) {
    case OP_ADD:
    case OP_AADD:
        cb_emit32(0x8B000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_ADD32:
        cb_emit32(0x0B000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit32(0xCB000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_SUB32:
        cb_emit32(0x4B000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_MUL:
        cb_emit32(0x9B007C00UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_MUL32:
        cb_emit32(0x1B007C00UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_DIV:
        cb_emit32(0x9AC00C00UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_DIV32:
        cb_emit32(0x1AC00C00UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_UDIV:
        cb_emit32(0x9AC00800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_UDIV32:
        cb_emit32(0x1AC00800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_MOD:
        cb_emit32(0x9AC00C0FUL | ((u32)rhs << 16) | ((u32)lhs << 5)); /* SDIV X15, lhs, rhs */
        cb_emit32(0x9B0081E0UL | ((u32)rhs << 16) | ((u32)lhs << 10) | (u32)dst); /* MSUB dst, X15, rhs, lhs */
        break;
    case OP_MOD32:
        cb_emit32(0x1AC00C0FUL | ((u32)rhs << 16) | ((u32)lhs << 5));
        cb_emit32(0x1B0081E0UL | ((u32)rhs << 16) | ((u32)lhs << 10) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_UMOD:
        cb_emit32(0x9AC0080FUL | ((u32)rhs << 16) | ((u32)lhs << 5));
        cb_emit32(0x9B0081E0UL | ((u32)rhs << 16) | ((u32)lhs << 10) | (u32)dst);
        break;
    case OP_UMOD32:
        cb_emit32(0x1AC0080FUL | ((u32)rhs << 16) | ((u32)lhs << 5));
        cb_emit32(0x1B0081E0UL | ((u32)rhs << 16) | ((u32)lhs << 10) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_AND:
        cb_emit32(0x8A000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_AND32:
        cb_emit32(0x0A000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_OR:
        cb_emit32(0xAA000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_OR32:
        cb_emit32(0x2A000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_XOR:
        cb_emit32(0xCA000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_XOR32:
        cb_emit32(0x4A000000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_LSHIFT:
        cb_emit32(0x9AC02000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_SHL32:
        cb_emit32(0x1AC02000UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_RSHIFT:
        cb_emit32(0x9AC02400UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_RSHR32:
        cb_emit32(0x1AC02400UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_ARSHIFT:
        cb_emit32(0x9AC02800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_ASHR32:
        cb_emit32(0x1AC02800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_INVERT:
        cb_emit32(0xAA2003E0UL | ((u32)lhs << 16) | (u32)dst);
        break;
    case OP_INV32:
        cb_emit32(0x2A2003E0UL | ((u32)lhs << 16) | (u32)dst);
        cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
        break;
    case OP_SEXT:
        cb_emit32(0x93401C00UL | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_W2A:
    case OP_A2W:
        a64r_mov_reg(dst, lhs, TY_W);
        break;
    case OP_W2I32:
    case OP_I322W:
        cb_emit32(0x93407C00UL | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_U2I32:
        cb_emit32(0x2A0003E0UL | ((u32)lhs << 16) | (u32)dst);
        break;
    default:
        ir4_die("unimplemented ARM64 reg ALU op");
    }
}

static int a64r_alu_imm(IrOp op, u8 dst, u8 lhs, i32 imm) {
    if (op == OP_ADD || op == OP_AADD || op == OP_SUB || op == OP_ASUB) {
        i32 delta = (op == OP_SUB || op == OP_ASUB) ? -imm : imm;
        if (delta == 0) {
            a64r_mov_reg(dst, lhs, TY_W);
            return 1;
        }
        if (delta > 0 && delta <= 4095) {
            cb_emit32(0x91000000UL | ((u32)delta << 10) | ((u32)lhs << 5) | (u32)dst);
            return 1;
        }
        if (delta < 0 && delta >= -4095) {
            cb_emit32(0xD1000000UL | ((u32)(-delta) << 10) | ((u32)lhs << 5) | (u32)dst);
            return 1;
        }
        return 0;
    }
    if (op >= OP_EQ && op <= OP_AGT && imm >= 0 && imm <= 4095) {
        u32 cond = a64_cmp_cond(op);
        cb_emit32(0xF100001FUL | ((u32)imm << 10) | ((u32)lhs << 5));
        cb_emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12) | (u32)dst);
        return 1;
    }
    if (op == OP_LSHIFT || op == OP_RSHIFT || op == OP_ARSHIFT ||
        op == OP_SHL32 || op == OP_RSHR32 || op == OP_ASHR32) {
        u32 max_sh = (op >= OP_SHL32) ? 31U : 63U;
        u32 c;
        if (imm < 0 || (u32)imm > max_sh)
            return 0;
        c = (u32)imm;
        if (c == 0) {
            if (op >= OP_SHL32)
                cb_emit32(0x93407C00UL | ((u32)lhs << 5) | (u32)dst);
            else
                a64r_mov_reg(dst, lhs, TY_W);
            return 1;
        }
        switch (op) {
        case OP_LSHIFT:
            cb_emit32(0xD3400000UL | ((64 - c) << 16) | ((63 - c) << 10) | ((u32)lhs << 5) | (u32)dst);
            break;
        case OP_RSHIFT:
            cb_emit32(0xD340FC00UL | (c << 16) | ((u32)lhs << 5) | (u32)dst);
            break;
        case OP_ARSHIFT:
            cb_emit32(0x9340FC00UL | (c << 16) | ((u32)lhs << 5) | (u32)dst);
            break;
        case OP_SHL32:
            cb_emit32(0x53000000UL | ((32 - c) << 16) | ((31 - c) << 10) | ((u32)lhs << 5) | (u32)dst);
            cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
            break;
        case OP_RSHR32:
            cb_emit32(0x53007C00UL | (c << 16) | ((u32)lhs << 5) | (u32)dst);
            cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
            break;
        case OP_ASHR32:
            cb_emit32(0x13007C00UL | (c << 16) | ((u32)lhs << 5) | (u32)dst);
            cb_emit32(0x93407C00UL | ((u32)dst << 5) | (u32)dst);
            break;
        default:
            return 0;
        }
        return 1;
    }
    return 0;
}

static void a64r_fp_op(IrOp op, u8 dst, u8 lhs, u8 rhs) {
    switch (op) {
    case OP_FNEG:
        cb_emit32(0x1E614000UL | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_FADD:
        cb_emit32(0x1E602800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_FSUB:
        cb_emit32(0x1E603800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_FMUL:
        cb_emit32(0x1E600800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_FDIV:
        cb_emit32(0x1E601800UL | ((u32)rhs << 16) | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT: {
        u32 cond = (op == OP_FEQ) ? 0x0 : (op == OP_FNE) ? 0x1 : (op == OP_FLT) ? 0x4 : 0xC;
        cb_emit32(0x1E602000UL | ((u32)rhs << 16) | ((u32)lhs << 5));
        cb_emit32(0xDA9F03E0UL | ((cond ^ 1UL) << 12) | (u32)dst);
        break;
    }
    case OP_W2F:
        cb_emit32(0x9E620000UL | ((u32)lhs << 5) | (u32)dst);
        break;
    case OP_F2W:
        cb_emit32(0x9E780000UL | ((u32)lhs << 5) | (u32)dst);
        break;
    default:
        ir4_die("unimplemented ARM64 FP op");
    }
}

static void a64r_mem_op(IrOp op, u8 val_reg, u8 base, i32 off, int is_store) {
    u32 sz, unscaled_op, scaled_op;
    switch (op) {
    case OP_U8FETCH:
        sz = 1; unscaled_op = 0x38400000UL; scaled_op = 0x39400000UL; break;
    case OP_I8FETCH:
        sz = 1; unscaled_op = 0x38800000UL; scaled_op = 0x39800000UL; break;
    case OP_SI8STORE:
    case OP_I8STORE:
        sz = 1; unscaled_op = 0x38000000UL; scaled_op = 0x39000000UL; break;
    case OP_U16FETCH:
        sz = 2; unscaled_op = 0x78400000UL; scaled_op = 0x79400000UL; break;
    case OP_I16FETCH:
        sz = 2; unscaled_op = 0x78800000UL; scaled_op = 0x79800000UL; break;
    case OP_SI16STORE:
    case OP_I16STORE:
        sz = 2; unscaled_op = 0x78000000UL; scaled_op = 0x79000000UL; break;
    case OP_I32FETCH:
        sz = 4; unscaled_op = 0xB8800000UL; scaled_op = 0xB9800000UL; break;
    case OP_I32STORE:
        sz = 4; unscaled_op = 0xB8000000UL; scaled_op = 0xB9000000UL; break;
    case OP_FFETCH:
        sz = 8; unscaled_op = 0xFC400000UL; scaled_op = 0xFD400000UL; break;
    case OP_SFSTORE:
    case OP_FSTORE:
        sz = 8; unscaled_op = 0xFC000000UL; scaled_op = 0xFD000000UL; break;
    case OP_FETCH:
    case OP_AFETCH:
        sz = 8; unscaled_op = 0xF8400000UL; scaled_op = 0xF9400000UL; break;
    default:
        sz = 8; unscaled_op = 0xF8000000UL; scaled_op = 0xF9000000UL; break;
    }
    (void)is_store;
    if (off >= 0 && ((u32)off & (sz - 1)) == 0 && ((u32)off / sz) <= 4095) {
        cb_emit32(scaled_op | (((u32)off / sz) << 10) | ((u32)base << 5) | (u32)val_reg);
    } else if (off >= -256 && off <= 255) {
        cb_emit32(unscaled_op | (((u32)off & 0x1FFUL) << 12) | ((u32)base << 5) | (u32)val_reg);
    } else {
        a64r_mov_imm(15, off);
        cb_emit32(0x8B0F000FUL | ((u32)base << 5)); /* ADD X15, base, X15 */
        cb_emit32(scaled_op | (15UL << 5) | (u32)val_reg);
    }
}

static void a64r_load_mem(IrOp op, u8 dst, u8 base, i32 off) { a64r_mem_op(op, dst, base, off, 0); }
static void a64r_store_mem(IrOp op, u8 src, u8 base, i32 off) { a64r_mem_op(op, src, base, off, 1); }

static void a64r_bcopy(u8 dst, u8 src, u8 len) {
    u8 l = len;
    if (l != 15) {
        a64r_mov_reg(15, l, TY_W);
        l = 15;
    }
    cb_emit32(0xB40000A0UL | (u32)l);                          /* CBZ Xl, +20 */
    cb_emit32(0x38401410UL | ((u32)src << 5));                 /* LDRB W16, [Xsrc], #1 */
    cb_emit32(0x38001410UL | ((u32)dst << 5));                 /* STRB W16, [Xdst], #1 */
    cb_emit32(0xF1000400UL | ((u32)l << 5) | (u32)l);          /* SUBS Xl, Xl, #1 */
    cb_emit32(0x54FFFFA1UL);                                   /* B.NE -12 */
}

static size_t a64r_br_fwd(BranchCond cond, u8 reg) {
    size_t pos = cb_pc();
    if (cond == BR_JZ)
        cb_emit32(0xB4000000UL | (u32)reg); /* CBZ Xr, rel19 */
    else
        cb_emit32(0x14000000UL);            /* B rel26 */
    return pos;
}

static void a64r_br_back(BranchCond cond, u8 reg, size_t target_addr) {
    i32 diff = (i32)target_addr - (i32)cb_cur_addr();
    if (cond == BR_JZ) {
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        cb_emit32(0xB4000000UL | (imm19 << 5) | (u32)reg);
    } else {
        cb_emit32(0x14000000UL | (((u32)(diff >> 2)) & 0x03FFFFFFUL));
    }
}

static int a64r_cmp_br_emit(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, int is_fwd,
                            size_t target_addr, size_t *patch_off) {
    u32 cond;
    if (op < OP_EQ || op > OP_AGT)
        return 0;
    if (has_imm && (imm < 0 || imm > 4095))
        return 0;
    cond = a64_cmp_cond(op);
    if (has_imm)
        cb_emit32(0xF100001FUL | ((u32)imm << 10) | ((u32)lhs << 5));
    else
        cb_emit32(0xEB00001FUL | ((u32)rhs << 16) | ((u32)lhs << 5));
    if (is_fwd) {
        *patch_off = cb_pc();
        cb_emit32(0x54000000UL | (cond ^ 1UL));
    } else {
        i32 diff = (i32)target_addr - (i32)cb_cur_addr();
        u32 imm19 = ((u32)(diff >> 2)) & 0x0007FFFFUL;
        cb_emit32(0x54000000UL | (imm19 << 5) | (cond ^ 1UL));
    }
    return 1;
}

static int a64r_cmp_br_fwd(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, size_t *patch_off) {
    return a64r_cmp_br_emit(op, has_imm, imm, lhs, rhs, 1, 0, patch_off);
}

static int a64r_cmp_br_back(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, size_t target_addr) {
    return a64r_cmp_br_emit(op, has_imm, imm, lhs, rhs, 0, target_addr, NULL);
}

static size_t a64r_emit_jtab(u8 reg, size_t count) {
    size_t tbl_off, k;
    cb_emit32(0x1000008FUL);                                /* ADR X15, +16 */
    cb_emit32(0xB8A079EFUL | ((u32)reg << 16));             /* LDRSW X15?, wait: LDRSW X8, [X15, Xr, LSL #2] */
    /* Correct: LDRSW X14, [X15, Xreg, LSL #2] -> 0xB8A079EE */
    cb_rewind(4);
    cb_emit32(0xB8A079EEUL | ((u32)reg << 16));             /* LDRSW X14, [X15, Xreg, LSL #2] */
    cb_emit32(0x8B0E01EEUL);                                /* ADD X14, X15, X14 */
    cb_emit32(0xD61F01C0UL);                                /* BR X14 */
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit32(0);
    return tbl_off;
}

static void a64r_icall(u8 reg) { cb_emit32(0xD63F0000UL | ((u32)reg << 5)); }

static void a64r_call_extern(const FuncSig *sig, const u8 *arg_regs) {
    u8 int_idx = 0, flt_idx = 0;
    size_t i;
    if (sig->has_addr)
        ir4_die("ARM64 extern does not take a fixed address");
    for (i = 0; i < sig->in_types.len; i++) {
        if (sig->in_types.data[i] == TY_F) {
            if (flt_idx >= 8)
                ir4_die("ARM64 extern supports at most 8 float args");
            a64r_mov_reg(flt_idx, arg_regs[i], TY_F);
            flt_idx++;
        } else {
            if (int_idx >= 8)
                ir4_die("ARM64 extern supports at most 8 int args");
            a64r_mov_reg(int_idx, arg_regs[i], TY_W);
            int_idx++;
        }
    }
    cb_add_extern_reloc(cb_pc(), sig->name);
    cb_emit32(0xD2800009UL); /* MOVZ X9, #0 */
    cb_emit32(0xF2A00009UL); /* MOVK X9, #0, LSL #16 */
    cb_emit32(0xF2C00009UL); /* MOVK X9, #0, LSL #32 */
    cb_emit32(0xF2E00009UL); /* MOVK X9, #0, LSL #48 */
    cb_emit32(0xD63F0120UL); /* BLR X9 */
}

const RegBackend reg_be_arm64 = {
    8, 1, 12, a64_stk_regs, a64_fp_stk_regs, 9, a64_loc_regs, 8, 9, 14, 16, 17,
    a64_cur_addr, a64r_fn_begin, a64r_fn_end, a64r_spill_store, a64r_spill_load,
    a64r_local_addr, a64r_sp_adj, a64r_store_out_arg, a64r_mov_reg, a64r_mov_imm,
    a64r_mov_uimm32, a64r_mov_fconst, a64r_load_data_addr, a64r_load_fn_addr,
    a64r_alu_reg, a64r_alu_imm, a64r_fp_op, a64r_load_mem, a64r_store_mem, a64r_bcopy,
    a64r_br_fwd, a64r_br_back, a64r_cmp_br_fwd, a64r_cmp_br_back, a64_patch_fwd,
    a64r_emit_jtab, a64_patch_jtab, a64_call, a64r_icall, a64r_call_extern};
