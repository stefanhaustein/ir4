/* reg_lower.h - Shared depth-indexed register-stack lowering for RISC backends */
#ifndef REG_LOWER_H
#define REG_LOWER_H

#include "frontend/internal.h"

#define REG_MAX_STK 16
#define REG_MAX_LOC 12

typedef struct {
    u8 word_bytes;
    u8 has_fp_regs;
    u8 num_stk_regs;
    const u8 *stk_regs;
    const u8 *fp_stk_regs;
    u8 num_loc_regs;
    const u8 *loc_regs;
    u8 scratch0;
    u8 scratch1;
    u8 scratch2;
    u8 fp_scratch;
    u8 fp_scratch1;

    size_t (*cur_addr)(void);

    void (*fn_begin)(const char *name, u8 used_loc_regs, u32 frame_bytes, size_t param_cnt,
                     const LocalVar *locals, const u8 *loc_is_reg, const u8 *loc_reg,
                     const i32 *loc_off, size_t total_locals);
    void (*fn_end)(u8 used_loc_regs, u32 frame_bytes);

    void (*spill_store)(u8 reg, StackType type, i32 frame_off);
    void (*spill_load)(u8 reg, StackType type, i32 frame_off);
    void (*local_addr)(u8 dst_reg, i32 frame_off);

    void (*mov_reg)(u8 dst, u8 src, StackType type);
    void (*mov_imm)(u8 dst, i32 val);
    void (*mov_uimm32)(u8 dst, u32 val);
    void (*mov_fconst)(u8 dst, double val);
    void (*load_data_addr)(u8 dst, size_t offset);
    size_t (*load_fn_addr)(u8 dst, size_t target_addr);

    void (*alu_reg)(IrOp op, u8 dst, u8 lhs, u8 rhs);
    int (*alu_imm)(IrOp op, u8 dst, u8 lhs, i32 imm);
    void (*fp_op)(IrOp op, u8 dst, u8 lhs, u8 rhs);

    void (*load_mem)(IrOp op, u8 dst, u8 base, i32 off);
    void (*store_mem)(IrOp op, u8 src, u8 base, i32 off);
    void (*emit_bcopy)(u8 dst, u8 src, u8 len);

    size_t (*br_fwd)(BranchCond cond, u8 reg);
    void (*br_back)(BranchCond cond, u8 reg, size_t target_addr);
    int (*cmp_br_fwd)(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, size_t *patch_off);
    int (*cmp_br_back)(IrOp op, int has_imm, i32 imm, u8 lhs, u8 rhs, size_t target_addr);
    void (*patch_fwd)(size_t patch_off);

    size_t (*emit_jtab)(u8 reg, size_t count);
    void (*patch_jtab)(size_t patch_offset, size_t idx, size_t count, size_t target_addr);

    void (*call)(size_t target_addr);
    void (*icall)(u8 reg);
    void (*call_extern)(const FuncSig *sig, const u8 *arg_regs);
} RegBackend;

typedef struct {
    size_t call_pc;
    size_t call_addr;
    size_t func_idx;
    Bool is_fn_ptr;
} RegFwdCall;

VEC_OF(RegFwdCallVec, RegFwdCall);

void reg_lower_word(const RegBackend *rbe, FuncSig *sig, WordIR *w,
                    const FuncVec *funcs, const WordVec *words,
                    const SizeVec *jtab_targets, size_t heap_start_off,
                    RegFwdCallVec *fwd_calls);

extern const RegBackend reg_be_arm64;
extern const RegBackend reg_be_rv32;

#endif /* REG_LOWER_H */
