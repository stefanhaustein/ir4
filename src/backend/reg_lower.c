/* reg_lower.c - Shared depth-indexed register-stack lowering for RISC backends */
#include "backend/reg_lower.h"
#include "codebuf.h"
#include "frontend/internal.h"

typedef enum {
    VK_REG = 0,
    VK_CONST,
    VK_UCONST,
    VK_FCONST,
    VK_LOCAL,
    VK_DATA,
    VK_LADDR
} VKind;

typedef struct {
    StackType type;
    VKind kind;
    i32 imm;
    u32 uimm;
    double fval;
    size_t idx; /* local slot or data offset */
} VSlot;

typedef struct {
    size_t patch_offset;
    size_t target_ir;
} RegPendingBr;

typedef struct {
    size_t patch_offset;
    size_t idx;
    size_t count;
    size_t target_ir;
} RegPendingJtab;

VEC_OF(RegPendingBrVec, RegPendingBr);
VEC_OF(RegPendingJtabVec, RegPendingJtab);

#define MAX_VSTACK 256

typedef struct {
    u8 valid;
    size_t depth;
    StackType types[MAX_VSTACK];
} SavedStack;

static void check_vstack_push(size_t depth) {
    if (depth >= MAX_VSTACK)
        ir4_die("expression stack depth exceeds register backend limit");
}

static StackType alu_result_type(IrOp op) {
    if (op >= OP_FADD && op <= OP_FNEG)
        return TY_F;
    if (op == OP_W2F)
        return TY_F;
    if (op == OP_W2A || op == OP_AADD || op == OP_ASUB)
        return TY_A;
    if (op == OP_W2I32 || op == OP_U2I32 || (op >= OP_ADD32 && op <= OP_ASHR32))
        return TY_I32;
    if (op == OP_AFETCH)
        return TY_A;
    if (op == OP_FFETCH)
        return TY_F;
    if (op == OP_I32FETCH)
        return TY_I32;
    return TY_W;
}

static int op_arity(IrOp op) {
    if (op == OP_BCOPY)
        return 3;
    if (op == OP_INVERT || op == OP_INV32 || op == OP_FNEG ||
        op == OP_SEXT || op == OP_W2A || op == OP_A2W ||
        op == OP_W2F || op == OP_F2W || op == OP_W2I32 ||
        op == OP_U2I32 || op == OP_I322W ||
        (op >= OP_I8FETCH && op <= OP_I32FETCH &&
         op != OP_I8STORE && op != OP_I16STORE && op != OP_STORE &&
         op != OP_ASTORE && op != OP_FSTORE && op != OP_I32STORE))
        return 1;
    return 2;
}

static int is_mem_store(IrOp op) {
    switch (op) {
    case OP_I8STORE:
    case OP_I16STORE:
    case OP_STORE:
    case OP_ASTORE:
    case OP_FSTORE:
    case OP_I32STORE:
    case OP_SI8STORE:
    case OP_SI16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SFSTORE:
        return 1;
    default:
        return 0;
    }
}

static int is_swapped_store(IrOp op) {
    return op >= OP_SI8STORE && op <= OP_SFSTORE;
}

static int is_mem_load(IrOp op) {
    switch (op) {
    case OP_I8FETCH:
    case OP_U8FETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_FETCH:
    case OP_AFETCH:
    case OP_FFETCH:
    case OP_I32FETCH:
        return 1;
    default:
        return 0;
    }
}

static int is_fp_alu(IrOp op) {
    return (op >= OP_FADD && op <= OP_FNEG) ||
           (op >= OP_FEQ && op <= OP_FGT) ||
           op == OP_W2F || op == OP_F2W;
}

typedef struct {
    const RegBackend *rbe;
    VSlot stk[MAX_VSTACK];
    size_t depth;
    u8 *loc_is_reg;
    u8 *loc_reg;
    i32 *loc_off;
    i32 local_bytes;
} RegState;

static int slot_in_reg(const RegState *st, size_t idx) {
    return idx < st->rbe->num_stk_regs;
}

static u8 slot_reg(const RegState *st, size_t idx, StackType type) {
    if (st->rbe->has_fp_regs && type == TY_F)
        return st->rbe->fp_stk_regs[idx];
    return st->rbe->stk_regs[idx];
}

static i32 slot_spill_off(const RegState *st, size_t idx) {
    return st->local_bytes + (i32)((idx + 1) * st->rbe->word_bytes);
}

static void store_to_slot(const RegState *st, size_t idx, u8 src_reg, StackType type) {
    if (slot_in_reg(st, idx)) {
        u8 dst = slot_reg(st, idx, type);
        if (dst != src_reg)
            st->rbe->mov_reg(dst, src_reg, type);
    } else {
        st->rbe->spill_store(src_reg, type, slot_spill_off(st, idx));
    }
}

static void load_from_slot(const RegState *st, size_t idx, u8 dst_reg, StackType type) {
    if (slot_in_reg(st, idx)) {
        u8 src = slot_reg(st, idx, type);
        if (src != dst_reg)
            st->rbe->mov_reg(dst_reg, src, type);
    } else {
        st->rbe->spill_load(dst_reg, type, slot_spill_off(st, idx));
    }
}

static void materialize_slot(RegState *st, size_t idx) {
    VSlot *v = &st->stk[idx];
    u8 dst;
    int in_reg = slot_in_reg(st, idx);

    if (v->kind == VK_REG)
        return;

    dst = in_reg ? slot_reg(st, idx, v->type)
                 : ((st->rbe->has_fp_regs && v->type == TY_F) ? st->rbe->fp_scratch : st->rbe->scratch0);

    switch (v->kind) {
    case VK_CONST:
        st->rbe->mov_imm(dst, v->imm);
        break;
    case VK_UCONST:
        st->rbe->mov_uimm32(dst, v->uimm);
        break;
    case VK_FCONST:
        if (st->rbe->has_fp_regs) {
            st->rbe->mov_fconst(dst, v->fval);
        } else {
            st->rbe->mov_fconst(dst, v->fval);
        }
        break;
    case VK_LOCAL:
        if (st->loc_is_reg[v->idx]) {
            if (dst != st->loc_reg[v->idx])
                st->rbe->mov_reg(dst, st->loc_reg[v->idx], v->type);
        } else {
            st->rbe->spill_load(dst, v->type, st->loc_off[v->idx]);
        }
        break;
    case VK_DATA:
        st->rbe->load_data_addr(dst, v->idx);
        break;
    case VK_LADDR:
        st->rbe->local_addr(dst, st->loc_off[v->idx]);
        break;
    case VK_REG:
        break;
    }

    if (!in_reg)
        st->rbe->spill_store(dst, v->type, slot_spill_off(st, idx));

    v->kind = VK_REG;
}

static void flush_all(RegState *st) {
    size_t i;
    for (i = 0; i < st->depth; i++)
        materialize_slot(st, i);
}

static void flush_local_refs(RegState *st, size_t local_slot) {
    size_t i;
    for (i = 0; i < st->depth; i++) {
        if (st->stk[i].kind == VK_LOCAL && st->stk[i].idx == local_slot)
            materialize_slot(st, i);
    }
}

static u8 read_slot_to_reg(RegState *st, size_t idx, u8 fallback_reg) {
    const VSlot *v = &st->stk[idx];
    if (v->kind == VK_REG && slot_in_reg(st, idx))
        return slot_reg(st, idx, v->type);
    if (v->kind == VK_LOCAL && st->loc_is_reg[v->idx])
        return st->loc_reg[v->idx];
    if (v->kind == VK_REG) {
        st->rbe->spill_load(fallback_reg, v->type, slot_spill_off(st, idx));
        return fallback_reg;
    }
    if (v->kind == VK_CONST) {
        st->rbe->mov_imm(fallback_reg, v->imm);
        return fallback_reg;
    }
    if (v->kind == VK_UCONST) {
        st->rbe->mov_uimm32(fallback_reg, v->uimm);
        return fallback_reg;
    }
    if (v->kind == VK_FCONST) {
        st->rbe->mov_fconst(fallback_reg, v->fval);
        return fallback_reg;
    }
    if (v->kind == VK_LOCAL) {
        st->rbe->spill_load(fallback_reg, v->type, st->loc_off[v->idx]);
        return fallback_reg;
    }
    if (v->kind == VK_DATA) {
        st->rbe->load_data_addr(fallback_reg, v->idx);
        return fallback_reg;
    }
    st->rbe->local_addr(fallback_reg, st->loc_off[v->idx]);
    return fallback_reg;
}

static u8 dst_reg_for(const RegState *st, size_t idx, StackType type) {
    if (slot_in_reg(st, idx))
        return slot_reg(st, idx, type);
    return (st->rbe->has_fp_regs && type == TY_F) ? st->rbe->fp_scratch : st->rbe->scratch0;
}

static void commit_dst(RegState *st, size_t idx, StackType type, u8 dst_reg) {
    st->stk[idx].type = type;
    st->stk[idx].kind = VK_REG;
    if (!slot_in_reg(st, idx))
        st->rbe->spill_store(dst_reg, type, slot_spill_off(st, idx));
}

static void save_live_below(RegState *st, size_t keep_below) {
    size_t i;
    size_t lim = keep_below < st->rbe->num_stk_regs ? keep_below : st->rbe->num_stk_regs;
    for (i = 0; i < lim; i++) {
        materialize_slot(st, i);
        st->rbe->spill_store(slot_reg(st, i, st->stk[i].type), st->stk[i].type,
                             slot_spill_off(st, i));
    }
    for (i = lim; i < keep_below; i++)
        materialize_slot(st, i);
}

static void restore_live_below(RegState *st, size_t keep_below) {
    size_t i;
    size_t lim = keep_below < st->rbe->num_stk_regs ? keep_below : st->rbe->num_stk_regs;
    for (i = 0; i < lim; i++) {
        st->rbe->spill_load(slot_reg(st, i, st->stk[i].type), st->stk[i].type,
                            slot_spill_off(st, i));
        st->stk[i].kind = VK_REG;
    }
}

static void save_branch_state(SavedStack *saved, size_t target_ir, size_t ir_len,
                              const RegState *st) {
    size_t i;
    if (target_ir > ir_len)
        return;
    saved[target_ir].valid = 1;
    saved[target_ir].depth = st->depth;
    for (i = 0; i < st->depth && i < MAX_VSTACK; i++)
        saved[target_ir].types[i] = st->stk[i].type;
}

static void prescan_word(const RegBackend *rbe, const FuncSig *sig, const WordIR *w,
                         const FuncVec *funcs, u8 *loc_addr_taken, size_t *out_max_depth) {
    size_t i;
    size_t depth = 0;
    size_t max_depth = sig->out_types.len;
    StackType types[MAX_VSTACK];
    SavedStack *saved = (SavedStack *)calloc(w->ir.len + 1, sizeof(SavedStack));
    int dead = 0;

    if (!saved)
        ir4_die("out of memory in reg_lower prescan");

    for (i = 0; i < w->ir.len; i++) {
        const IrInstr *ins = &w->ir.data[i];
        if (ins->kind == IR_LOCAL_OP && ins->u.loc.op == LOCAL_ADDR)
            loc_addr_taken[ins->u.loc.slot] = 1;
    }

    for (i = 0; i <= w->ir.len; i++) {
        if (saved[i].valid) {
            size_t k;
            depth = saved[i].depth;
            for (k = 0; k < depth; k++)
                types[k] = saved[i].types[k];
            dead = 0;
        }
        if (i == w->ir.len)
            break;
        if (dead)
            continue;
        {
            const IrInstr *ins = &w->ir.data[i];
            switch (ins->kind) {
            case IR_FN_END:
                dead = 1;
                break;
            case IR_PUSH_W:
                check_vstack_push(depth);
                types[depth++] = TY_W;
                break;
            case IR_PUSH_A:
            case IR_PUSH_DATA:
            case IR_PUSH_HEAP_START:
            case IR_PUSH_FN:
                check_vstack_push(depth);
                types[depth++] = TY_A;
                break;
            case IR_PUSH_F:
                check_vstack_push(depth);
                types[depth++] = TY_F;
                break;
            case IR_PUSH_I32:
                check_vstack_push(depth);
                types[depth++] = TY_I32;
                break;
            case IR_LOCAL_OP:
                if (ins->u.loc.op == LOCAL_GET) {
                    check_vstack_push(depth);
                    types[depth++] = ins->u.loc.type;
                } else if (ins->u.loc.op == LOCAL_ADDR) {
                    check_vstack_push(depth);
                    types[depth++] = TY_A;
                } else {
                    depth--;
                }
                break;
            case IR_STACK_OP:
                if (ins->u.stk.op == OP_DUP) {
                    check_vstack_push(depth);
                    types[depth] = types[depth - 1];
                    depth++;
                } else if (ins->u.stk.op == OP_DROP) {
                    depth--;
                } else if (ins->u.stk.op == OP_SWAP) {
                    StackType tmp = types[depth - 1];
                    types[depth - 1] = types[depth - 2];
                    types[depth - 2] = tmp;
                }
                break;
            case IR_ALU_OP:
                if (ins->u.op == OP_BCOPY) {
                    depth -= 3;
                } else if (is_mem_store(ins->u.op)) {
                    depth -= 2;
                } else {
                    int ar = op_arity(ins->u.op);
                    depth -= (size_t)ar;
                    check_vstack_push(depth);
                    types[depth++] = alu_result_type(ins->u.op);
                }
                break;
            case IR_CALL:
            case IR_CALL_EXTERN: {
                const FuncSig *csig = funcs->data[ins->u.func_idx];
                size_t k;
                depth -= csig->in_types.len;
                for (k = 0; k < csig->out_types.len; k++) {
                    check_vstack_push(depth);
                    types[depth++] = csig->out_types.data[k];
                }
                break;
            }
            case IR_ICALL: {
                size_t k;
                depth -= (size_t)ins->u.icall.in_cnt + 1;
                for (k = 0; k < ins->u.icall.out_cnt; k++) {
                    check_vstack_push(depth);
                    types[depth] = (StackType)ins->u.icall.out_types[k];
                    depth++;
                    if (depth > max_depth)
                        max_depth = depth;
                }
                break;
            }
            case IR_BR_FWD:
                if (ins->u.br.cond == BR_JZ)
                    depth--;
                if ((size_t)ins->u.br.target <= w->ir.len) {
                    size_t k;
                    saved[ins->u.br.target].valid = 1;
                    saved[ins->u.br.target].depth = depth;
                    for (k = 0; k < depth; k++)
                        saved[ins->u.br.target].types[k] = types[k];
                }
                if (ins->u.br.cond == BR_JMP)
                    dead = 1;
                break;
            case IR_BR_BACK:
                if (ins->u.br.cond == BR_JZ)
                    depth--;
                else
                    dead = 1;
                break;
            case IR_JTAB:
                depth--;
                dead = 1;
                break;
            default:
                break;
            }
            if (depth > max_depth)
                max_depth = depth;
        }
    }
    (void)rbe;
    free(saved);
    *out_max_depth = max_depth;
}

static int is_br_target(const WordIR *w, size_t ir_idx, const SizeVec *jtab_targets) {
    size_t j;
    for (j = 0; j < w->ir.len; j++) {
        const IrInstr *bi = &w->ir.data[j];
        if ((bi->kind == IR_BR_FWD || bi->kind == IR_BR_BACK) && (size_t)bi->u.br.target == ir_idx)
            return 1;
        if (bi->kind == IR_JTAB) {
            size_t k;
            for (k = 0; k < (size_t)bi->u.jtab.count; k++) {
                if (jtab_targets->data[bi->u.jtab.start + k] == ir_idx)
                    return 1;
            }
        }
    }
    return 0;
}

void reg_lower_word(const RegBackend *rbe, FuncSig *sig, WordIR *w,
                    const FuncVec *funcs, const WordVec *words,
                    const SizeVec *jtab_targets, size_t heap_start_off,
                    RegFwdCallVec *fwd_calls) {
    size_t i, p;
    size_t max_depth = 0;
    u8 used_loc_regs = 0;
    u8 *loc_addr_taken;
    u8 *loc_is_reg;
    u8 *loc_reg;
    i32 *loc_off;
    i32 local_bytes = 0;
    u32 frame_bytes;
    RegState st;
    SavedStack *saved;
    size_t *ir_addr;
    RegPendingBrVec pending;
    RegPendingJtabVec pending_jtab;
    int dead = 0;

    loc_addr_taken = (u8 *)calloc(w->locals.len ? w->locals.len : 1, 1);
    loc_is_reg = (u8 *)calloc(w->locals.len ? w->locals.len : 1, 1);
    loc_reg = (u8 *)calloc(w->locals.len ? w->locals.len : 1, 1);
    loc_off = (i32 *)calloc(w->locals.len ? w->locals.len : 1, sizeof(i32));
    saved = (SavedStack *)calloc(w->ir.len + 1, sizeof(SavedStack));
    ir_addr = (size_t *)calloc(w->ir.len + 1, sizeof(size_t));
    pending.data = NULL;
    pending.len = 0;
    pending.cap = 0;
    pending_jtab.data = NULL;
    pending_jtab.len = 0;
    pending_jtab.cap = 0;

    if (!loc_addr_taken || !loc_is_reg || !loc_reg || !loc_off || !saved || !ir_addr)
        ir4_die("out of memory in reg_lower_word");

    prescan_word(rbe, sig, w, funcs, loc_addr_taken, &max_depth);

    for (i = 0; i < w->locals.len; i++) {
        if (w->locals.data[i].size == 0 && !loc_addr_taken[i] &&
            w->locals.data[i].type != TY_F && used_loc_regs < rbe->num_loc_regs) {
            loc_is_reg[i] = 1;
            loc_reg[i] = rbe->loc_regs[used_loc_regs++];
        } else {
            i32 sz = (w->locals.data[i].size > 0)
                         ? (i32)((w->locals.data[i].size + (size_t)(rbe->word_bytes - 1)) &
                                 ~(size_t)(rbe->word_bytes - 1))
                         : (i32)rbe->word_bytes;
            local_bytes += sz;
            loc_off[i] = local_bytes;
        }
    }

    frame_bytes = (u32)(local_bytes + (i32)(max_depth * rbe->word_bytes));
    if (rbe->word_bytes == 8)
        frame_bytes = (frame_bytes + 15U) & ~15U;

    sig->addr = rbe->cur_addr();
    sig->has_addr = 1;

    rbe->fn_begin(sig->name, used_loc_regs, frame_bytes, w->param_cnt, w->locals.data,
                  loc_is_reg, loc_reg, loc_off, w->locals.len);

    memset(&st, 0, sizeof(st));
    st.rbe = rbe;
    st.loc_is_reg = loc_is_reg;
    st.loc_reg = loc_reg;
    st.loc_off = loc_off;
    st.local_bytes = local_bytes;

    for (i = 0; i <= w->ir.len; i++) {
        int br_tgt = (i < w->ir.len && is_br_target(w, i, jtab_targets));
        if (br_tgt && !dead)
            flush_all(&st);

        for (p = 0; p < pending.len; p++) {
            if (pending.data[p].target_ir == i)
                rbe->patch_fwd(pending.data[p].patch_offset);
        }
        for (p = 0; p < pending_jtab.len; p++) {
            if (pending_jtab.data[p].target_ir == i) {
                rbe->patch_jtab(pending_jtab.data[p].patch_offset, pending_jtab.data[p].idx,
                                pending_jtab.data[p].count, rbe->cur_addr());
            }
        }

        if (saved[i].valid) {
            size_t k;
            st.depth = saved[i].depth;
            for (k = 0; k < st.depth; k++) {
                st.stk[k].type = saved[i].types[k];
                st.stk[k].kind = VK_REG;
            }
            dead = 0;
        } else if (br_tgt && dead) {
            /* Unmarked label target (e.g. '::' after jmp) starts with empty stack */
            st.depth = 0;
            dead = 0;
        }

        if (i == w->ir.len)
            break;

        ir_addr[i] = rbe->cur_addr();
        if (dead)
            continue;

        {
            const IrInstr *ins = &w->ir.data[i];
            switch (ins->kind) {
            case IR_FN_END:
                flush_all(&st);
                rbe->fn_end(used_loc_regs, frame_bytes);
                dead = 1;
                break;

            case IR_PUSH_W:
                check_vstack_push(st.depth);
                st.stk[st.depth].type = TY_W;
                st.stk[st.depth].kind = VK_CONST;
                st.stk[st.depth].imm = ins->u.w;
                st.depth++;
                break;

            case IR_PUSH_A:
                check_vstack_push(st.depth);
                st.stk[st.depth].type = TY_A;
                st.stk[st.depth].kind = VK_UCONST;
                st.stk[st.depth].uimm = ins->u.a;
                st.depth++;
                break;

            case IR_PUSH_F:
                check_vstack_push(st.depth);
                st.stk[st.depth].type = TY_F;
                st.stk[st.depth].kind = VK_FCONST;
                st.stk[st.depth].fval = ins->u.f;
                st.depth++;
                break;

            case IR_PUSH_I32:
                check_vstack_push(st.depth);
                st.stk[st.depth].type = TY_I32;
                st.stk[st.depth].kind = VK_CONST;
                st.stk[st.depth].imm = ins->u.w;
                st.depth++;
                break;

            case IR_PUSH_DATA:
                check_vstack_push(st.depth);
                st.stk[st.depth].type = TY_A;
                st.stk[st.depth].kind = VK_DATA;
                st.stk[st.depth].idx = ins->u.offset;
                st.depth++;
                break;

            case IR_PUSH_HEAP_START:
                check_vstack_push(st.depth);
                st.stk[st.depth].type = TY_A;
                st.stk[st.depth].kind = VK_DATA;
                st.stk[st.depth].idx = heap_start_off;
                st.depth++;
                break;

            case IR_PUSH_FN: {
                size_t target = ins->u.func_idx;
                size_t fn_addr = rbe->cur_addr();
                u8 dst;
                size_t fn_pc;
                check_vstack_push(st.depth);
                dst = dst_reg_for(&st, st.depth, TY_A);
                fn_pc = rbe->load_fn_addr(dst, funcs->data[target]->addr);
                commit_dst(&st, st.depth, TY_A, dst);
                st.depth++;
                if (!funcs->data[target]->has_addr) {
                    RegFwdCall *fc = VEC_ADD(*fwd_calls);
                    fc->call_pc = fn_pc;
                    fc->call_addr = fn_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 1;
                }
                break;
            }

            case IR_LOCAL_OP: {
                size_t slot = ins->u.loc.slot;
                if (ins->u.loc.op == LOCAL_GET) {
                    check_vstack_push(st.depth);
                    st.stk[st.depth].type = ins->u.loc.type;
                    st.stk[st.depth].kind = VK_LOCAL;
                    st.stk[st.depth].idx = slot;
                    st.depth++;
                } else if (ins->u.loc.op == LOCAL_ADDR) {
                    check_vstack_push(st.depth);
                    st.stk[st.depth].type = TY_A;
                    st.stk[st.depth].kind = VK_LADDR;
                    st.stk[st.depth].idx = slot;
                    st.depth++;
                } else if (ins->u.loc.op == LOCAL_SET) {
                    flush_local_refs(&st, slot);
                    st.depth--;
                    if (loc_is_reg[slot]) {
                        u8 src = read_slot_to_reg(&st, st.depth, loc_reg[slot]);
                        if (src != loc_reg[slot])
                            rbe->mov_reg(loc_reg[slot], src, ins->u.loc.type);
                    } else {
                        u8 src = read_slot_to_reg(&st, st.depth, rbe->scratch0);
                        rbe->spill_store(src, ins->u.loc.type, loc_off[slot]);
                    }
                } else { /* LOCAL_ADD */
                    flush_local_refs(&st, slot);
                    st.depth--;
                    if (loc_is_reg[slot]) {
                        if (st.stk[st.depth].kind == VK_CONST &&
                            rbe->alu_imm(OP_ADD, loc_reg[slot], loc_reg[slot],
                                         st.stk[st.depth].imm)) {
                            /* fused in place */
                        } else {
                            u8 rhs = read_slot_to_reg(&st, st.depth, rbe->scratch0);
                            rbe->alu_reg(OP_ADD, loc_reg[slot], loc_reg[slot], rhs);
                        }
                    } else {
                        u8 rhs = read_slot_to_reg(&st, st.depth, rbe->scratch0);
                        rbe->spill_load(rbe->scratch1, ins->u.loc.type, loc_off[slot]);
                        rbe->alu_reg(OP_ADD, rbe->scratch1, rbe->scratch1, rhs);
                        rbe->spill_store(rbe->scratch1, ins->u.loc.type, loc_off[slot]);
                    }
                }
                break;
            }

            case IR_STACK_OP:
                if (ins->u.stk.op == OP_DROP) {
                    st.depth--;
                } else if (ins->u.stk.op == OP_DUP) {
                    const VSlot *top = &st.stk[st.depth - 1];
                    check_vstack_push(st.depth);
                    if (top->kind != VK_REG) {
                        st.stk[st.depth] = *top;
                    } else {
                        u8 src = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                        u8 dst = dst_reg_for(&st, st.depth, top->type);
                        if (dst != src)
                            rbe->mov_reg(dst, src, top->type);
                        commit_dst(&st, st.depth, top->type, dst);
                    }
                    st.depth++;
                } else if (ins->u.stk.op == OP_SWAP) {
                    VSlot *a = &st.stk[st.depth - 2];
                    VSlot *b = &st.stk[st.depth - 1];
                    if (a->kind != VK_REG && b->kind != VK_REG) {
                        VSlot tmp = *a;
                        *a = *b;
                        *b = tmp;
                    } else {
                        StackType ta = a->type;
                        StackType tb = b->type;
                        u8 ra, rb;
                        materialize_slot(&st, st.depth - 2);
                        materialize_slot(&st, st.depth - 1);
                        ra = read_slot_to_reg(&st, st.depth - 2, rbe->scratch0);
                        rb = read_slot_to_reg(&st, st.depth - 1, rbe->scratch1);
                        if (rbe->has_fp_regs && ta == TY_F && tb == TY_F) {
                            rbe->mov_reg(rbe->fp_scratch, ra, TY_F);
                            store_to_slot(&st, st.depth - 2, rb, TY_F);
                            store_to_slot(&st, st.depth - 1, rbe->fp_scratch, TY_F);
                        } else if (rbe->has_fp_regs && (ta == TY_F || tb == TY_F)) {
                            /* Different register banks on ARM64: copy directly across banks */
                            u8 tmp_f = rbe->fp_scratch;
                            u8 tmp_g = rbe->scratch2;
                            if (ta == TY_F) {
                                rbe->mov_reg(tmp_f, ra, TY_F);
                                store_to_slot(&st, st.depth - 2, rb, tb);
                                store_to_slot(&st, st.depth - 1, tmp_f, TY_F);
                            } else {
                                rbe->mov_reg(tmp_g, ra, ta);
                                store_to_slot(&st, st.depth - 2, rb, TY_F);
                                store_to_slot(&st, st.depth - 1, tmp_g, ta);
                            }
                        } else {
                            rbe->mov_reg(rbe->scratch2, ra, ta);
                            store_to_slot(&st, st.depth - 2, rb, tb);
                            store_to_slot(&st, st.depth - 1, rbe->scratch2, ta);
                        }
                        a->type = tb;
                        b->type = ta;
                        a->kind = VK_REG;
                        b->kind = VK_REG;
                    }
                }
                break;

            case IR_ALU_OP: {
                IrOp op = ins->u.op;

                /* Fused base + offset memory access: [base, off, AADD/ASUB, mem] */
                if ((op == OP_AADD || op == OP_ASUB) && i + 1 < w->ir.len &&
                    !is_br_target(w, i + 1, jtab_targets) &&
                    w->ir.data[i + 1].kind == IR_ALU_OP &&
                    st.stk[st.depth - 1].kind == VK_CONST &&
                    (is_mem_load(w->ir.data[i + 1].u.op) ||
                     (is_mem_store(w->ir.data[i + 1].u.op) &&
                      !is_swapped_store(w->ir.data[i + 1].u.op)))) {
                    IrOp mop = w->ir.data[i + 1].u.op;
                    i32 off = (op == OP_ASUB) ? -st.stk[st.depth - 1].imm : st.stk[st.depth - 1].imm;
                    st.depth--; /* pop const off */
                    if (is_mem_load(mop)) {
                        u8 base = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                        StackType rty = alu_result_type(mop);
                        u8 dst = dst_reg_for(&st, st.depth - 1, rty);
                        rbe->load_mem(mop, dst, base, off);
                        commit_dst(&st, st.depth - 1, rty, dst);
                    } else {
                        u8 base = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                        u8 val = read_slot_to_reg(&st, st.depth - 2, rbe->scratch1);
                        rbe->store_mem(mop, val, base, off);
                        st.depth -= 2;
                    }
                    i++;
                    break;
                }

                /* Fused compare + conditional branch */
                if (op >= OP_EQ && op <= OP_AGT && i + 1 < w->ir.len &&
                    !is_br_target(w, i + 1, jtab_targets)) {
                    const IrInstr *bins = &w->ir.data[i + 1];
                    if ((bins->kind == IR_BR_FWD || bins->kind == IR_BR_BACK) &&
                        bins->u.br.cond == BR_JZ) {
                        int has_imm = (st.stk[st.depth - 1].kind == VK_CONST);
                        i32 imm = has_imm ? st.stk[st.depth - 1].imm : 0;
                        u8 lhs = read_slot_to_reg(&st, st.depth - 2, rbe->scratch0);
                        u8 rhs = has_imm ? 0 : read_slot_to_reg(&st, st.depth - 1, rbe->scratch1);
                        size_t old_depth = st.depth;
                        st.depth -= 2;
                        flush_all(&st);
                        if (bins->kind == IR_BR_FWD) {
                            size_t patch_off = 0;
                            if (rbe->cmp_br_fwd(op, has_imm, imm, lhs, rhs, &patch_off)) {
                                RegPendingBr *pb = VEC_ADD(pending);
                                pb->patch_offset = patch_off;
                                pb->target_ir = bins->u.br.target;
                                save_branch_state(saved, bins->u.br.target, w->ir.len, &st);
                                i++;
                                break;
                            }
                        } else {
                            if (rbe->cmp_br_back(op, has_imm, imm, lhs, rhs,
                                                 ir_addr[bins->u.br.target])) {
                                i++;
                                break;
                            }
                        }
                        st.depth = old_depth;
                    }
                }

                if (op == OP_BCOPY) {
                    u8 len = read_slot_to_reg(&st, st.depth - 1, rbe->scratch2);
                    u8 src = read_slot_to_reg(&st, st.depth - 2, rbe->scratch1);
                    u8 dst = read_slot_to_reg(&st, st.depth - 3, rbe->scratch0);
                    rbe->emit_bcopy(dst, src, len);
                    st.depth -= 3;
                } else if (is_mem_load(op)) {
                    u8 base = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                    StackType rty = alu_result_type(op);
                    u8 dst = dst_reg_for(&st, st.depth - 1, rty);
                    rbe->load_mem(op, dst, base, 0);
                    commit_dst(&st, st.depth - 1, rty, dst);
                } else if (is_mem_store(op)) {
                    u8 base, val;
                    if (is_swapped_store(op)) {
                        val = read_slot_to_reg(&st, st.depth - 1, rbe->scratch1);
                        base = read_slot_to_reg(&st, st.depth - 2, rbe->scratch0);
                    } else {
                        base = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                        val = read_slot_to_reg(&st, st.depth - 2, rbe->scratch1);
                    }
                    rbe->store_mem(op, val, base, 0);
                    st.depth -= 2;
                } else if (is_fp_alu(op)) {
                    int ar = op_arity(op);
                    StackType rty = alu_result_type(op);
                    if (!rbe->has_fp_regs) {
                        size_t base_d = st.depth - (size_t)ar;
                        u8 lhs, rhs = 0, dst;
                        save_live_below(&st, base_d);
                        lhs = read_slot_to_reg(&st, base_d, rbe->scratch0);
                        if (ar == 2)
                            rhs = read_slot_to_reg(&st, base_d + 1, rbe->scratch1);
                        dst = dst_reg_for(&st, base_d, rty);
                        rbe->fp_op(op, dst, lhs, rhs);
                        commit_dst(&st, base_d, rty, dst);
                        restore_live_below(&st, base_d);
                        st.depth = base_d + 1;
                    } else {
                        u8 lhs = read_slot_to_reg(&st, st.depth - (size_t)ar,
                                                  (st.stk[st.depth - (size_t)ar].type == TY_F)
                                                      ? rbe->fp_scratch
                                                      : rbe->scratch0);
                        u8 rhs = (ar == 2) ? read_slot_to_reg(&st, st.depth - 1, rbe->fp_scratch1)
                                           : 0;
                        u8 dst = dst_reg_for(&st, st.depth - (size_t)ar, rty);
                        rbe->fp_op(op, dst, lhs, rhs);
                        commit_dst(&st, st.depth - (size_t)ar, rty, dst);
                        st.depth -= (size_t)(ar - 1);
                    }
                } else if (op_arity(op) == 1) {
                    u8 lhs = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                    StackType rty = alu_result_type(op);
                    u8 dst = dst_reg_for(&st, st.depth - 1, rty);
                    rbe->alu_reg(op, dst, lhs, 0);
                    commit_dst(&st, st.depth - 1, rty, dst);
                } else {
                    /* Binary int/addr/i32 ALU or comparison */
                    StackType rty = alu_result_type(op);
                    u8 lhs = read_slot_to_reg(&st, st.depth - 2, rbe->scratch0);
                    u8 dst = dst_reg_for(&st, st.depth - 2, rty);
                    if (st.stk[st.depth - 1].kind == VK_CONST &&
                        rbe->alu_imm(op, dst, lhs, st.stk[st.depth - 1].imm)) {
                        commit_dst(&st, st.depth - 2, rty, dst);
                        st.depth--;
                    } else {
                        u8 rhs = read_slot_to_reg(&st, st.depth - 1, rbe->scratch1);
                        rbe->alu_reg(op, dst, lhs, rhs);
                        commit_dst(&st, st.depth - 2, rty, dst);
                        st.depth--;
                    }
                }
                break;
            }

            case IR_CALL: {
                size_t target = ins->u.func_idx;
                const FuncSig *csig = funcs->data[target];
                size_t in_cnt = csig->in_types.len;
                size_t out_cnt = csig->out_types.len;
                size_t base_d = st.depth - in_cnt;
                size_t reg_cnt = in_cnt < rbe->num_stk_regs ? in_cnt : rbe->num_stk_regs;
                size_t stk_cnt = in_cnt > rbe->num_stk_regs ? (in_cnt - rbe->num_stk_regs) : 0;
                u32 out_arg_bytes = (u32)((stk_cnt * rbe->word_bytes + 15U) & ~15U);
                size_t k, call_addr, call_pc;

                if (words->data[target]->is_code)
                    ir4_die("calling raw 'code' word is unsupported in register backend");

                save_live_below(&st, base_d);
                for (k = 0; k < in_cnt; k++)
                    materialize_slot(&st, base_d + k);
                if (out_arg_bytes > 0) {
                    rbe->sp_adj(-(i32)out_arg_bytes);
                    for (k = rbe->num_stk_regs; k < in_cnt; k++) {
                        StackType aty = csig->in_types.data[k];
                        u8 tmp = (rbe->has_fp_regs && aty == TY_F) ? rbe->fp_scratch : rbe->scratch0;
                        load_from_slot(&st, base_d + k, tmp, aty);
                        rbe->store_out_arg(tmp, aty,
                                           (u32)((k - rbe->num_stk_regs) * rbe->word_bytes));
                    }
                }
                for (k = 0; k < reg_cnt; k++)
                    load_from_slot(&st, base_d + k, slot_reg(&st, k, csig->in_types.data[k]),
                                   csig->in_types.data[k]);

                call_addr = rbe->cur_addr();
                call_pc = cb_pc();
                rbe->call(csig->addr);
                if (!csig->has_addr) {
                    RegFwdCall *fc = VEC_ADD(*fwd_calls);
                    fc->call_pc = call_pc;
                    fc->call_addr = call_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 0;
                }
                if (out_arg_bytes > 0)
                    rbe->sp_adj((i32)out_arg_bytes);

                for (k = out_cnt; k > 0; k--) {
                    size_t idx = k - 1;
                    StackType ot = csig->out_types.data[idx];
                    u8 src;
                    check_vstack_push(base_d + idx);
                    src = slot_reg(&st, idx, ot);
                    store_to_slot(&st, base_d + idx, src, ot);
                    st.stk[base_d + idx].type = ot;
                    st.stk[base_d + idx].kind = VK_REG;
                }
                restore_live_below(&st, base_d);
                st.depth = base_d + out_cnt;
                break;
            }

            case IR_ICALL: {
                size_t in_cnt = ins->u.icall.in_cnt;
                size_t out_cnt = ins->u.icall.out_cnt;
                size_t base_d = st.depth - 1 - in_cnt;
                size_t reg_cnt = in_cnt < rbe->num_stk_regs ? in_cnt : rbe->num_stk_regs;
                size_t stk_cnt = in_cnt > rbe->num_stk_regs ? (in_cnt - rbe->num_stk_regs) : 0;
                u32 out_arg_bytes = (u32)((stk_cnt * rbe->word_bytes + 15U) & ~15U);
                size_t k;

                save_live_below(&st, base_d);
                for (k = 0; k < in_cnt; k++)
                    materialize_slot(&st, base_d + k);
                materialize_slot(&st, st.depth - 1);
                if (out_arg_bytes > 0) {
                    rbe->sp_adj(-(i32)out_arg_bytes);
                    for (k = rbe->num_stk_regs; k < in_cnt; k++) {
                        StackType aty = st.stk[base_d + k].type;
                        u8 tmp = (rbe->has_fp_regs && aty == TY_F) ? rbe->fp_scratch : rbe->scratch0;
                        load_from_slot(&st, base_d + k, tmp, aty);
                        rbe->store_out_arg(tmp, aty,
                                           (u32)((k - rbe->num_stk_regs) * rbe->word_bytes));
                    }
                }
                for (k = 0; k < reg_cnt; k++)
                    load_from_slot(&st, base_d + k, slot_reg(&st, k, st.stk[base_d + k].type),
                                   st.stk[base_d + k].type);
                load_from_slot(&st, st.depth - 1, rbe->scratch0, TY_A);
                rbe->icall(rbe->scratch0);
                if (out_arg_bytes > 0)
                    rbe->sp_adj((i32)out_arg_bytes);

                for (k = out_cnt; k > 0; k--) {
                    size_t idx = k - 1;
                    StackType ot = (StackType)ins->u.icall.out_types[idx];
                    u8 src;
                    check_vstack_push(base_d + idx);
                    src = slot_reg(&st, idx, ot);
                    store_to_slot(&st, base_d + idx, src, ot);
                    st.stk[base_d + idx].type = ot;
                    st.stk[base_d + idx].kind = VK_REG;
                }
                restore_live_below(&st, base_d);
                st.depth = base_d + out_cnt;
                break;
            }

            case IR_CALL_EXTERN: {
                const FuncSig *csig = funcs->data[ins->u.func_idx];
                size_t in_cnt = csig->in_types.len;
                size_t out_cnt = csig->out_types.len;
                size_t base_d = st.depth - in_cnt;
                u8 arg_regs[16];
                size_t int_idx = 0, flt_idx = 0;
                size_t k;

                if (out_cnt > 1)
                    ir4_die("extern supports at most 1 return value in register backend");
                for (k = 0; k < in_cnt; k++) {
                    if (rbe->has_fp_regs && csig->in_types.data[k] == TY_F) {
                        if (flt_idx >= 8)
                            ir4_die("extern supports at most 8 float args in register backend");
                        flt_idx++;
                    } else {
                        if (int_idx >= 8)
                            ir4_die("extern supports at most 8 integer args in register backend");
                        int_idx++;
                    }
                }

                save_live_below(&st, base_d);
                for (k = 0; k < in_cnt; k++)
                    materialize_slot(&st, base_d + k);
                int_idx = 0;
                flt_idx = 0;
                for (k = 0; k < in_cnt; k++) {
                    StackType aty = csig->in_types.data[k];
                    u8 dst = (rbe->has_fp_regs && aty == TY_F) ? rbe->fp_stk_regs[flt_idx++]
                                                               : rbe->stk_regs[int_idx++];
                    load_from_slot(&st, base_d + k, dst, aty);
                    arg_regs[k] = dst;
                }
                rbe->call_extern(csig, arg_regs);
                if (out_cnt == 1) {
                    StackType ot = csig->out_types.data[0];
                    u8 ret_reg;
                    check_vstack_push(base_d);
                    ret_reg = slot_reg(&st, 0, ot);
                    store_to_slot(&st, base_d, ret_reg, ot);
                    st.stk[base_d].type = ot;
                    st.stk[base_d].kind = VK_REG;
                }
                restore_live_below(&st, base_d);
                st.depth = base_d + out_cnt;
                break;
            }

            case IR_BR_FWD: {
                u8 cond_reg = 0;
                RegPendingBr *pb;
                if (ins->u.br.cond == BR_JZ) {
                    cond_reg = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                    st.depth--;
                }
                flush_all(&st);
                pb = VEC_ADD(pending);
                pb->patch_offset = rbe->br_fwd(ins->u.br.cond, cond_reg);
                pb->target_ir = ins->u.br.target;
                save_branch_state(saved, ins->u.br.target, w->ir.len, &st);
                if (ins->u.br.cond == BR_JMP)
                    dead = 1;
                break;
            }

            case IR_BR_BACK: {
                u8 cond_reg = 0;
                if (ins->u.br.cond == BR_JZ) {
                    cond_reg = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                    st.depth--;
                }
                flush_all(&st);
                rbe->br_back(ins->u.br.cond, cond_reg, ir_addr[ins->u.br.target]);
                if (ins->u.br.cond == BR_JMP)
                    dead = 1;
                break;
            }

            case IR_JTAB: {
                u8 idx_reg = read_slot_to_reg(&st, st.depth - 1, rbe->scratch0);
                size_t tbl_off, k;
                st.depth--;
                flush_all(&st);
                tbl_off = rbe->emit_jtab(idx_reg, ins->u.jtab.count);
                for (k = 0; k < (size_t)ins->u.jtab.count; k++) {
                    size_t tir = jtab_targets->data[ins->u.jtab.start + k];
                    if (tir <= i) {
                        rbe->patch_jtab(tbl_off, k, ins->u.jtab.count, ir_addr[tir]);
                    } else {
                        RegPendingJtab *pj = VEC_ADD(pending_jtab);
                        pj->patch_offset = tbl_off;
                        pj->idx = k;
                        pj->count = ins->u.jtab.count;
                        pj->target_ir = tir;
                    }
                }
                dead = 1;
                break;
            }

            default:
                break;
            }
        }
    }

    free(loc_addr_taken);
    free(loc_is_reg);
    free(loc_reg);
    free(loc_off);
    free(saved);
    free(ir_addr);
    free(pending.data);
    free(pending_jtab.data);
}
