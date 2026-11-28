/* ir4.h - Core IR4 types, limits, and Backend/Platform interfaces
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
#include "collections.h"

typedef unsigned char u8;
typedef u8 Bool;
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
#ifndef IR4_ENABLE_65816
#define IR4_ENABLE_65816 IR4_DEF_OPT
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
#ifndef IR4_ENABLE_SIM65816
#define IR4_ENABLE_SIM65816 IR4_ENABLE_65816
#endif
#ifndef IR4_ENABLE_X16GS
#define IR4_ENABLE_X16GS IR4_ENABLE_65816
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
    u8 type; /* StackType */
    u16 slot;
    size_t size;
} LocalVar;

#define FN_FL_EXTERN    0x01
#define FN_FL_DEFINED   0x02
#define FN_FL_DEFAULT   0x04
#define FN_FL_HOT       0x08
#define FN_FL_HAS_ADDR  0x10
#define FN_FL_RECURSIVE 0x20

#define FN_INLINE_TYPES 8

VEC_OF(TypeVec, StackType);
VEC_OF(StrVec, const char *);

typedef struct {
    const char **in_regs; /* NULL or in_cnt entries */
    const char *out_reg;
} ExternRegs;

typedef struct {
    const char *name;
    size_t addr;
    u8 flags;
    u8 req_rank;
    u16 req_group;
    u16 bank;
    u8 in_cnt;
    u8 out_cnt;
    ExternRegs *ext;
    union {
        u8 inline_t[FN_INLINE_TYPES];
        u8 *heap_t;
    } types;
} FnSig;

#define FN_FLAG(s, f)      (((s)->flags & (f)) != 0)
#define FN_SET_FLAG(s, f)  ((s)->flags |= (u8)(f))
#define FN_CLR_FLAG(s, f)  ((s)->flags &= (u8)~(f))

#define FN_TYPES_PTR(s)    (((s)->in_cnt + (s)->out_cnt <= FN_INLINE_TYPES) \
                            ? (s)->types.inline_t : (s)->types.heap_t)
#define FN_IN_TYPE(s, i)   ((StackType)FN_TYPES_PTR(s)[(i)])
#define FN_OUT_TYPE(s, i)  ((StackType)FN_TYPES_PTR(s)[(s)->in_cnt + (i)])

/* Backend code-generation interface implemented by each target (be_*.c). */
typedef struct {
    const char *name;
    u8 word_bytes;
    u8 addr_bytes;
    u8 float_bytes;

    size_t (*cur_addr)(void);
    void (*fn_begin)(const char *name, int param_cnt, const LocalVar *locals,
                     int total_locals, Bool is_recursive);
    void (*fn_end)(const FnSig *sig, const LocalVar *locals, int total_locals);
    void (*call)(size_t target_addr);
    int (*tail_call)(size_t target_addr, const FnSig *sig, const FnSig *callee,
                     const LocalVar *locals, int total_locals);
    void (*call_extern)(const FnSig *sig);

    void (*push_w)(i32 val);
    void (*push_a)(u32 val);
    void (*push_f)(double val);
    void (*push_i32)(i32 val);
    void (*push_data)(size_t data_offset);
    void (*local_op)(LocalOp op, int slot, StackType type, size_t stack_bytes);
    int (*local_add_imm)(int slot, StackType type, size_t stack_bytes, i32 imm);

    void (*stack_op)(StackOp op, StackType t1, StackType t2);
    void (*op)(IrOp op);
    int (*add_imm)(int is_addr, i32 imm);
    int (*cmp_imm)(IrOp op, i32 imm);
    int (*shift_imm)(IrOp op, u8 count);
    int (*bit_imm)(IrOp op, i32 imm);
    int (*mem_imm)(IrOp op, i32 off);
    int (*data_mem)(IrOp op, size_t data_offset);
    int (*local_mem)(int slot, i32 off, IrOp op, size_t stack_bytes);
    int (*local_deref)(int slot, i32 off, IrOp op, size_t stack_bytes);

    size_t (*emit_br_fwd)(BranchCond cond);
    void (*emit_br_back)(BranchCond cond, size_t target_addr);
    int (*cmp_br_fwd)(IrOp op, int has_imm, i32 imm, size_t *patch_offset);
    int (*cmp_br_back)(IrOp op, int has_imm, i32 imm, size_t target_addr);
    void (*patch_fwd)(size_t patch_offset);
    size_t (*emit_jtab)(size_t count);
    void (*patch_jtab)(size_t patch_offset, size_t idx, size_t count, size_t target_addr);
    void (*patch_call)(size_t call_pc, size_t call_addr, size_t target_addr);
    size_t (*push_fn)(size_t target_addr);
    void (*patch_fn)(size_t patch_pc, size_t patch_addr, size_t target_addr);
    void (*icall)(void);
    size_t (*stack_data_off)(void);
    void (*peep_fence)(void);
} Backend;

/* Maximum number of return values in a function or indirect call signature. */
#define IR4_MAX_OUTS 4

/* Maximum number of locals per function (including parameters and :bytes copies). */
#define IR4_MAX_LOCALS 128

/* Error reporting & optional live progress (no-op unless -DIR4_STATUS; lexer.c) */
void ir4_die(const char *msg);
void ir4_status(const char *msg);
void ir4_status_file(const char *prefix, const char *path, u32 line_num);
void ir4_status_prog(const char *prefix, int done, int total);
void ir4_status_clear(void);

/* Optimization control (implemented in frontend/frontend.c) */
void ir4_set_opt_level(int level);
int ir4_get_opt_level(void);
void ir4_set_asm_check(int enable);

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
    void (*run)(int main_idx);
    const char *default_ext;
} Platform;

extern const Backend be_z80;
extern const Backend be_ez80;
extern const Backend be_6502;
extern const Backend be_65816;
extern const Backend be_arm64;
extern const Backend be_rv32;

extern const Platform plat_z80_cpm;
extern const Platform plat_z80_zx81;
extern const Platform plat_ez80_agon;
extern const Platform plat_6502_sim;
extern const Platform plat_65816_sim;
extern const Platform plat_65816_x16gs;
#if IR4_ENABLE_X16GS
void x16gs_set_out_path(const char *path);
#endif
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
void be_ez80_set_compact(int compact);
int be_ez80_get_compact(void);
void be_6502_set_call_bank(u16 bank);
void be_6502_peep_fence(void);
void be_6502_set_trampoline(u16 bank_call_addr, u16 kernal_call_addr);
void be_6502_ensure_helpers(int need_mul, int need_div, int need_icall, int need_fp);
void be_rv32_set_obj(int obj);
void ir4_jit_set_args(const char *prog, int argc, char **argv);

#endif /* IR4_H */
