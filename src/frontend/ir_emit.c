/* ir_emit.c - Reachability DCE and Pass 2 IR-to-backend lowering */
#include "internal.h"
#if IR4_ENABLE_65816
#include "backend/65816.h"
#endif
#if IR4_ENABLE_ARM64 || IR4_ENABLE_RV32
#include "backend/reg_lower.h"
#endif
#include "codebuf.h"

#define HOT_CODE_INLINE_MAX 16

typedef struct {
    size_t patch_offset;
    size_t target_ir;
} PendingBr;

typedef struct {
    size_t call_pc;
    size_t call_addr;
    size_t fn_idx;
    Bool is_fn_ptr;
} FwdCall;

typedef struct {
    size_t patch_offset;
    size_t idx;
    size_t count;
    size_t target_ir;
} PendingJtab;

VEC_OF(FwdCallVec, FwdCall);
VEC_OF(PendingBrVec, PendingBr);
VEC_OF(PendingJtabVec, PendingJtab);
VEC_OF(U8Vec, u8);

static FwdCallVec fwd_calls;
static U8Vec br_tgt;
static size_t heap_start_off = 0;

void ir_emit_reset(void) {
    fwd_calls.len = 0;
    heap_start_off = 0;
}

static Bool can_reach_fn(size_t start, size_t target, Bool *visited, const Bool *addr_taken) {
    SizeVec stack;
    stack.data = NULL;
    stack.len = 0;
    stack.cap = 0;
    VEC_PUSH(stack, start);
    while (stack.len > 0) {
        size_t cur = stack.data[--stack.len];
        FnIR *w = fn_irs.data[cur];
        size_t i;
        if (FN_FLAG(fn_sigs.data[cur], FN_FL_EXTERN) || w->is_code)
            continue;
        fn_ir_ensure(w);
        for (i = 0; i < w->ir.len; i++) {
            const IrInstr *ins = &w->ir.data[i];
            if (ins->kind == IR_CALL) {
                size_t callee = ins->u.fn_idx;
                if (callee == target) {
                    fn_ir_drop(w);
                    free(stack.data);
                    return true;
                }
                if (!visited[callee]) {
                    visited[callee] = true;
                    VEC_PUSH(stack, callee);
                }
            } else if (ins->kind == IR_ICALL) {
                size_t k;
                for (k = 0; k < fn_sigs.len; k++) {
                    if (!addr_taken[k])
                        continue;
                    if (k == target) {
                        fn_ir_drop(w);
                        free(stack.data);
                        return true;
                    }
                    if (!visited[k]) {
                        visited[k] = true;
                        VEC_PUSH(stack, k);
                    }
                }
            }
        }
        fn_ir_drop(w);
    }
    free(stack.data);
    return false;
}

static int can_inline_hot_code(size_t fidx, size_t *out_len) {
    FnSig *sig = fn_sigs.data[fidx];
    FnIR *cw = fn_irs.data[fidx];
    if (!ir4_get_opt_level() || !cw->is_code || !FN_FLAG(sig, FN_FL_HOT) || cw->raw_len == 0 ||
        cw->raw_len > HOT_CODE_INLINE_MAX)
        return 0;
    if (strcmp(be->name, "6502") != 0 && strcmp(be->name, "65816") != 0 &&
        strcmp(be->name, "z80") != 0 && strcmp(be->name, "ez80") != 0)
        return 0;
    *out_len = cw->raw_len;
    return 1;
}

static void mark_reachable(size_t root) {
    SizeVec stack;
    stack.data = NULL;
    stack.len = 0;
    stack.cap = 0;
    VEC_PUSH(stack, root);
    while (stack.len > 0) {
        size_t fidx = stack.data[--stack.len];
        FnIR *w = fn_irs.data[fidx];
        size_t i;
        if (FN_FLAG(fn_sigs.data[fidx], FN_FL_EXTERN))
            continue;
        if (!FN_FLAG(fn_sigs.data[fidx], FN_FL_DEFINED))
            ir4_die("called 'fwd' function was never defined");
        if (w->reachable)
            continue;
        w->reachable = 1;
        if (w->is_code)
            continue;
        fn_ir_ensure(w);
        for (i = 0; i < w->ir.len; i++) {
            const IrInstr *ins = &w->ir.data[i];
            if (ins->kind == IR_CALL) {
                size_t inl_len = 0;
                if (!can_inline_hot_code(ins->u.fn_idx, &inl_len))
                    VEC_PUSH(stack, ins->u.fn_idx);
            } else if (ins->kind == IR_PUSH_FN) {
                VEC_PUSH(stack, ins->u.fn_idx);
            }
        }
        fn_ir_drop(w);
    }
    free(stack.data);
}

static void emit_raw_code_block(size_t raw_start, size_t raw_len, size_t reloc_start,
                                  size_t reloc_cnt) {
    size_t i;
    size_t base_pc = cb_pc();
    size_t base_addr = be->cur_addr();
    for (i = 0; i < raw_len; i++) {
        cb_emit8(raw_code_buf.data[raw_start + i]);
    }
    for (i = 0; i < reloc_cnt; i++) {
        const CodeReloc *rel = &code_relocs.data[reloc_start + i];
        size_t pc = base_pc + rel->ir_off;
        if (rel->kind == CODE_REL_SELF) {
            cb_patch16(pc, (u16)(base_addr + rel->addend));
        } else if (rel->kind == CODE_REL_STACK) {
#if IR4_ENABLE_6502
            if (be == &be_6502 && be_6502_stack_zp_base() != 0) {
                cb_patch16(pc, (u16)(be_6502_stack_zp_base() + rel->addend));
            } else
#endif
            {
                cb_add_data_reloc(pc, be->stack_data_off() + rel->addend);
            }
        } else if (rel->kind == CODE_REL_DATA) {
            if (be->addr_bytes == 3)
                cb_add_data_reloc24(pc, rel->addend);
            else if (be->addr_bytes == 2)
                cb_add_data_reloc(pc, rel->addend);
            else
                ir4_die("'&' in raw code is only supported on 16-bit and 24-bit targets");
        }
    }
}

static int ir_is_br_target(const FnIR *w, size_t ir_idx) {
    (void)w;
    return br_tgt.data[ir_idx] != 0;
}

static int ir_can_fuse(const FnIR *w, size_t i, size_t extra) {
    size_t k;
    if (!ir4_get_opt_level())
        return 0;
    if (i + extra >= w->ir.len)
        return 0;
    for (k = 1; k <= extra; k++) {
        if (ir_is_br_target(w, i + k))
            return 0;
    }
    return 1;
}

static void emit_push_w_instr(const FnIR *w, size_t *ip, PendingBrVec *pending,
                              const SizeVec *ir_addr) {
    size_t i = *ip;
    const IrInstr *ins = &w->ir.data[i];
    if (ir_can_fuse(w, i, 1) && w->ir.data[i + 1].kind == IR_LOCAL_OP) {
        const IrInstr *nins = &w->ir.data[i + 1];
        if (nins->u.loc.op == LOCAL_ADD && be->local_add_imm &&
            be->local_add_imm(nins->u.loc.slot, nins->u.loc.type, nins->u.loc.stack_bytes,
                              ins->u.w)) {
            *ip = i + 1;
            return;
        }
    }
    if (ir_can_fuse(w, i, 2) && w->ir.data[i + 1].kind == IR_ALU_OP &&
        w->ir.data[i + 2].kind == IR_ALU_OP) {
        IrOp aop = w->ir.data[i + 1].u.op;
        IrOp mop = w->ir.data[i + 2].u.op;
        if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH && mop <= OP_I32STORE &&
            be->mem_imm) {
            i32 w_imm = (be->word_bytes == 2) ? (i32)(i16)(u16)ins->u.w : ins->u.w;
            i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)w_imm) : w_imm;
            if (be->mem_imm(mop, off)) {
                *ip = i + 2;
                return;
            }
        }
    }
    if (ir_can_fuse(w, i, 1) && w->ir.data[i + 1].kind == IR_ALU_OP) {
        IrOp aop = w->ir.data[i + 1].u.op;
        i32 imm = ins->u.w;
        if (aop == OP_ADD || aop == OP_SUB || aop == OP_AADD || aop == OP_ASUB) {
            int is_addr = (aop == OP_AADD || aop == OP_ASUB);
            i32 w_imm = (be->word_bytes == 2) ? (i32)(i16)(u16)imm : imm;
            i32 delta = (aop == OP_SUB || aop == OP_ASUB) ? (i32)(0UL - (u32)w_imm) : w_imm;
            if (be->add_imm && be->add_imm(is_addr, delta)) {
                *ip = i + 1;
                return;
            }
        } else if (aop >= OP_EQ && aop <= OP_AGT) {
            if (ir_can_fuse(w, i, 2)) {
                const IrInstr *bins = &w->ir.data[i + 2];
                if (bins->kind == IR_BR_FWD && bins->u.br.cond == BR_JZ && be->cmp_br_fwd) {
                    size_t patch_off = 0;
                    if (be->cmp_br_fwd(aop, 1, imm, &patch_off)) {
                        PendingBr *pb = VEC_ADD(*pending);
                        pb->patch_offset = patch_off;
                        pb->target_ir = bins->u.br.target;
                        *ip = i + 2;
                        return;
                    }
                } else if (bins->kind == IR_BR_BACK && bins->u.br.cond == BR_JZ &&
                           be->cmp_br_back) {
                    if (be->cmp_br_back(aop, 1, imm, ir_addr->data[bins->u.br.target])) {
                        *ip = i + 2;
                        return;
                    }
                }
            }
            if (aop <= OP_UGE && be->cmp_imm && be->cmp_imm(aop, imm)) {
                *ip = i + 1;
                return;
            }
        } else if (aop == OP_LSHIFT || aop == OP_RSHIFT || aop == OP_ARSHIFT ||
                   aop == OP_SHL32 || aop == OP_RSHR32 || aop == OP_ASHR32) {
            u8 max_sh = (aop >= OP_SHL32 || be->word_bytes == 4) ? 31
                        : (be->word_bytes == 8)                  ? 63
                                                                 : (u8)(be->word_bytes * 8 - 1);
            if (imm >= 0 && imm <= (i32)max_sh && be->shift_imm && be->shift_imm(aop, (u8)imm)) {
                *ip = i + 1;
                return;
            }
        } else if (aop == OP_AND || aop == OP_OR || aop == OP_XOR) {
            if (be->bit_imm && be->bit_imm(aop, imm)) {
                *ip = i + 1;
                return;
            }
        }
    }
    be->push_w(ins->u.w);
}

static void emit_local_instr(const FnIR *w, size_t *ip) {
    size_t i = *ip;
    const IrInstr *ins = &w->ir.data[i];
    int is_addr = (ins->u.loc.op == LOCAL_ADDR && be->local_mem != NULL);
    int is_deref = (ins->u.loc.op == LOCAL_GET && ins->u.loc.type == TY_A &&
                    be->local_deref != NULL);
    if (is_addr || is_deref) {
        int (*fn)(int, i32, IrOp, size_t) = is_addr ? be->local_mem : be->local_deref;
        if (ir_can_fuse(w, i, 3) && w->ir.data[i + 1].kind == IR_PUSH_W &&
            w->ir.data[i + 2].kind == IR_ALU_OP && w->ir.data[i + 3].kind == IR_ALU_OP) {
            IrOp aop = w->ir.data[i + 2].u.op;
            IrOp mop = w->ir.data[i + 3].u.op;
            if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH && mop <= OP_I32STORE) {
                i32 imm = w->ir.data[i + 1].u.w;
                i32 w_imm = (be->word_bytes == 2) ? (i32)(i16)(u16)imm : imm;
                i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)w_imm) : w_imm;
                if (fn(ins->u.loc.slot, off, mop, ins->u.loc.stack_bytes)) {
                    *ip = i + 3;
                    return;
                }
            }
        } else if (ir_can_fuse(w, i, 1) && w->ir.data[i + 1].kind == IR_ALU_OP) {
            IrOp mop = w->ir.data[i + 1].u.op;
            if (mop >= OP_I8FETCH && mop <= OP_I32STORE &&
                fn(ins->u.loc.slot, 0, mop, ins->u.loc.stack_bytes)) {
                *ip = i + 1;
                return;
            }
        }
    }
    be->local_op(ins->u.loc.op, ins->u.loc.slot, ins->u.loc.type, ins->u.loc.stack_bytes);
}

static void emit_call_instr(const FnIR *w, const FnSig *sig, size_t *ip) {
    size_t i = *ip;
    size_t target = w->ir.data[i].u.fn_idx;
    size_t inl_len = 0;
    if (FN_FLAG(fn_sigs.data[target], FN_FL_EXTERN)) {
        be->call_extern(fn_sigs.data[target]);
        return;
    }
    if (can_inline_hot_code(target, &inl_len)) {
        emit_raw_code_block(fn_irs.data[target]->raw_start, inl_len,
                            fn_irs.data[target]->reloc_start, fn_irs.data[target]->reloc_cnt);
        return;
    }
    if (FN_FLAG(fn_sigs.data[target], FN_FL_HAS_ADDR) && be->tail_call && ir_can_fuse(w, i, 1) &&
        w->ir.data[i + 1].kind == IR_FN_END) {
#if IR4_ENABLE_6502
        if (be == &be_6502) {
            be_6502_set_call_bank(fn_sigs.data[target]->bank);
        }
#endif
        if (be->tail_call(fn_sigs.data[target]->addr, sig, fn_sigs.data[target], w->locals.data,
                          (int)w->locals.len)) {
            *ip = i + 1;
            return;
        }
    }
    {
        size_t call_addr = be->cur_addr();
        size_t call_pc = cb_pc();
#if IR4_ENABLE_6502
        if (be == &be_6502) {
            be_6502_set_call_bank(fn_sigs.data[target]->bank);
        }
#endif
        be->call(fn_sigs.data[target]->addr);
        if (!FN_FLAG(fn_sigs.data[target], FN_FL_HAS_ADDR)) {
            FwdCall *fc = VEC_ADD(fwd_calls);
            fc->call_pc = call_pc;
            fc->call_addr = call_addr;
            fc->fn_idx = target;
            fc->is_fn_ptr = 0;
        }
    }
}

static void emit_fn(size_t fidx) {
    static SizeVec ir_addr;
    FnSig *sig = fn_sigs.data[fidx];
    FnIR *w = fn_irs.data[fidx];
    PendingBrVec pending;
    PendingJtabVec pending_jtab;
    size_t i, p;

    pending.data = NULL;
    pending.len = 0;
    pending.cap = 0;
    pending_jtab.data = NULL;
    pending_jtab.len = 0;
    pending_jtab.cap = 0;

    if (w->is_code) {
        if (strcmp(be->name, "65816") == 0) {
            size_t cstart = be->cur_addr();
            if ((cstart >> 16) != ((cstart + w->raw_len + 1) >> 16)) {
                size_t pad = 0x10000UL - (cstart & 0xFFFFUL);
                while (pad-- > 0)
                    cb_emit8(0);
            }
        }
        sig->addr = be->cur_addr();
        FN_SET_FLAG(sig, FN_FL_HAS_ADDR);
        emit_raw_code_block(w->raw_start, w->raw_len, w->reloc_start, w->reloc_cnt);
        if (strcmp(be->name, "6502") == 0)
            cb_emit8(0x60); /* RTS */
        else if (strcmp(be->name, "65816") == 0)
            cb_emit8(0x6B); /* RTL */
        else if (strcmp(be->name, "z80") == 0 || strcmp(be->name, "ez80") == 0)
            cb_emit8(0xC9); /* RET */
        else if (strcmp(be->name, "rv32") == 0)
            cb_emit32(0x00008067UL); /* RET (JALR x0, 0(ra)) */
        else
            ir4_die("'code' is not supported on this backend");
        return;
    }

    fn_ir_ensure(w);

#if IR4_ENABLE_ARM64 || IR4_ENABLE_RV32
    {
        const RegBackend *rbe = NULL;
#if IR4_ENABLE_ARM64
        if (strcmp(be->name, "arm64") == 0)
            rbe = &reg_be_arm64;
#endif
#if IR4_ENABLE_RV32
        if (strcmp(be->name, "rv32") == 0)
            rbe = &reg_be_rv32;
#endif
        if (rbe && ir4_get_opt_level() > 0) {
            RegFwdCallVec reg_fwd;
            size_t rf;
            reg_fwd.data = NULL;
            reg_fwd.len = 0;
            reg_fwd.cap = 0;
            reg_lower_fn(rbe, sig, w, &fn_sigs, &fn_irs, &jtab_targets, heap_start_off, &reg_fwd);
            for (rf = 0; rf < reg_fwd.len; rf++) {
                FwdCall fc;
                fc.call_pc = reg_fwd.data[rf].call_pc;
                fc.call_addr = reg_fwd.data[rf].call_addr;
                fc.fn_idx = reg_fwd.data[rf].fn_idx;
                fc.is_fn_ptr = reg_fwd.data[rf].is_fn_ptr;
                VEC_PUSH(fwd_calls, fc);
            }
            free(reg_fwd.data);
            fn_ir_drop(w);
            return;
        }
    }
#endif

    VEC_ENSURE(ir_addr, w->ir.len + 1);
    VEC_ENSURE(br_tgt, w->ir.len + 1);
    memset(br_tgt.data, 0, w->ir.len + 1);
    for (i = 0; i < w->ir.len; i++) {
        const IrInstr *bi = &w->ir.data[i];
        if ((bi->kind == IR_BR_FWD || bi->kind == IR_BR_BACK) && (size_t)bi->u.br.target <= w->ir.len) {
            br_tgt.data[bi->u.br.target] = 1;
        } else if (bi->kind == IR_JTAB) {
            size_t k;
            for (k = 0; k < (size_t)bi->u.jtab.count; k++) {
                size_t tir = jtab_targets.data[bi->u.jtab.start + k];
                if (tir <= w->ir.len)
                    br_tgt.data[tir] = 1;
            }
        }
    }
    {
        int retried = 0;
        int is_816 = (strcmp(be->name, "65816") == 0);
#if IR4_ENABLE_65816
        if (is_816) {
            (void)be_65816_rstack_off();
            be_65816_relax_reset();
        }
#endif
    retry_emit:
        {
            CbCheckpoint cp = cb_checkpoint();
            size_t fc_len = fwd_calls.len;
            size_t start_addr = be->cur_addr();
            sig->addr = start_addr;
            FN_SET_FLAG(sig, FN_FL_HAS_ADDR);
            be->fn_begin(sig->name, w->param_cnt, w->locals.data, (int)w->locals.len,
                         FN_FLAG(sig, FN_FL_RECURSIVE));

    for (i = 0; i <= w->ir.len; i++) {
        if (be->peep_fence && ir_is_br_target(w, i))
            be->peep_fence();
        p = 0;
        while (p < pending.len) {
            if (pending.data[p].target_ir == i) {
                be->patch_fwd(pending.data[p].patch_offset);
                pending.data[p] = pending.data[--pending.len];
            } else {
                p++;
            }
        }
        p = 0;
        while (p < pending_jtab.len) {
            if (pending_jtab.data[p].target_ir == i) {
                be->patch_jtab(pending_jtab.data[p].patch_offset, pending_jtab.data[p].idx,
                               pending_jtab.data[p].count, be->cur_addr());
                pending_jtab.data[p] = pending_jtab.data[--pending_jtab.len];
            } else {
                p++;
            }
        }
        if (i == w->ir.len)
            break;
        ir_addr.data[i] = be->cur_addr();
        {
            const IrInstr *ins = &w->ir.data[i];
            switch (ins->kind) {
            case IR_FN_END:
                be->fn_end(sig, w->locals.data, (int)w->locals.len);
                break;
            case IR_PUSH_W:
                emit_push_w_instr(w, &i, &pending, &ir_addr);
                break;
            case IR_PUSH_A:
                be->push_a(ins->u.a);
                break;
            case IR_PUSH_F:
                be->push_f(ins->u.f);
                break;
            case IR_PUSH_I32:
                be->push_i32(ins->u.w);
                break;
            case IR_PUSH_DATA:
                if (ir_can_fuse(w, i, 1) &&
                    w->ir.data[i + 1].kind == IR_ALU_OP &&
                    be->data_mem &&
                    be->data_mem(w->ir.data[i + 1].u.op, ins->u.offset)) {
                    i++;
                    break;
                }
                be->push_data(ins->u.offset);
                break;
            case IR_PUSH_HEAP_START: {
                size_t off = CB_HEAP_START_OFF;
#if IR4_ENABLE_ARM64
                if (be == &be_arm64) off = heap_start_off;
#endif
#if IR4_ENABLE_RV32
                if (be == &be_rv32) off = heap_start_off;
#endif
                be->push_data(off);
                break;
            }
            case IR_LOCAL_OP:
                emit_local_instr(w, &i);
                break;
            case IR_STACK_OP:
                be->stack_op(ins->u.stk.op, ins->u.stk.t1, ins->u.stk.t2);
                break;
            case IR_ALU_OP:
                if (ins->u.op >= OP_EQ && ins->u.op <= OP_AGT && ir_can_fuse(w, i, 1)) {
                    const IrInstr *bins = &w->ir.data[i + 1];
                    if (bins->kind == IR_BR_FWD && bins->u.br.cond == BR_JZ && be->cmp_br_fwd) {
                        size_t patch_off = 0;
                        if (be->cmp_br_fwd(ins->u.op, 0, 0, &patch_off)) {
                            PendingBr *pb = VEC_ADD(pending);
                            pb->patch_offset = patch_off;
                            pb->target_ir = bins->u.br.target;
                            i++;
                            break;
                        }
                    } else if (bins->kind == IR_BR_BACK && bins->u.br.cond == BR_JZ &&
                               be->cmp_br_back) {
                        if (be->cmp_br_back(ins->u.op, 0, 0, ir_addr.data[bins->u.br.target])) {
                            i++;
                            break;
                        }
                    }
                }
                be->op(ins->u.op);
                break;
            case IR_CALL:
                emit_call_instr(w, sig, &i);
                break;
            case IR_PUSH_FN: {
                size_t target = ins->u.fn_idx;
                size_t fn_addr = be->cur_addr();
                size_t fn_pc = be->push_fn(fn_sigs.data[target]->addr);
                if (!FN_FLAG(fn_sigs.data[target], FN_FL_HAS_ADDR)) {
                    FwdCall *fc = VEC_ADD(fwd_calls);
                    fc->call_pc = fn_pc;
                    fc->call_addr = fn_addr;
                    fc->fn_idx = target;
                    fc->is_fn_ptr = 1;
                }
                break;
            }
            case IR_ICALL:
                be->icall();
                break;
            case IR_CALL_EXTERN:
                be->call_extern(fn_sigs.data[ins->u.fn_idx]);
                break;
            case IR_BR_FWD: {
                PendingBr *pb = VEC_ADD(pending);
                pb->patch_offset = be->emit_br_fwd(ins->u.br.cond);
                pb->target_ir = ins->u.br.target;
                break;
            }
            case IR_BR_BACK:
                be->emit_br_back(ins->u.br.cond, ir_addr.data[ins->u.br.target]);
                break;
            case IR_JTAB: {
                size_t tbl_off, k;
                if (!be->emit_jtab || !be->patch_jtab)
                    ir4_die("'jtab' is not supported on this backend");
                tbl_off = be->emit_jtab(ins->u.jtab.count);
                for (k = 0; k < (size_t)ins->u.jtab.count; k++) {
                    size_t tir = jtab_targets.data[ins->u.jtab.start + k];
                    if (tir <= i) {
                        be->patch_jtab(tbl_off, k, ins->u.jtab.count, ir_addr.data[tir]);
                    } else {
                        PendingJtab *pj = VEC_ADD(pending_jtab);
                        pj->patch_offset = tbl_off;
                        pj->idx = k;
                        pj->count = ins->u.jtab.count;
                        pj->target_ir = tir;
                    }
                }
                break;
            }
            }
        }
    }
#if IR4_ENABLE_65816
            if (is_816 && be_65816_need_relax()) {
                cb_restore(cp);
                be_65816_invalidate_from(start_addr);
                fwd_calls.len = fc_len;
                pending.len = 0;
                pending_jtab.len = 0;
                goto retry_emit;
            }
#endif
            if (is_816 && !retried && be->cur_addr() > start_addr &&
                ((start_addr >> 16) != (be->cur_addr() >> 16))) {
                size_t pad = 0x10000UL - (start_addr & 0xFFFFUL);
                cb_restore(cp);
#if IR4_ENABLE_65816
                be_65816_invalidate_from(start_addr);
#endif
                fwd_calls.len = fc_len;
                pending.len = 0;
                pending_jtab.len = 0;
                while (pad-- > 0)
                    cb_emit8(0);
                retried = 1;
                goto retry_emit;
            }
            if (is_816 && be->cur_addr() > start_addr &&
                ((start_addr >> 16) != (be->cur_addr() >> 16))) {
                ir4_status_clear();
                fprintf(stderr, "ir4c: function '%s' (%lu bytes) exceeds 64 KB 65816 bank limit\n",
                        sig->name, (unsigned long)(be->cur_addr() - start_addr));
                exit(1);
            }
        }
    }
    free(pending.data);
    free(pending_jtab.data);
    fn_ir_drop(w);
}

void run_reachability_dce(void) {
    int main_idx = ir4_find_fn("main");
    size_t f;
    Bool *visited;

    if (main_idx >= 0 && init_fns.len > 0) {
        size_t wrap_idx;
        FnSig *wsig;
        FnIR *ww;
        IrInstr *ins;
        size_t k;
        fn_sigs.data[main_idx]->name = intern_str("__main_body");
        register_fn_sym(fn_sigs.data[main_idx]->name, (size_t)main_idx);
        wrap_idx = alloc_fn(&wsig, &ww);
        wsig->name = intern_str("main");
        FN_SET_FLAG(wsig, FN_FL_DEFINED);
        register_fn_sym(wsig->name, wrap_idx);
        {
            FnSig *msig = fn_sigs.data[main_idx];
            fn_sig_set_types(wsig, NULL, 0,
                             msig->out_cnt ? (FN_TYPES_PTR(msig) + msig->in_cnt) : NULL,
                             msig->out_cnt);
        }
        for (k = 0; k < init_fns.len; k++) {
            ins = emit_ir(IR_CALL, ww);
            ins->u.fn_idx = init_fns.data[k];
        }
        ins = emit_ir(IR_CALL, ww);
        ins->u.fn_idx = (size_t)main_idx;
        emit_ir(IR_FN_END, ww);
        main_idx = (int)wrap_idx;
    }
    if (main_idx >= 0) {
        size_t di;
        ir4_status("Analyzing reachability...");
        mark_reachable((size_t)main_idx);
        for (di = 0; di < cb_data_init_reloc_count(); di++) {
            size_t po, tv;
            u8 ab;
            Bool is_c;
            cb_get_data_init_reloc(di, &po, &tv, &ab, &is_c);
            if (is_c)
                mark_reachable(tv);
        }
    } else {
        for (f = 0; f < fn_sigs.len; f++) {
            if (!FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) && FN_FLAG(fn_sigs.data[f], FN_FL_DEFINED))
                fn_irs.data[f]->reachable = true;
        }
    }

    /* Free IR and local metadata of unreachable functions before Pass 2 lowering */
    for (f = 0; f < fn_sigs.len; f++) {
        FnIR *w = fn_irs.data[f];
        if (!w->reachable && !w->is_code && !FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN)) {
            free_sig_vecs(fn_sigs.data[f]);
            free(w->ir.data);
            w->ir.data = NULL;
            w->ir.len = 0;
            w->ir.cap = 0;
            free(w->locals.data);
            w->locals.data = NULL;
            w->locals.len = 0;
            w->locals.cap = 0;
        }
    }

    {
        Bool *addr_taken = (Bool *)vec_calloc(fn_sigs.len ? fn_sigs.len : 1, sizeof(Bool));
        size_t di;
        for (di = 0; di < cb_data_init_reloc_count(); di++) {
            size_t po, tv;
            u8 ab;
            Bool is_c;
            cb_get_data_init_reloc(di, &po, &tv, &ab, &is_c);
            if (is_c && tv < fn_sigs.len)
                addr_taken[tv] = true;
        }
        for (f = 0; f < fn_sigs.len; f++) {
            FnIR *w = fn_irs.data[f];
            size_t i;
            if (!w->reachable || FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) || w->is_code)
                continue;
            for (i = 0; i < w->ir.len; i++) {
                const IrInstr *ins = &w->ir.data[i];
                if (ins->kind == IR_PUSH_FN && (size_t)ins->u.fn_idx < fn_sigs.len)
                    addr_taken[ins->u.fn_idx] = true;
            }
        }
        visited = (Bool *)vec_calloc(fn_sigs.len ? fn_sigs.len : 1, sizeof(Bool));
        for (f = 0; f < fn_sigs.len; f++) {
            if (fn_irs.data[f]->reachable) {
                memset(visited, 0, fn_sigs.len * sizeof(Bool));
                if (can_reach_fn(f, f, visited, addr_taken))
                    FN_SET_FLAG(fn_sigs.data[f], FN_FL_RECURSIVE);
                else
                    FN_CLR_FLAG(fn_sigs.data[f], FN_FL_RECURSIVE);
            }
        }
        free(visited);
        free(addr_taken);
    }
}

static u16 assign_code_banks(void) {
    u16 max_bank = 0;
    u32 soft_cap = (u32)(cb_bank_size() > 4096 ? cb_bank_size() - 4096 : cb_bank_size());
    u16 cur_b = 1;
    u32 cur_sz = 0;
    size_t f;

    for (f = 0; f < fn_sigs.len; f++) {
        FnSig *sig = fn_sigs.data[f];
        FnIR *w = fn_irs.data[f];
        if (FN_FLAG(sig, FN_FL_EXTERN) || !w->reachable) {
            sig->bank = 0;
            continue;
        }
        if (FN_FLAG(sig, FN_FL_HOT) || w->is_code || strcmp(sig->name, "main") == 0 ||
            strcmp(sig->name, "init") == 0) {
            sig->bank = 0;
        } else {
            u32 est_sz = (u32)(w->ir.len * 64);
            if (est_sz == 0)
                est_sz = 16;
            if (cur_sz > 0 && cur_sz + est_sz > soft_cap) {
                cur_b++;
                cur_sz = 0;
            }
            sig->bank = cur_b;
            cur_sz += est_sz;
            if (cur_b > max_bank)
                max_bank = cur_b;
        }
    }
    return max_bank;
}

#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
static int get_be_compact(void) {
#if IR4_ENABLE_6502
    if (be == &be_6502)
        return be_6502_get_compact();
#endif
#if IR4_ENABLE_EZ80
    if (be == &be_ez80)
        return be_ez80_get_compact();
#endif
    return 0;
}

static void set_be_compact(int c) {
#if IR4_ENABLE_6502
    if (be == &be_6502)
        be_6502_set_compact(c);
#endif
#if IR4_ENABLE_EZ80
    if (be == &be_ez80)
        be_ez80_set_compact(c);
#endif
}

static Bool *select_compact_fns(void) {
    Bool *fn_compact = NULL;
    u32 *est = (u32 *)vec_calloc(fn_sigs.len, sizeof(u32));
    u32 *heat = (u32 *)vec_calloc(fn_sigs.len, sizeof(u32));
    u32 total_est = 0;
    u32 compact_thresh = 10240;
    u32 native_budget = 6144;
    size_t max_ir = 1;
    size_t f;

#if IR4_ENABLE_EZ80
    if (be == &be_ez80 && (cb_cur_addr() - cb_pc()) < 0x0B0000UL) {
        free(est);
        free(heat);
        return NULL;
    }
#endif

    for (f = 0; f < fn_sigs.len; f++) {
        if (!FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) && fn_irs.data[f]->reachable &&
            !fn_irs.data[f]->is_code) {
            size_t ilen = fn_ir_len(fn_irs.data[f]);
            est[f] = (u32)(ilen * 10);
            total_est += est[f];
            if (ilen > max_ir)
                max_ir = ilen;
        }
    }
    if (total_est > compact_thresh) {
        u8 *depth = (u8 *)vec_calloc(max_ir, sizeof(u8));
        int pass;
        fn_compact = (Bool *)vec_calloc(fn_sigs.len, sizeof(Bool));
        for (f = 0; f < fn_sigs.len; f++) {
            FnIR *w = fn_irs.data[f];
            size_t i;
            if (FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) || !w->reachable || w->is_code)
                continue;
            fn_ir_ensure(w);
            fn_compact[f] = !FN_FLAG(fn_sigs.data[f], FN_FL_HOT);
            if (FN_FLAG(fn_sigs.data[f], FN_FL_HOT))
                heat[f] += 10000;
            if (FN_FLAG(fn_sigs.data[f], FN_FL_RECURSIVE))
                heat[f] += 100;
            memset(depth, 0, w->ir.len);
            for (i = 0; i < w->ir.len; i++) {
                const IrInstr *ins = &w->ir.data[i];
                if (ins->kind == IR_BR_BACK) {
                    size_t k;
                    for (k = ins->u.br.target; k <= i; k++) {
                        if (depth[k] < 255)
                            depth[k]++;
                    }
                }
            }
            for (i = 0; i < w->ir.len; i++) {
                const IrInstr *ins = &w->ir.data[i];
                u32 wgt = (depth[i] == 0) ? 0 : (depth[i] == 1 ? 10 : 100);
                heat[f] += wgt;
                if (ins->kind == IR_CALL && wgt > 0) {
                    heat[ins->u.fn_idx] += wgt * 4;
                }
            }
            fn_ir_drop(w);
        }
        /* Propagate loop heat one hop to callees of hot functions */
        for (pass = 0; pass < 2; pass++) {
            for (f = 0; f < fn_sigs.len; f++) {
                FnIR *w = fn_irs.data[f];
                size_t i;
                if (FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) || !w->reachable || w->is_code ||
                    heat[f] == 0)
                    continue;
                fn_ir_ensure(w);
                for (i = 0; i < w->ir.len; i++) {
                    const IrInstr *ins = &w->ir.data[i];
                    if (ins->kind == IR_CALL && heat[ins->u.fn_idx] < heat[f]) {
                        heat[ins->u.fn_idx] += heat[f] / 2;
                    }
                }
                fn_ir_drop(w);
            }
        }
        /* Greedily keep hottest functions (by heat/byte) native up to budget */
        for (;;) {
            size_t best_f = fn_sigs.len;
            u32 best_score = 0;
            for (f = 0; f < fn_sigs.len; f++) {
                u32 score;
                if (!fn_compact[f] || heat[f] == 0 || est[f] > native_budget)
                    continue;
                score = (heat[f] * 100) / (est[f] ? est[f] : 1);
                if (score > best_score) {
                    best_score = score;
                    best_f = f;
                }
            }
            if (best_f == fn_sigs.len)
                break;
            fn_compact[best_f] = false;
            native_budget -= est[best_f];
        }
        free(depth);
    }
    free(est);
    free(heat);
    return fn_compact;
}
#endif

void emit_program(void) {
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
    int saved_compact = get_be_compact();
#endif
    Bool *fn_compact = NULL;
    u16 max_bank = 0;
    size_t f;
    size_t c;

#if IR4_ENABLE_6502
    if (be == &be_6502)
        (void)be_6502_stack_data_off();
#endif
    heap_start_off = cb_alloc_data(0, be->word_bytes);

    if (cb_is_banked() && fn_sigs.len > 0) {
        max_bank = assign_code_banks();
    }
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
    else if (saved_compact == 2 && fn_sigs.len > 0) {
        fn_compact = select_compact_fns();
    }
#endif

    if (cb_is_banked()) {
        u16 b;
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
        set_be_compact(0);
#endif
        for (b = 0; b <= max_bank; b++) {
            if (b > 0)
                cb_begin_bank(b);
            for (f = 0; f < fn_sigs.len; f++) {
                if (!FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) && fn_irs.data[f]->reachable &&
                    fn_sigs.data[f]->bank == b) {
                    emit_fn(f);
                }
            }
#if IR4_ENABLE_6502
            if (b == 0 && max_bank > 0 && be == &be_6502) {
                int need_mul = 0, need_div = 0, need_icall = 0, need_fp = 0;
                for (f = 0; f < fn_sigs.len; f++) {
                    FnIR *w = fn_irs.data[f];
                    size_t i;
                    if (FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) || !w->reachable ||
                        fn_sigs.data[f]->bank == 0)
                        continue;
                    fn_ir_ensure(w);
                    for (i = 0; i < w->ir.len; i++) {
                        const IrInstr *ins = &w->ir.data[i];
                        if (ins->kind == IR_ICALL) {
                            need_icall = 1;
                        } else if (ins->kind == IR_ALU_OP) {
                            IrOp op = ins->u.op;
                            if (op == OP_MUL)
                                need_mul = 1;
                            else if (op == OP_DIV || op == OP_MOD || op == OP_UDIV || op == OP_UMOD)
                                need_div = 1;
                            else if ((op >= OP_FADD && op <= OP_FDIV) ||
                                     (op >= OP_FEQ && op <= OP_FGT) || op == OP_W2F ||
                                     op == OP_F2W)
                                need_fp = 1;
                        }
                    }
                    fn_ir_drop(w);
                }
                be_6502_ensure_helpers(need_mul, need_div, need_icall, need_fp);
            }
#endif
        }
        cb_end_banks();
    } else {
        int reach_total = 0;
        int reach_done = 0;
        for (f = 0; f < fn_sigs.len; f++) {
            if (!FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) && fn_irs.data[f]->reachable)
                reach_total++;
        }
        ir4_status_prog("Generating code", 0, reach_total);
        for (f = 0; f < fn_sigs.len; f++) {
            if (!FN_FLAG(fn_sigs.data[f], FN_FL_EXTERN) && fn_irs.data[f]->reachable) {
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
                if (FN_FLAG(fn_sigs.data[f], FN_FL_HOT))
                    set_be_compact(0);
                else if (fn_compact)
                    set_be_compact(fn_compact[f] ? 1 : 0);
                else if (saved_compact == 2)
                    set_be_compact(0);
#endif
                emit_fn(f);
                reach_done++;
                if ((reach_done & 15) == 0 || reach_done == reach_total)
                    ir4_status_prog("Generating code", reach_done, reach_total);
            }
        }
    }
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
    set_be_compact(saved_compact);
#endif
    free(fn_compact);

    ir4_status("Patching relocations...");
    for (c = 0; c < fwd_calls.len; c++) {
        if (fwd_calls.data[c].is_fn_ptr) {
            be->patch_fn(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                         fn_sigs.data[fwd_calls.data[c].fn_idx]->addr);
        } else {
            be->patch_call(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                           fn_sigs.data[fwd_calls.data[c].fn_idx]->addr);
        }
    }
    for (c = 0; c < cb_data_init_reloc_count(); c++) {
        size_t po, tv;
        u8 ab;
        Bool is_c;
        cb_get_data_init_reloc(c, &po, &tv, &ab, &is_c);
        if (is_c) {
            cb_patch_data_init_reloc(c, fn_sigs.data[tv]->addr);
        }
    }
    fn_ir_cleanup();
}
