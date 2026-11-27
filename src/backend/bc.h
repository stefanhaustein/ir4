#ifndef IR4_BC_H
#define IR4_BC_H

#include "ir4.h"

/* Compact 16-bit stack-VM bytecode opcodes (<= 128 opcodes; 0x80..0xFF push 0..127). */
#define BC_PUSH_IMM7_BASE 0x80

typedef enum {
    BC_RET = 0,
    BC_ENTER,       /* u16 frame_bytes (u8 on 6502) */
    BC_LEAVE,       /* u16 frame_bytes (none on 6502) */
    BC_ENTER_BIG,   /* u16 big_bytes (6502 bfp) */
    BC_LEAVE_BIG,   /* 6502 bfp restore */
    BC_LOCAL_ADDR_BIG, /* u16 off (6502 bfp) */

    BC_PUSH_0_32,
    BC_PUSH_I8,     /* i8 */
    BC_PUSH_W,      /* word_bytes */
    BC_PUSH_I32,    /* i32 */
    BC_PUSH_F,      /* f32 (4 bytes) */
    BC_PUSH_DATA,   /* addr_bytes */
    BC_PUSH_FN,     /* addr_bytes */

    BC_INC,
    BC_DEC,
    BC_ADD_IMM8,    /* i8 */

    BC_STATIC_GET,  /* addr_bytes */
    BC_STATIC_GET32,/* addr_bytes */
    BC_STATIC_SET,  /* addr_bytes */
    BC_STATIC_SET32,/* addr_bytes */
    BC_LOCAL_GET,   /* addr_bytes */
    BC_LOCAL_GET32, /* addr_bytes */
    BC_LOCAL_SET,   /* addr_bytes */
    BC_LOCAL_SET32, /* addr_bytes */
    BC_LOCAL_ADDR,  /* addr_bytes */

    BC_DUP,
    BC_DUP32,
    BC_DROP,
    BC_DROP32,
    BC_SWAP,
    BC_SWAP_W32,
    BC_SWAP_32W,
    BC_SWAP32,

    BC_JMP,         /* u16 target_addr */
    BC_JZ,          /* u16 target_addr */
    BC_JTAB,        /* u16[] targets */
    BC_CALL,        /* u16 target_addr */
    BC_ICALL,
    BC_CALL_EXTERN, /* u16 extern_id */

    BC_ADD,
    BC_SUB,
    BC_MUL,
    BC_DIV,
    BC_MOD,
    BC_UDIV,
    BC_UMOD,
    BC_AND,
    BC_OR,
    BC_XOR,
    BC_INVERT,
    BC_LSHIFT,
    BC_ARSHIFT,
    BC_RSHIFT,

    BC_FADD,
    BC_FSUB,
    BC_FMUL,
    BC_FDIV,
    BC_FNEG,

    BC_ADD32,
    BC_SUB32,
    BC_MUL32,
    BC_DIV32,
    BC_MOD32,
    BC_UDIV32,
    BC_UMOD32,
    BC_AND32,
    BC_OR32,
    BC_XOR32,
    BC_INV32,
    BC_SHL32,
    BC_ASHR32,
    BC_RSHR32,

    BC_EQ,
    BC_NE,
    BC_LT,
    BC_GT,
    BC_LE,
    BC_GE,
    BC_ULT,
    BC_UGT,
    BC_ULE,
    BC_UGE,

    BC_FEQ,
    BC_FNE,
    BC_FLT,
    BC_FGT,

    BC_EQ32,
    BC_NE32,
    BC_LT32,
    BC_GT32,
    BC_LE32,
    BC_GE32,
    BC_ULT32,
    BC_UGT32,
    BC_ULE32,
    BC_UGE32,

    BC_I8FETCH,
    BC_U8FETCH,
    BC_I8STORE,
    BC_FETCH,
    BC_STORE,
    BC_FETCH32,
    BC_STORE32,
    BC_BCOPY,

    BC_SEXT,
    BC_W2F,
    BC_F2W,
    BC_W2I32,
    BC_U2I32,
    BC_I322W,

    /* Explicit 16-bit sub-word memory ops (used when word_bytes > 2) */
    BC_U16FETCH,
    BC_I16FETCH,
    BC_STORE16
} BcOp;

u16 bc_extern_count(void);
const char *bc_extern_name(u16 id);

/* Shared parameterized bytecode emitter helpers */
void bc_emit_addr(size_t val, u8 addr_bytes);
void bc_patch_addr(size_t patch_off, size_t target, u8 addr_bytes);
void bc_emit_data_reloc(size_t data_off, u8 addr_bytes);
void bc_emit_push_w(i32 val, u8 word_bytes);
void bc_emit_push_a(u32 val, u8 addr_bytes);
void bc_emit_push_f(double val);
void bc_emit_push_i32(i32 val);
void bc_emit_push_data(size_t data_offset, u8 addr_bytes);
void bc_emit_stack_op(StackOp op, StackType t1, StackType t2);
void bc_emit_op(IrOp op, u8 word_bytes);
int bc_emit_add_imm(i32 imm);
size_t bc_emit_br_fwd_op(BranchCond cond, u8 addr_bytes);
void bc_emit_br_back_op(BranchCond cond, size_t target_addr, u8 addr_bytes);
void bc_patch_fwd_op(size_t patch_offset, u8 addr_bytes);
size_t bc_emit_jtab_op(size_t count, u8 addr_bytes);
void bc_patch_jtab_op(size_t patch_offset, size_t idx, size_t target_addr, u8 addr_bytes);
size_t bc_emit_push_fn_op(size_t target_addr, u8 addr_bytes);
void bc_patch_fn_op(size_t patch_pc, size_t target_addr, u8 addr_bytes);
void bc_patch_call_op(size_t call_pc, size_t target_addr, u8 addr_bytes);

extern const Backend be_bc;
extern const Backend be_bc32;
extern const Platform plat_bc_run;
extern const Platform plat_bc32;
extern const Platform plat_web;
extern const Platform plat_web_js;

#endif
