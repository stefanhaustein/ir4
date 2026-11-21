/* ir4.h - Core IR4 types, limits, and Backend/Platform interfaces (Strict C89)
 *
 * Shared contract between the IR4th frontend, code buffer, backends, and
 * platform/output drivers. Each backend implements the Backend vtable; each
 * output mode (--cpm, --sim65, --virt, --obj, --run) implements Platform.
 */
#ifndef IR4_H
#define IR4_H

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vec.h"

typedef unsigned char u8;
typedef int Bool;
#ifndef true
#define false 0
#define true 1
#endif
typedef signed char i8;
typedef unsigned short u16;
typedef signed short i16;
#if UINT_MAX == 0xFFFFFFFFUL
typedef unsigned int u32;
typedef signed int i32;
#else
typedef unsigned long u32;
typedef signed long i32;
#endif

/* Optional target/platform subsetting: define IR4_MINIMAL to default all to 0,
 * then enable only desired backends/platforms (e.g. -DIR4_ENABLE_EZ80=1 -DIR4_ENABLE_AGON=1). */
#ifdef IR4_MINIMAL
#define IR4_DEF_OPT 0
#else
#define IR4_DEF_OPT 1
#endif

#ifndef IR4_ENABLE_Z80
#define IR4_ENABLE_Z80 IR4_DEF_OPT
#endif
#ifndef IR4_ENABLE_EZ80
#define IR4_ENABLE_EZ80 IR4_DEF_OPT
#endif
#ifndef IR4_ENABLE_6502
#define IR4_ENABLE_6502 IR4_DEF_OPT
#endif
#ifndef IR4_ENABLE_ARM64
#define IR4_ENABLE_ARM64 IR4_DEF_OPT
#endif
#ifndef IR4_ENABLE_RV32
#define IR4_ENABLE_RV32 IR4_DEF_OPT
#endif
#ifndef IR4_ENABLE_BC
#define IR4_ENABLE_BC IR4_DEF_OPT
#endif

#ifndef IR4_ENABLE_CPM
#define IR4_ENABLE_CPM IR4_ENABLE_Z80
#endif
#ifndef IR4_ENABLE_ZX81
#define IR4_ENABLE_ZX81 IR4_ENABLE_Z80
#endif
#ifndef IR4_ENABLE_AGON
#define IR4_ENABLE_AGON IR4_ENABLE_EZ80
#endif
#ifndef IR4_ENABLE_SIM65
#define IR4_ENABLE_SIM65 IR4_ENABLE_6502
#endif
#ifndef IR4_ENABLE_C64
#define IR4_ENABLE_C64 IR4_ENABLE_6502
#endif
#ifndef IR4_ENABLE_X16
#define IR4_ENABLE_X16 IR4_ENABLE_6502
#endif
#ifndef IR4_ENABLE_JIT
#define IR4_ENABLE_JIT IR4_ENABLE_ARM64
#endif
#ifndef IR4_ENABLE_ELF
#define IR4_ENABLE_ELF IR4_ENABLE_RV32
#endif

/* Stack types (v0.3) */
typedef enum { TY_NONE = 0, TY_W, TY_A, TY_F, TY_I32 } StackType;

/* Stack operations */
typedef enum { OP_DUP = 0, OP_DROP, OP_SWAP } StackOp;

/* Local variable operations */
typedef enum { LOCAL_GET = 0, LOCAL_SET, LOCAL_ADDR, LOCAL_ADD } LocalOp;

/* Branch conditions */
typedef enum { BR_JMP = 0, BR_JZ } BranchCond;

/* Unified IR operations (ALU, comparison, memory, conversion) */
typedef enum {
    OP_ADD = 0,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_UDIV,
    OP_UMOD,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_INVERT,
    OP_LSHIFT,
    OP_ARSHIFT,
    OP_RSHIFT,
    OP_AADD,
    OP_ASUB,
    OP_ADIFF,
    OP_FADD,
    OP_FSUB,
    OP_FMUL,
    OP_FDIV,
    OP_FNEG,
    OP_ADD32,
    OP_SUB32,
    OP_MUL32,
    OP_DIV32,
    OP_MOD32,
    OP_UDIV32,
    OP_UMOD32,
    OP_AND32,
    OP_OR32,
    OP_XOR32,
    OP_INV32,
    OP_SHL32,
    OP_ASHR32,
    OP_RSHR32,

    OP_EQ,
    OP_NE,
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_ULT,
    OP_UGT,
    OP_ULE,
    OP_UGE,
    OP_AEQ,
    OP_ANE,
    OP_ALT,
    OP_AGT,
    OP_FEQ,
    OP_FNE,
    OP_FLT,
    OP_FGT,
    OP_EQ32,
    OP_NE32,
    OP_LT32,
    OP_GT32,
    OP_LE32,
    OP_GE32,
    OP_ULT32,
    OP_UGT32,
    OP_ULE32,
    OP_UGE32,

    OP_I8FETCH,
    OP_U8FETCH,
    OP_I8STORE,
    OP_I16FETCH,
    OP_U16FETCH,
    OP_I16STORE,
    OP_FETCH,
    OP_STORE,
    OP_AFETCH,
    OP_ASTORE,
    OP_FFETCH,
    OP_FSTORE,
    OP_I32FETCH,
    OP_I32STORE,

    OP_SI8STORE,
    OP_SI16STORE,
    OP_SSTORE,
    OP_SASTORE,
    OP_SFSTORE,
    OP_BCOPY,

    OP_SEXT,
    OP_W2A,
    OP_A2W,
    OP_W2F,
    OP_F2W,
    OP_W2I32,
    OP_U2I32,
    OP_I322W
} IrOp;

typedef struct {
    const char *name;
    StackType type;
    size_t slot;
    size_t size;
} LocalVar;

VEC_OF(TypeVec, StackType);
VEC_OF(StrVec, const char *);

typedef struct {
    const char *name;
    size_t addr;
    Bool is_extern;
    Bool is_defined;
    Bool is_default;
    Bool is_hot;
    Bool has_addr;
    Bool is_recursive;
    u16 req_group;
    u8 req_rank;
    u16 bank;
    TypeVec in_types;
    StrVec in_regs;
    TypeVec out_types;
    const char *out_reg;
} FuncSig;

/* Backend code-generation interface implemented by each target (be_*.c). */
typedef struct {
    const char *name;
    u8 word_bytes;
    u8 addr_bytes;
    u8 float_bytes;

    size_t (*cur_addr)(void);
    void (*fn_begin)(const char *name, size_t param_cnt, const LocalVar *locals,
                     size_t total_locals, Bool is_recursive);
    void (*fn_end)(const FuncSig *sig, const LocalVar *locals, size_t total_locals);
    void (*call)(size_t target_addr);
    void (*call_extern)(const FuncSig *sig);

    void (*push_w)(i32 val);
    void (*push_a)(u32 val);
    void (*push_f)(double val);
    void (*push_i32)(i32 val);
    void (*push_data)(size_t data_offset);
    void (*local_op)(LocalOp op, size_t slot, StackType type, size_t stack_bytes);
    int (*local_add_imm)(size_t slot, StackType type, size_t stack_bytes, i32 imm);

    void (*stack_op)(StackOp op, StackType t1, StackType t2);
    void (*op)(IrOp op);
    int (*add_imm)(i32 imm);
    int (*cmp_imm)(IrOp op, i32 imm);
    int (*shift_imm)(IrOp op, u8 count);
    int (*mem_imm)(IrOp op, i32 off);
    int (*data_mem)(IrOp op, size_t data_offset);
    int (*local_mem)(size_t slot, i32 off, IrOp op, size_t stack_bytes);

    size_t (*emit_br_fwd)(BranchCond cond);
    void (*emit_br_back)(BranchCond cond, size_t target_addr);
    int (*cmp_br_fwd)(IrOp op, int has_imm, i32 imm, size_t *patch_offset);
    int (*cmp_br_back)(IrOp op, int has_imm, i32 imm, size_t target_addr);
    void (*patch_fwd)(size_t patch_offset);
    void (*patch_call)(size_t call_pc, size_t call_addr, size_t target_addr);
    size_t (*push_fn)(size_t target_addr);
    void (*patch_fn)(size_t patch_pc, size_t patch_addr, size_t target_addr);
    void (*icall)(void);
    size_t (*stack_data_off)(void);
} Backend;

/* Error reporting (implemented in frontend.c) */
void ir4_die(const char *msg);

/* Platform / output-format hooks (startup stub, epilogue, file writer, runner). */
typedef struct {
    const char *flag;
    const Backend *be;
    size_t default_base;
    const char *req_platform;
    Bool needs_main;
    void (*begin)(size_t base);
    void (*end)(int main_idx);
    int (*write)(FILE *fp, size_t base);
    void (*run)(size_t main_idx);
} Platform;

extern const Backend be_z80;
extern const Backend be_ez80;
extern const Backend be_6502;
extern const Backend be_arm64;
extern const Backend be_rv32;

extern const Platform plat_z80_cpm;
extern const Platform plat_z80_zx81;
extern const Platform plat_ez80_agon;
extern const Platform plat_6502_sim;
extern const Platform plat_6502_c64;
extern const Platform plat_6502_x16;
extern const Platform plat_6502_x16_cart;
extern const Platform plat_arm64_jit;
extern const Platform plat_rv32_virt;
extern const Platform plat_rv32_obj;

void be_6502_set_zp(u8 zp);
void be_6502_set_default_zp(u8 zp);
void be_6502_set_stack_zp(u8 base, u8 init_x);
u8 be_6502_stack_zp_base(void);
size_t be_6502_stack_data_off(void);
void be_6502_set_compact(int compact);
int be_6502_get_compact(void);
void be_6502_set_call_bank(u16 bank);
void be_6502_peep_fence(void);
void be_6502_set_trampoline(u16 bank_call_addr, u16 kernal_call_addr);
void be_6502_ensure_helpers(int need_mul, int need_div, int need_icall, int need_fp);
void be_rv32_set_obj(int obj);

#endif /* IR4_H */
