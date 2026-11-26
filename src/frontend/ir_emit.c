/* ir_emit.c - Reachability DCE and Pass 2 IR-to-backend lowering */
#include "internal.h"
#include "codebuf.h"
#include "strpool.h"

#define HOT_CODE_INLINE_MAX 16

typedef struct {
    size_t patch_offset;
    size_t target_ir;
} PendingBr;

typedef struct {
    size_t call_pc;
    size_t call_addr;
    size_t func_idx;
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

static FwdCallVec fwd_calls;
static size_t heap_start_off = 0;

void ir_emit_reset(void) {
    fwd_calls.len = 0;
    heap_start_off = 0;
}

static Bool can_reach_func(size_t cur, size_t target, Bool *visited, const Bool *addr_taken) {
    WordIR *w = words.data[cur];
    size_t i;
    if (funcs.data[cur]->is_extern || w->is_code)
        return false;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            size_t callee = ins->u.func_idx;
            if (callee == target)
                return true;
            if (!visited[callee]) {
                visited[callee] = true;
                if (can_reach_func(callee, target, visited, addr_taken))
                    return true;
            }
        } else if (ins->kind == IR_ICALL) {
            size_t k;
            for (k = 0; k < funcs.len; k++) {
                if (!addr_taken[k])
                    continue;
                if (k == target)
                    return true;
                if (!visited[k]) {
                    visited[k] = true;
                    if (can_reach_func(k, target, visited, addr_taken))
                        return true;
                }
            }
        }
    }
    return false;
}

static int can_inline_hot_code(size_t fidx, size_t *out_len) {
    FuncSig *sig = funcs.data[fidx];
    WordIR *cw = words.data[fidx];
    if (!ir4_get_opt_level() || !cw->is_code || !sig->is_hot || cw->ir_len == 0 ||
        cw->ir_len > HOT_CODE_INLINE_MAX)
        return 0;
    if (strcmp(be->name, "6502") != 0 && strcmp(be->name, "65816") != 0 &&
        strcmp(be->name, "z80") != 0 && strcmp(be->name, "ez80") != 0)
        return 0;
    *out_len = cw->ir_len;
    return 1;
}

static void mark_reachable(size_t fidx) {
    WordIR *w = words.data[fidx];
    size_t i;
    if (funcs.data[fidx]->is_extern)
        return;
    if (!funcs.data[fidx]->is_defined)
        ir4_die("called 'fwd' function was never defined");
    if (w->reachable)
        return;
    w->reachable = 1;
    if (w->is_code)
        return;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            size_t inl_len = 0;
            if (!can_inline_hot_code(ins->u.func_idx, &inl_len))
                mark_reachable(ins->u.func_idx);
        } else if (ins->kind == IR_PUSH_FN) {
            mark_reachable(ins->u.func_idx);
        }
    }
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

static int ir_is_br_target(const WordIR *w, size_t ir_idx) {
    size_t j;
    for (j = 0; j < w->ir_len; j++) {
        const IrInstr *bi = &ir_buf.data[w->ir_start + j];
        if ((bi->kind == IR_BR_FWD || bi->kind == IR_BR_BACK) && bi->u.br.target == ir_idx)
            return 1;
        if (bi->kind == IR_JTAB) {
            size_t k;
            for (k = 0; k < bi->u.jtab.count; k++) {
                if (jtab_targets.data[bi->u.jtab.start + k] == ir_idx)
                    return 1;
            }
        }
    }
    return 0;
}

static int ir_can_fuse(const WordIR *w, size_t i, size_t extra) {
    size_t k;
    if (!ir4_get_opt_level())
        return 0;
    if (i + extra >= w->ir_len)
        return 0;
    for (k = 1; k <= extra; k++) {
        if (ir_is_br_target(w, i + k))
            return 0;
    }
    return 1;
}

static void emit_word(size_t fidx) {
    static SizeVec ir_addr;
    FuncSig *sig = funcs.data[fidx];
    WordIR *w = words.data[fidx];
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
        sig->addr = be->cur_addr();
        sig->has_addr = 1;
        emit_raw_code_block(w->ir_start, w->ir_len, w->reloc_start, w->reloc_cnt);
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

    VEC_ENSURE(ir_addr, w->ir_len + 1);
    sig->addr = be->cur_addr();
    sig->has_addr = 1;
    be->fn_begin(sig->name, w->param_cnt, w->locals.data, w->locals.len, sig->is_recursive);

    for (i = 0; i <= w->ir_len; i++) {
        if (be->peep_fence && ir_is_br_target(w, i))
            be->peep_fence();
        for (p = 0; p < pending.len; p++) {
            if (pending.data[p].target_ir == i) {
                be->patch_fwd(pending.data[p].patch_offset);
            }
        }
        for (p = 0; p < pending_jtab.len; p++) {
            if (pending_jtab.data[p].target_ir == i) {
                be->patch_jtab(pending_jtab.data[p].patch_offset, pending_jtab.data[p].idx,
                               pending_jtab.data[p].count, be->cur_addr());
            }
        }
        if (i == w->ir_len)
            break;
        ir_addr.data[i] = be->cur_addr();
        {
            const IrInstr *ins = &ir_buf.data[w->ir_start + i];
            switch (ins->kind) {
            case IR_FN_END:
                be->fn_end(sig, w->locals.data, w->locals.len);
                break;
            case IR_PUSH_W:
                if (ir_can_fuse(w, i, 1) &&
                    ir_buf.data[w->ir_start + i + 1].kind == IR_LOCAL_OP) {
                    const IrInstr *nins = &ir_buf.data[w->ir_start + i + 1];
                    if (nins->u.loc.op == LOCAL_ADD && be->local_add_imm &&
                        be->local_add_imm(nins->u.loc.slot, nins->u.loc.type,
                                          nins->u.loc.stack_bytes, ins->u.w)) {
                        i++;
                        break;
                    }
                }
                if (ir_can_fuse(w, i, 2) &&
                    ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    ir_buf.data[w->ir_start + i + 2].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf.data[w->ir_start + i + 1].u.op;
                    IrOp mop = ir_buf.data[w->ir_start + i + 2].u.op;
                    if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH &&
                        mop <= OP_FSTORE && be->mem_imm) {
                        i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)ins->u.w) : ins->u.w;
                        if (be->mem_imm(mop, off)) {
                            i += 2;
                            break;
                        }
                    }
                }
                if (ir_can_fuse(w, i, 1) &&
                    ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf.data[w->ir_start + i + 1].u.op;
                    i32 imm = ins->u.w;
                    if (aop == OP_ADD || aop == OP_SUB || aop == OP_AADD || aop == OP_ASUB) {
                        i32 delta = (aop == OP_SUB || aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                        if (be->add_imm && be->add_imm(delta)) {
                            i++;
                            break;
                        }
                    } else if (aop >= OP_EQ && aop <= OP_AGT) {
                        if (ir_can_fuse(w, i, 2)) {
                            const IrInstr *bins = &ir_buf.data[w->ir_start + i + 2];
                            if (bins->kind == IR_BR_FWD && bins->u.br.cond == BR_JZ &&
                                be->cmp_br_fwd) {
                                size_t patch_off = 0;
                                if (be->cmp_br_fwd(aop, 1, imm, &patch_off)) {
                                    PendingBr *pb = VEC_ADD(pending);
                                    pb->patch_offset = patch_off;
                                    pb->target_ir = bins->u.br.target;
                                    i += 2;
                                    break;
                                }
                            } else if (bins->kind == IR_BR_BACK && bins->u.br.cond == BR_JZ &&
                                       be->cmp_br_back) {
                                if (be->cmp_br_back(aop, 1, imm,
                                                    ir_addr.data[bins->u.br.target])) {
                                    i += 2;
                                    break;
                                }
                            }
                        }
                        if (aop <= OP_UGE && be->cmp_imm && be->cmp_imm(aop, imm)) {
                            i++;
                            break;
                        }
                    } else if (aop == OP_LSHIFT || aop == OP_RSHIFT || aop == OP_ARSHIFT ||
                               aop == OP_SHL32 || aop == OP_RSHR32 || aop == OP_ASHR32) {
                        u8 max_sh = (aop >= OP_SHL32 || be->word_bytes == 4) ? 31
                                    : (be->word_bytes == 8)                  ? 63
                                                                             : (u8)(be->word_bytes * 8 - 1);
                        if (imm >= 0 && imm <= (i32)max_sh && be->shift_imm &&
                            be->shift_imm(aop, (u8)imm)) {
                            i++;
                            break;
                        }
                    } else if (aop == OP_AND || aop == OP_OR || aop == OP_XOR) {
                        if (be->bit_imm && be->bit_imm(aop, imm)) {
                            i++;
                            break;
                        }
                    }
                }
                be->push_w(ins->u.w);
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
                    ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    be->data_mem &&
                    be->data_mem(ir_buf.data[w->ir_start + i + 1].u.op, ins->u.offset)) {
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
                if (ins->u.loc.op == LOCAL_ADDR && be->local_mem) {
                    if (ir_can_fuse(w, i, 3) &&
                        ir_buf.data[w->ir_start + i + 1].kind == IR_PUSH_W &&
                        ir_buf.data[w->ir_start + i + 2].kind == IR_ALU_OP &&
                        ir_buf.data[w->ir_start + i + 3].kind == IR_ALU_OP) {
                        IrOp aop = ir_buf.data[w->ir_start + i + 2].u.op;
                        IrOp mop = ir_buf.data[w->ir_start + i + 3].u.op;
                        if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH &&
                            mop <= OP_FSTORE) {
                            i32 imm = ir_buf.data[w->ir_start + i + 1].u.w;
                            i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                            if (be->local_mem(ins->u.loc.slot, off, mop, ins->u.loc.stack_bytes)) {
                                i += 3;
                                break;
                            }
                        }
                    } else if (ir_can_fuse(w, i, 1) &&
                               ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP) {
                        IrOp mop = ir_buf.data[w->ir_start + i + 1].u.op;
                        if (mop >= OP_I8FETCH && mop <= OP_FSTORE &&
                            be->local_mem(ins->u.loc.slot, 0, mop, ins->u.loc.stack_bytes)) {
                            i += 1;
                            break;
                        }
                    }
                }
                be->local_op(ins->u.loc.op, ins->u.loc.slot, ins->u.loc.type,
                             ins->u.loc.stack_bytes);
                break;
            case IR_STACK_OP:
                be->stack_op(ins->u.stk.op, ins->u.stk.t1, ins->u.stk.t2);
                break;
            case IR_ALU_OP:
                if (ins->u.op >= OP_EQ && ins->u.op <= OP_AGT && ir_can_fuse(w, i, 1)) {
                    const IrInstr *bins = &ir_buf.data[w->ir_start + i + 1];
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
            case IR_CALL: {
                size_t target = ins->u.func_idx;
                size_t inl_len = 0;
                if (funcs.data[target]->is_extern) {
                    be->call_extern(funcs.data[target]);
                    break;
                }
                if (can_inline_hot_code(target, &inl_len)) {
                    emit_raw_code_block(words.data[target]->ir_start, inl_len,
                                        words.data[target]->reloc_start,
                                        words.data[target]->reloc_cnt);
                    break;
                }
                if (funcs.data[target]->has_addr && be->tail_call && ir_can_fuse(w, i, 1) &&
                    ir_buf.data[w->ir_start + i + 1].kind == IR_FN_END) {
#if IR4_ENABLE_6502
                    if (be == &be_6502) {
                        be_6502_set_call_bank(funcs.data[target]->bank);
                    }
#endif
                    if (be->tail_call(funcs.data[target]->addr, sig, funcs.data[target],
                                      w->locals.data, w->locals.len)) {
                        i++;
                        break;
                    }
                }
                {
                    size_t call_addr = be->cur_addr();
                    size_t call_pc = cb_pc();
#if IR4_ENABLE_6502
                    if (be == &be_6502) {
                        be_6502_set_call_bank(funcs.data[target]->bank);
                    }
#endif
                    be->call(funcs.data[target]->addr);
                    if (!funcs.data[target]->has_addr) {
                        FwdCall *fc = VEC_ADD(fwd_calls);
                        fc->call_pc = call_pc;
                        fc->call_addr = call_addr;
                        fc->func_idx = target;
                        fc->is_fn_ptr = 0;
                    }
                }
                break;
            }
            case IR_PUSH_FN: {
                size_t target = ins->u.func_idx;
                size_t fn_addr = be->cur_addr();
                size_t fn_pc = be->push_fn(funcs.data[target]->addr);
                if (!funcs.data[target]->has_addr) {
                    FwdCall *fc = VEC_ADD(fwd_calls);
                    fc->call_pc = fn_pc;
                    fc->call_addr = fn_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 1;
                }
                break;
            }
            case IR_ICALL:
                be->icall();
                break;
            case IR_CALL_EXTERN:
                be->call_extern(funcs.data[ins->u.func_idx]);
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
                for (k = 0; k < ins->u.jtab.count; k++) {
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
            case IR_INLINE: {
                const InlineBlock *ib = &inline_blocks.data[ins->u.inl_idx];
                if (strcmp(be->name, "bc") == 0)
                    ir4_die("'inline' is not supported on bytecode target");
                if (be->peep_fence)
                    be->peep_fence();
                emit_raw_code_block(ib->raw_start, ib->raw_len, ib->reloc_start, ib->reloc_cnt);
                break;
            }
            }
        }
    }
    free(pending.data);
    free(pending_jtab.data);
}

void run_reachability_dce(void) {
    int main_idx = ir4_find_func("main");
    size_t f;
    Bool *visited;

    if (main_idx >= 0 && init_funcs.len > 0) {
        size_t wrap_idx;
        FuncSig *wsig;
        WordIR *ww;
        IrInstr *ins;
        size_t k;
        funcs.data[main_idx]->name = intern_str("__main_body");
        wrap_idx = alloc_func(&wsig, &ww);
        wsig->name = intern_str("main");
        wsig->is_defined = 1;
        for (k = 0; k < funcs.data[main_idx]->out_types.len; k++) {
            VEC_PUSH(wsig->out_types, funcs.data[main_idx]->out_types.data[k]);
        }
        ww->ir_start = ir_buf.len;
        for (k = 0; k < init_funcs.len; k++) {
            ins = emit_ir(IR_CALL, ww);
            ins->u.func_idx = init_funcs.data[k];
        }
        ins = emit_ir(IR_CALL, ww);
        ins->u.func_idx = (size_t)main_idx;
        emit_ir(IR_FN_END, ww);
        main_idx = (int)wrap_idx;
    }
    if (main_idx >= 0) {
        size_t di;
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
        for (f = 0; f < funcs.len; f++) {
            if (!funcs.data[f]->is_extern && funcs.data[f]->is_defined)
                words.data[f]->reachable = true;
        }
    }

    {
        Bool *addr_taken = (Bool *)vec_calloc(funcs.len ? funcs.len : 1, sizeof(Bool));
        size_t di;
        for (di = 0; di < cb_data_init_reloc_count(); di++) {
            size_t po, tv;
            u8 ab;
            Bool is_c;
            cb_get_data_init_reloc(di, &po, &tv, &ab, &is_c);
            if (is_c && tv < funcs.len)
                addr_taken[tv] = true;
        }
        for (f = 0; f < funcs.len; f++) {
            WordIR *w = words.data[f];
            size_t i;
            if (!w->reachable || funcs.data[f]->is_extern || w->is_code)
                continue;
            for (i = 0; i < w->ir_len; i++) {
                const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                if (ins->kind == IR_PUSH_FN && ins->u.func_idx < funcs.len)
                    addr_taken[ins->u.func_idx] = true;
            }
        }
        visited = (Bool *)vec_calloc(funcs.len ? funcs.len : 1, sizeof(Bool));
        for (f = 0; f < funcs.len; f++) {
            if (words.data[f]->reachable) {
                memset(visited, 0, funcs.len * sizeof(Bool));
                funcs.data[f]->is_recursive = can_reach_func(f, f, visited, addr_taken);
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

    for (f = 0; f < funcs.len; f++) {
        FuncSig *sig = funcs.data[f];
        WordIR *w = words.data[f];
        if (sig->is_extern || !w->reachable) {
            sig->bank = 0;
            continue;
        }
        if (sig->is_hot || w->is_code || strcmp(sig->name, "main") == 0 ||
            strcmp(sig->name, "init") == 0) {
            sig->bank = 0;
        } else {
            u32 est_sz = (u32)(w->ir_len * 64);
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

static Bool *select_compact_funcs(void) {
    Bool *fn_compact = NULL;
    u32 *est = (u32 *)vec_calloc(funcs.len, sizeof(u32));
    u32 *heat = (u32 *)vec_calloc(funcs.len, sizeof(u32));
    u32 total_est = 0;
    size_t max_ir = 1;
    size_t f;

    for (f = 0; f < funcs.len; f++) {
        if (!funcs.data[f]->is_extern && words.data[f]->reachable && !words.data[f]->is_code) {
            est[f] = (u32)(words.data[f]->ir_len * 10);
            total_est += est[f];
            if (words.data[f]->ir_len > max_ir)
                max_ir = words.data[f]->ir_len;
        }
    }
    if (total_est > 10240) {
        u8 *depth = (u8 *)vec_calloc(max_ir, sizeof(u8));
        u32 native_budget = 6144;
        int pass;
        fn_compact = (Bool *)vec_calloc(funcs.len, sizeof(Bool));
        for (f = 0; f < funcs.len; f++) {
            WordIR *w = words.data[f];
            size_t i;
            if (funcs.data[f]->is_extern || !w->reachable || w->is_code)
                continue;
            fn_compact[f] = !(funcs.data[f]->is_hot || w->has_inline);
            if (funcs.data[f]->is_hot)
                heat[f] += 10000;
            if (funcs.data[f]->is_recursive)
                heat[f] += 100;
            memset(depth, 0, w->ir_len);
            for (i = 0; i < w->ir_len; i++) {
                const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                if (ins->kind == IR_BR_BACK) {
                    size_t k;
                    for (k = ins->u.br.target; k <= i; k++) {
                        if (depth[k] < 255)
                            depth[k]++;
                    }
                }
            }
            for (i = 0; i < w->ir_len; i++) {
                const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                u32 wgt = (depth[i] == 0) ? 0 : (depth[i] == 1 ? 10 : 100);
                heat[f] += wgt;
                if (ins->kind == IR_CALL && wgt > 0) {
                    heat[ins->u.func_idx] += wgt * 4;
                }
            }
        }
        /* Propagate loop heat one hop to callees of hot functions */
        for (pass = 0; pass < 2; pass++) {
            for (f = 0; f < funcs.len; f++) {
                WordIR *w = words.data[f];
                size_t i;
                if (funcs.data[f]->is_extern || !w->reachable || w->is_code || heat[f] == 0)
                    continue;
                for (i = 0; i < w->ir_len; i++) {
                    const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                    if (ins->kind == IR_CALL && heat[ins->u.func_idx] < heat[f]) {
                        heat[ins->u.func_idx] += heat[f] / 2;
                    }
                }
            }
        }
        /* Greedily keep hottest functions (by heat/byte) native up to budget */
        for (;;) {
            size_t best_f = funcs.len;
            u32 best_score = 0;
            for (f = 0; f < funcs.len; f++) {
                u32 score;
                if (!fn_compact[f] || heat[f] == 0 || est[f] > native_budget)
                    continue;
                score = (heat[f] * 100) / (est[f] ? est[f] : 1);
                if (score > best_score) {
                    best_score = score;
                    best_f = f;
                }
            }
            if (best_f == funcs.len)
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

    if (cb_is_banked() && funcs.len > 0) {
        max_bank = assign_code_banks();
    }
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
    else if (saved_compact == 2 && funcs.len > 0) {
        fn_compact = select_compact_funcs();
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
            for (f = 0; f < funcs.len; f++) {
                if (!funcs.data[f]->is_extern && words.data[f]->reachable &&
                    funcs.data[f]->bank == b) {
                    emit_word(f);
                }
            }
#if IR4_ENABLE_6502
            if (b == 0 && max_bank > 0 && be == &be_6502) {
                int need_mul = 0, need_div = 0, need_icall = 0, need_fp = 0;
                for (f = 0; f < funcs.len; f++) {
                    WordIR *w = words.data[f];
                    size_t i;
                    if (funcs.data[f]->is_extern || !w->reachable || funcs.data[f]->bank == 0)
                        continue;
                    for (i = 0; i < w->ir_len; i++) {
                        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
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
                }
                be_6502_ensure_helpers(need_mul, need_div, need_icall, need_fp);
            }
#endif
        }
        cb_end_banks();
    } else {
        for (f = 0; f < funcs.len; f++) {
            if (!funcs.data[f]->is_extern && words.data[f]->reachable) {
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
                if (funcs.data[f]->is_hot)
                    set_be_compact(0);
                else if (fn_compact)
                    set_be_compact(fn_compact[f] ? 1 : 0);
                else if (saved_compact == 2)
                    set_be_compact(0);
#endif
                emit_word(f);
            }
        }
    }
#if IR4_ENABLE_6502 || IR4_ENABLE_EZ80
    set_be_compact(saved_compact);
#endif
    free(fn_compact);

    for (c = 0; c < fwd_calls.len; c++) {
        if (fwd_calls.data[c].is_fn_ptr) {
            be->patch_fn(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                         funcs.data[fwd_calls.data[c].func_idx]->addr);
        } else {
            be->patch_call(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                           funcs.data[fwd_calls.data[c].func_idx]->addr);
        }
    }
    for (c = 0; c < cb_data_init_reloc_count(); c++) {
        size_t po, tv;
        u8 ab;
        Bool is_c;
        cb_get_data_init_reloc(c, &po, &tv, &ab, &is_c);
        if (is_c) {
            cb_patch_data_init_reloc(c, funcs.data[tv]->addr);
        }
    }
}
