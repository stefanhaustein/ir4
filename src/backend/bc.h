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
    BC_PUSH_I16,    /* u16 */
    BC_PUSH_I32,    /* i32 */
    BC_PUSH_F,      /* f32 (4 bytes) */
    BC_PUSH_DATA,   /* u16 addr */
    BC_PUSH_FN,     /* u16 addr */

    BC_INC,
    BC_DEC,
    BC_ADD_IMM8,    /* i8 */

    BC_STATIC_GET2, /* u16 addr */
    BC_STATIC_GET4, /* u16 addr */
    BC_STATIC_SET2, /* u16 addr */
    BC_STATIC_SET4, /* u16 addr */
    BC_LOCAL_GET2,  /* u16 off */
    BC_LOCAL_GET4,  /* u16 off */
    BC_LOCAL_SET2,  /* u16 off */
    BC_LOCAL_SET4,  /* u16 off */
    BC_LOCAL_ADDR,  /* u16 off */

    BC_DUP2,
    BC_DUP4,
    BC_DROP2,
    BC_DROP4,
    BC_SWAP22,
    BC_SWAP24,
    BC_SWAP42,
    BC_SWAP44,

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
    BC_FETCH16,
    BC_STORE16,
    BC_FETCH32,
    BC_STORE32,
    BC_BCOPY,

    BC_SEXT,
    BC_W2F,
    BC_F2W,
    BC_W2I32,
    BC_U2I32,
    BC_I322W
} BcOp;

const char *bc_extern_name(u16 id);

extern const Backend be_bc;
extern const Platform plat_bc_run;

#endif
