/* ez80.c - eZ80 ADL (24-bit) Direct Binary Backend for IR4th
 * Calling & Register Convention (ADL = 1):
 *   - Data stack: hardware SP (3 bytes per :i/:a slot, 6 bytes per :f/i32 slot)
 *   - Return stack: dedicated 24-bit BSS stack pointed to by BSS rsp_off
 *   - Locals: static BSS for non-recursive functions; SP-relative for recursive
 */
#include "backend/ez80.h"
#include "backend/ez80_rt.h"
#include "backend/bc.h"
#include "codebuf.h"
#include "vec.h"

static size_t ez_rsp_off = 0;
static size_t ez_rstack_off = 0;
static size_t ez_tmp_off = 0;
static size_t ez_vm_ip_off = 0;
static size_t ez_vm_leave_off = 0;
static int ez_rt_inited = 0;
static int ez_compact = 2; /* 0 = native, 1 = all compact, 2 = auto */
static size_t ez_vm_enter_addr = 0;

void be_ez80_set_compact(int compact) { ez_compact = compact; }
int be_ez80_get_compact(void) { return ez_compact; }
static void ez_ensure_bc_vm(void);
static void ez_op(IrOp op);
static void ez_stack_op(StackOp op, StackType t1, StackType t2);

static size_t ez_mul24_addr = 0;
static size_t ez_udivmod24_addr = 0;
static size_t ez_sdivmod24_addr = 0;
static size_t ez_shl24_addr = 0;
static size_t ez_shr24_addr = 0;
static size_t ez_ashr24_addr = 0;
static size_t ez_rt_addr = 0;
static size_t ez_fcmp_addr = 0;

static void ez_ensure_fcmp(void) {
    size_t skip;
    size_t jnz1, jnz2, jnz3, jp_diff, jnz_c, jz_eq, jz_pos1, jnc_pos, jz_pos2;
    size_t not_both_zero, got_c, ret_eq, ret_c, ret_pos, diff_sign, ret_pos1;
    if (ez_fcmp_addr != 0)
        return;
    cb_emit8(EZ80_JP);
    skip = cb_pc();
    cb_emit24(0);
    ez_fcmp_addr = cb_cur_addr();
    /* Inputs: E:HL = a, A:BC = b. Output: Z=1,S=0 (eq); Z=0,S=1 (lt); Z=0,S=0 (gt) */
    cb_emit8(0x57); /* LD D, A */
    cb_emit8(0x7B); /* LD A, E */
    cb_emit8(0xB2); /* OR D */
    cb_emit8_8(0xE6, 0x7F); /* AND 0x7F */
    cb_emit8(0x20); jnz1 = cb_pc(); cb_emit8(0); /* JR NZ, not_both_zero */
    /* Check BC == 0 */
    cb_emit8(0xE5); /* PUSH HL */
    cb_emit8(0xC5); /* PUSH BC */
    cb_emit8(0xB7); /* OR A */
    cb_emit8(EZ80_LD_HL_N24); cb_emit24(0);
    cb_emit8_8(0xED, 0x4A); /* ADC HL, BC */
    cb_emit8(0xC1); /* POP BC */
    cb_emit8(0xE1); /* POP HL */
    cb_emit8(0x20); jnz2 = cb_pc(); cb_emit8(0); /* JR NZ, not_both_zero */
    /* Check HL == 0 */
    cb_emit8(0xE5); /* PUSH HL */
    cb_emit8(0xC5); /* PUSH BC */
    cb_emit8(0x01); cb_emit24(0); /* LD BC, 0 */
    cb_emit8(0xB7); /* OR A */
    cb_emit8_8(0xED, 0x4A); /* ADC HL, BC */
    cb_emit8(0xC1); /* POP BC */
    cb_emit8(0xE1); /* POP HL */
    cb_emit8(0x20); jnz3 = cb_pc(); cb_emit8(0); /* JR NZ, not_both_zero */
    cb_emit8(0xAF); /* XOR A -> Z=1, S=0 */
    cb_emit8(0xC9); /* RET */

    not_both_zero = cb_cur_addr();
    cb_patch8(jnz1, (u8)(not_both_zero - (jnz1 + 1)));
    cb_patch8(jnz2, (u8)(not_both_zero - (jnz2 + 1)));
    cb_patch8(jnz3, (u8)(not_both_zero - (jnz3 + 1)));
    cb_emit8(0x7B); /* LD A, E */
    cb_emit8(0xAA); /* XOR D */
    cb_emit8(EZ80_JP_M); jp_diff = cb_pc(); cb_emit24(0); /* JP M, diff_sign */
    cb_emit8(0x7B); /* LD A, E */
    cb_emit8(0xBA); /* CP D */
    cb_emit8(0x20); jnz_c = cb_pc(); cb_emit8(0); /* JR NZ, got_c */
    cb_emit8(0xB7); /* OR A */
    cb_emit8_8(0xED, 0x42); /* SBC HL, BC */
    cb_emit8(0x28); jz_eq = cb_pc(); cb_emit8(0); /* JR Z, ret_eq */

    got_c = cb_cur_addr();
    cb_patch8(jnz_c, (u8)(got_c - (jnz_c + 1)));
    cb_emit8_8(0xCB, 0x7B); /* BIT 7, E */
    cb_emit8(0x28); jz_pos1 = cb_pc(); cb_emit8(0); /* JR Z, ret_c */
    cb_emit8(0x3F); /* CCF */

    ret_c = cb_cur_addr();
    cb_patch8(jz_pos1, (u8)(ret_c - (jz_pos1 + 1)));
    cb_emit8_8(0x3E, 0x01); /* LD A, 1 */
    cb_emit8(0x30); jnc_pos = cb_pc(); cb_emit8(0); /* JR NC, ret_pos */
    cb_emit8_8(0x3E, 0xFF); /* LD A, 0xFF */

    ret_pos = cb_cur_addr();
    cb_patch8(jnc_pos, (u8)(ret_pos - (jnc_pos + 1)));
    cb_emit8(0xB7); /* OR A -> S=bit7(A), Z=0 */
    cb_emit8(0xC9); /* RET */

    ret_eq = cb_cur_addr();
    cb_patch8(jz_eq, (u8)(ret_eq - (jz_eq + 1)));
    cb_emit8(0xAF); /* XOR A -> Z=1, S=0 */
    cb_emit8(0xC9); /* RET */

    diff_sign = cb_cur_addr();
    cb_patch24(jp_diff, (u32)diff_sign);
    cb_emit8_8(0xCB, 0x7B); /* BIT 7, E */
    cb_emit8(0x28); jz_pos2 = cb_pc(); cb_emit8(0); /* JR Z, ret_pos1 */
    cb_emit8_8(0x3E, 0xFF); /* LD A, 0xFF */
    cb_emit8(0xB7); /* OR A -> S=1, Z=0 */
    cb_emit8(0xC9); /* RET */

    ret_pos1 = cb_cur_addr();
    cb_patch8(jz_pos2, (u8)(ret_pos1 - (jz_pos2 + 1)));
    cb_emit8_8(0x3E, 0x01); /* LD A, 1 */
    cb_emit8(0xB7); /* OR A -> S=0, Z=0 */
    cb_emit8(0xC9); /* RET */

    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ez_ensure_rt(void) {
    size_t skip;
    size_t base_pc, i;
    if (ez_rt_addr != 0)
        return;
    cb_emit8(EZ80_JP);
    skip = cb_pc();
    cb_emit24(0);
    ez_rt_addr = cb_cur_addr();
    base_pc = cb_pc();
    for (i = 0; i < sizeof(ez80_rt_code); i++)
        cb_emit8(ez80_rt_code[i]);
    for (i = 0; i < sizeof(ez80_rt_relocs) / sizeof(ez80_rt_relocs[0]); i++) {
        u16 rel = ez80_rt_relocs[i];
        u32 v = (u32)ez80_rt_code[rel] | ((u32)ez80_rt_code[rel + 1] << 8) |
                ((u32)ez80_rt_code[rel + 2] << 16);
        cb_patch24(base_pc + rel, v + (u32)ez_rt_addr);
    }
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ez_ensure_rt_data(void) {
    if (!ez_rt_inited) {
        ez_rstack_off = cb_alloc_data(256 * 3, 1);
        ez_rsp_off = cb_alloc_data(3, 1);
        ez_tmp_off = cb_alloc_data(6, 1);
        ez_vm_ip_off = cb_alloc_data(3, 1);
        ez_vm_leave_off = cb_alloc_data(4, 1);
        ez_rt_inited = 1;
    }
}

size_t be_ez80_rsp_off(void) {
    ez_ensure_rt_data();
    return ez_rsp_off;
}

size_t be_ez80_rstack_top_off(void) {
    ez_ensure_rt_data();
    return ez_rstack_off + 256 * 3;
}

static size_t ez_cur_addr(void) { return cb_cur_addr(); }

static void emit8_24(u8 op, u32 val) {
    cb_emit8(op);
    cb_emit24(val);
}

static void ez_ld_hl_data(size_t data_off) {
    cb_emit8(EZ80_LD_HL_N24);
    cb_add_data_reloc24(cb_pc(), data_off);
    cb_emit24(0);
}

static void ez_ld_hl_ind_data(size_t data_off) {
    cb_emit8(EZ80_LD_HL_NN);
    cb_add_data_reloc24(cb_pc(), data_off);
    cb_emit24(0);
}

static void ez_ld_data_hl(size_t data_off) {
    cb_emit8(EZ80_LD_NN_HL);
    cb_add_data_reloc24(cb_pc(), data_off);
    cb_emit24(0);
}

static size_t ez_last_push_pc = 0;
static u8 ez_last_push_op = 0;

static void ez_peep_fence(void) { ez_last_push_op = 0; }

static void ez_push_hl(void) {
    cb_emit8(EZ80_PUSH_HL);
    if (ir4_get_opt_level()) {
        ez_last_push_op = EZ80_PUSH_HL;
        ez_last_push_pc = cb_pc();
    }
}

static void ez_pop_hl(void) {
    if (ez_last_push_op == EZ80_PUSH_HL && cb_pc() == ez_last_push_pc) {
        cb_rewind(1);
        ez_last_push_op = 0;
        return;
    }
    ez_last_push_op = 0;
    cb_emit8(EZ80_POP_HL);
}

static void ez_pop_de(void) {
    if (ez_last_push_op == EZ80_PUSH_HL && cb_pc() == ez_last_push_pc) {
        cb_rewind(1);
        cb_emit8(EZ80_EX_DE_HL);
        ez_last_push_op = 0;
        return;
    }
    ez_last_push_op = 0;
    cb_emit8(EZ80_POP_DE);
}

VEC_OF(SizeVec, size_t);
VEC_OF(U8Vec, u8);
static SizeVec ez_local_off;
static U8Vec ez_is_static;
static size_t ez_frame_bytes = 0;
static size_t ez_static_base = 0;

static int ez_local_add_imm(size_t slot, StackType type, size_t stack_bytes, i32 imm) {
    (void)type;
    (void)stack_bytes;
    if (imm == 0)
        return 1;
    if (ez_is_static.data[slot]) {
        size_t addr_off = ez_static_base + ez_local_off.data[slot];
        if (ez_compact) {
            cb_emit8(BC_STATIC_GET);
            cb_add_data_reloc24(cb_pc(), addr_off);
            cb_emit24(0);
            if (imm == 1)
                cb_emit8(BC_INC);
            else if (imm == -1)
                cb_emit8(BC_DEC);
            else if (imm >= -128 && imm <= 127)
                cb_emit8_8(BC_ADD_IMM8, (u8)(i8)imm);
            else {
                cb_emit8(BC_PUSH_DATA);
                cb_emit24((u32)imm & 0xFFFFFFUL);
                cb_emit8(BC_ADD);
            }
            cb_emit8(BC_STATIC_SET);
            cb_add_data_reloc24(cb_pc(), addr_off);
            cb_emit24(0);
            return 1;
        }
        ez_ld_hl_ind_data(addr_off);
        if (imm == 1) {
            cb_emit8(0x23); /* INC HL */
        } else if (imm == -1) {
            cb_emit8(0x2B); /* DEC HL */
        } else {
            emit8_24(EZ80_LD_DE_N24, (u32)imm);
            cb_emit8(EZ80_ADD_HL_DE);
        }
        ez_ld_data_hl(addr_off);
        return 1;
    }
    return 0;
}

static void ez_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
    size_t off;
    int is4 = (type == TY_F || type == TY_I32);

    if (ez_compact) {
        if (ez_is_static.data[slot]) {
            size_t addr_off = ez_static_base + ez_local_off.data[slot];
            if (op == LOCAL_ADDR) {
                cb_emit8(BC_PUSH_DATA);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
            } else if (op == LOCAL_GET) {
                cb_emit8(is4 ? BC_STATIC_GET32 : BC_STATIC_GET);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
            } else if (op == LOCAL_ADD) {
                cb_emit8(BC_STATIC_GET);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
                cb_emit8(BC_ADD);
                cb_emit8(BC_STATIC_SET);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
            } else {
                cb_emit8(is4 ? BC_STATIC_SET32 : BC_STATIC_SET);
                cb_add_data_reloc24(cb_pc(), addr_off);
                cb_emit24(0);
            }
            return;
        }
        off = ez_local_off.data[slot] + stack_bytes;
        if (op == LOCAL_ADDR) {
            cb_emit8(BC_LOCAL_ADDR);
            cb_emit24((u32)off);
        } else if (op == LOCAL_GET) {
            cb_emit8(is4 ? BC_LOCAL_GET32 : BC_LOCAL_GET);
            cb_emit24((u32)off);
        } else if (op == LOCAL_ADD) {
            cb_emit8(BC_LOCAL_GET);
            cb_emit24((u32)(off + 3));
            cb_emit8(BC_ADD);
            cb_emit8(BC_LOCAL_SET);
            cb_emit24((u32)off);
        } else {
            cb_emit8(is4 ? BC_LOCAL_SET32 : BC_LOCAL_SET);
            cb_emit24((u32)off);
        }
        return;
    }

    if (ez_is_static.data[slot]) {
        size_t addr_off = ez_static_base + ez_local_off.data[slot];
        if (op == LOCAL_ADDR) {
            ez_ld_hl_data(addr_off);
            ez_push_hl();
        } else if (op == LOCAL_GET) {
            if (is4) {
                ez_ld_hl_ind_data(addr_off + 3);
                ez_push_hl(); /* upper byte + padding */
                ez_ld_hl_ind_data(addr_off);
                ez_push_hl();
            } else {
                ez_ld_hl_ind_data(addr_off);
                ez_push_hl();
            }
        } else if (op == LOCAL_ADD) {
            ez_pop_de();
            ez_ld_hl_ind_data(addr_off);
            cb_emit8(EZ80_ADD_HL_DE);
            ez_ld_data_hl(addr_off);
        } else {
            if (is4) {
                ez_pop_hl();
                ez_ld_data_hl(addr_off);
                ez_pop_hl();
                ez_ld_data_hl(addr_off + 3);
            } else {
                ez_pop_hl();
                ez_ld_data_hl(addr_off);
            }
        }
        return;
    }

    off = ez_local_off.data[slot] + stack_bytes;
    if (op == LOCAL_ADDR) {
        emit8_24(EZ80_LD_HL_N24, (u32)off);
        cb_emit8(EZ80_ADD_HL_SP);
        ez_push_hl();
    } else if (op == LOCAL_GET) {
        if (is4) {
            emit8_24(EZ80_LD_HL_N24, (u32)(off + 3));
            cb_emit8(EZ80_ADD_HL_SP);
            cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
            ez_push_hl();
            emit8_24(EZ80_LD_HL_N24, (u32)(off + 3));
            cb_emit8(EZ80_ADD_HL_SP);
            cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
            ez_push_hl();
        } else {
            emit8_24(EZ80_LD_HL_N24, (u32)off);
            cb_emit8(EZ80_ADD_HL_SP);
            cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
            ez_push_hl();
        }
    } else if (op == LOCAL_ADD) {
        ez_pop_de();
        emit8_24(EZ80_LD_HL_N24, (u32)off);
        cb_emit8(EZ80_ADD_HL_SP);
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
        cb_emit8(EZ80_ADD_HL_DE);
        cb_emit8(0xEB); /* EX DE, HL */
        emit8_24(EZ80_LD_HL_N24, (u32)off);
        cb_emit8(EZ80_ADD_HL_SP);
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    } else {
        if (is4) {
            ez_pop_de();
            emit8_24(EZ80_LD_HL_N24, (u32)(off + 3));
            cb_emit8(EZ80_ADD_HL_SP);
            cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
            ez_pop_de();
            emit8_24(EZ80_LD_HL_N24, (u32)(off + 3));
            cb_emit8(EZ80_ADD_HL_SP);
            cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
        } else {
            ez_pop_de();
            emit8_24(EZ80_LD_HL_N24, (u32)off);
            cb_emit8(EZ80_ADD_HL_SP);
            cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
        }
    }
}

static void ez_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                        size_t total_locals, Bool is_recursive) {
    size_t i;
    (void)name;
    ez_ensure_rt_data();

    if (ez_compact) {
        ez_ensure_bc_vm();
        emit8_24(EZ80_CALL, (u32)ez_vm_enter_addr);
    } else {
        /* Save 3-byte return address from SP onto BSS rstack */
        cb_emit8(EZ80_POP_DE);
        ez_ld_hl_ind_data(ez_rsp_off);
        cb_emit8(EZ80_DEC_HL);
        cb_emit8(EZ80_DEC_HL);
        cb_emit8(EZ80_DEC_HL);
        ez_ld_data_hl(ez_rsp_off);
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    }

    VEC_ENSURE(ez_local_off, total_locals);
    VEC_ENSURE(ez_is_static, total_locals);
    ez_frame_bytes = 0;

    if (!is_recursive) {
        size_t static_bytes = 0;
        size_t sp_bytes = 0;
        for (i = 0; i < total_locals; i++) {
            if (locals[i].size > 0) {
                ez_local_off.data[i] = sp_bytes;
                ez_is_static.data[i] = 0;
                sp_bytes += locals[i].size;
            } else {
                size_t sz = (locals[i].type == TY_F || locals[i].type == TY_I32) ? 6 : 3;
                ez_local_off.data[i] = static_bytes;
                ez_is_static.data[i] = 1;
                static_bytes += sz;
            }
        }
        ez_static_base = static_bytes ? cb_alloc_data(static_bytes, 1) : 0;
        for (i = param_cnt; i > 0; i--) {
            ez_local_op(LOCAL_SET, i - 1, locals[i - 1].type, 0);
        }
        ez_frame_bytes = sp_bytes;
        if (sp_bytes > 0) {
            if (ez_compact) {
                cb_emit8(BC_ENTER);
                cb_emit24((u32)sp_bytes);
            } else {
                emit8_24(EZ80_LD_HL_N24, (u32)(0x1000000UL - (sp_bytes & 0xFFFFFFUL)));
                cb_emit8(EZ80_ADD_HL_SP);
                cb_emit8(EZ80_LD_SP_HL);
            }
        }
    } else {
        /* Layout on SP: [local_only vars (0..local_bytes-1)] [params (param_cnt-1 .. 0)] */
        size_t local_bytes = 0;
        int has_scalar_local = 0;
        for (i = param_cnt; i < total_locals; i++) {
            size_t sz = locals[i].size
                            ? locals[i].size
                            : ((locals[i].type == TY_F || locals[i].type == TY_I32) ? 6 : 3);
            if (locals[i].size == 0)
                has_scalar_local = 1;
            ez_local_off.data[i] = local_bytes;
            ez_is_static.data[i] = 0;
            local_bytes += sz;
        }
        ez_frame_bytes = local_bytes;
        for (i = param_cnt; i > 0; i--) {
            size_t p = i - 1;
            size_t sz = (locals[p].type == TY_F || locals[p].type == TY_I32) ? 6 : 3;
            ez_local_off.data[p] = ez_frame_bytes;
            ez_is_static.data[p] = 0;
            ez_frame_bytes += sz;
        }
        if (local_bytes > 0) {
            if (ez_compact) {
                cb_emit8(BC_ENTER);
                cb_emit24((u32)local_bytes);
            } else {
                /* Allocate only non-param locals on SP; params are already on SP above them */
                emit8_24(EZ80_LD_HL_N24, (u32)(0x1000000UL - (local_bytes & 0xFFFFFFUL)));
                cb_emit8(EZ80_ADD_HL_SP);
                cb_emit8(EZ80_LD_SP_HL);
                if (has_scalar_local) {
                    emit8_24(EZ80_LD_DE_N24, 0);
                    for (i = param_cnt; i < total_locals; i++) {
                        if (locals[i].size == 0) {
                            size_t off = ez_local_off.data[i];
                            emit8_24(EZ80_LD_HL_N24, (u32)off);
                            cb_emit8(EZ80_ADD_HL_SP);
                            cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
                            if (locals[i].type == TY_F || locals[i].type == TY_I32) {
                                emit8_24(EZ80_LD_HL_N24, (u32)(off + 3));
                                cb_emit8(EZ80_ADD_HL_SP);
                                cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
                            }
                        }
                    }
                }
            }
        }
    }
}

static void ez_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
    (void)locals;
    (void)total_locals;
    if (ez_compact) {
        if (ez_frame_bytes > 0) {
            size_t i, out_words = 0;
            for (i = 0; i < sig->out_types.len; i++) {
                StackType t = sig->out_types.data[i];
                out_words += (t == TY_F || t == TY_I32) ? 2 : 1;
            }
            if (out_words > 4)
                ir4_die("eZ80 SP-frame return values exceed 12 bytes");
            cb_emit8(BC_LEAVE);
            cb_emit24((u32)ez_frame_bytes);
            cb_emit8((u8)out_words);
        }
        cb_emit8(BC_RET);
        return;
    }
    if (ez_frame_bytes > 0) {
        size_t i, out_words = 0;
        for (i = 0; i < sig->out_types.len; i++) {
            StackType t = sig->out_types.data[i];
            out_words += (t == TY_F || t == TY_I32) ? 2 : 1;
        }
        if (out_words > 4)
            ir4_die("eZ80 SP-frame return values exceed 12 bytes");
        if (out_words >= 1)
            cb_emit8(EZ80_POP_DE);
        if (out_words >= 2)
            cb_emit8(EZ80_POP_BC);
        if (out_words >= 3) {
            cb_emit8(EZ80_POP_HL);
            ez_ld_data_hl(ez_tmp_off);
        }
        if (out_words >= 4) {
            cb_emit8(EZ80_POP_HL);
            ez_ld_data_hl(ez_tmp_off + 3);
        }
        emit8_24(EZ80_LD_HL_N24, (u32)ez_frame_bytes);
        cb_emit8(EZ80_ADD_HL_SP);
        cb_emit8(EZ80_LD_SP_HL);
        if (out_words >= 4) {
            ez_ld_hl_ind_data(ez_tmp_off + 3);
            cb_emit8(EZ80_PUSH_HL);
        }
        if (out_words >= 3) {
            ez_ld_hl_ind_data(ez_tmp_off);
            cb_emit8(EZ80_PUSH_HL);
        }
        if (out_words >= 2)
            cb_emit8(EZ80_PUSH_BC);
        if (out_words >= 1)
            cb_emit8(EZ80_PUSH_DE);
    }

    /* Pop return address from BSS rstack and JP (HL) */
    ez_ld_hl_ind_data(ez_rsp_off);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_DE_MHL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_INC_HL);
    ez_ld_data_hl(ez_rsp_off);
    cb_emit8(EZ80_EX_DE_HL);
    cb_emit8(EZ80_JP_MHL);
}

static void ez_call(size_t target_addr) {
    emit8_24(ez_compact ? BC_CALL : EZ80_CALL, (u32)target_addr);
}

static void ez_call_extern(const FuncSig *sig) {
    if (!sig->has_addr)
        ir4_die("eZ80 extern requires fixed address");
    emit8_24(ez_compact ? BC_CALL : EZ80_CALL, (u32)sig->addr);
}

static void ez_push_w(i32 val) {
    if (ez_compact) {
        bc_emit_push_w(val, 3);
        return;
    }
    emit8_24(EZ80_LD_HL_N24, (u32)val & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_a(u32 val) {
    if (ez_compact) {
        bc_emit_push_a(val, 3);
        return;
    }
    emit8_24(EZ80_LD_HL_N24, val & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_f(double val) {
    float f = (float)val;
    u32 bits;
    memcpy(&bits, &f, 4);
    if (ez_compact) {
        bc_emit_push_f(val);
        return;
    }
    emit8_24(EZ80_LD_HL_N24, (bits >> 24) & 0xFFUL);
    ez_push_hl();
    emit8_24(EZ80_LD_HL_N24, bits & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_i32(i32 val) {
    u32 u = (u32)val;
    if (ez_compact) {
        bc_emit_push_i32(val);
        return;
    }
    emit8_24(EZ80_LD_HL_N24, (u >> 24) & 0xFFUL);
    ez_push_hl();
    emit8_24(EZ80_LD_HL_N24, u & 0xFFFFFFUL);
    ez_push_hl();
}

static void ez_push_data(size_t data_offset) {
    if (ez_compact) {
        bc_emit_push_data(data_offset, 3);
        return;
    }
    ez_ld_hl_data(data_offset);
    ez_push_hl();
}

static void ez_stack_op(StackOp op, StackType t1, StackType t2) {
    int is4_1 = (t1 == TY_F || t1 == TY_I32);
    int is4_2 = (t2 == TY_F || t2 == TY_I32);
    if (ez_compact) {
        bc_emit_stack_op(op, t1, t2);
        return;
    }
    switch (op) {
    case OP_DUP:
        if (!is4_1) {
            cb_emit8(EZ80_POP_HL);
            cb_emit8(EZ80_PUSH_HL);
            ez_push_hl();
        } else {
            cb_emit8(EZ80_POP_HL);
            cb_emit8(EZ80_POP_DE);
            cb_emit8(EZ80_PUSH_DE);
            cb_emit8(EZ80_PUSH_HL);
            cb_emit8(EZ80_PUSH_DE);
            cb_emit8(EZ80_PUSH_HL);
        }
        break;
    case OP_DROP:
        if (!is4_1) {
            ez_pop_hl();
        } else {
            ez_pop_hl();
            ez_pop_hl();
        }
        break;
    case OP_SWAP:
        if (!is4_1 && !is4_2) {
            cb_emit8(EZ80_POP_HL);
            cb_emit8(EZ80_POP_DE);
            cb_emit8(EZ80_PUSH_HL);
            cb_emit8(EZ80_PUSH_DE);
        } else if (is4_1 && !is4_2) {
            cb_emit8(EZ80_POP_HL); /* lo */
            cb_emit8(EZ80_POP_DE); /* hi */
            cb_emit8(EZ80_POP_BC); /* w */
            cb_emit8(EZ80_PUSH_DE); /* hi */
            cb_emit8(EZ80_PUSH_HL); /* lo */
            cb_emit8(EZ80_PUSH_BC);
        } else if (!is4_1 && is4_2) {
            cb_emit8(EZ80_POP_BC); /* w */
            cb_emit8(EZ80_POP_HL); /* lo */
            cb_emit8(EZ80_POP_DE); /* hi */
            cb_emit8(EZ80_PUSH_BC);
            cb_emit8(EZ80_PUSH_DE); /* hi */
            cb_emit8(EZ80_PUSH_HL); /* lo */
        } else {
            ez_ensure_rt_data();
            cb_emit8(EZ80_POP_HL); /* a_lo */
            ez_ld_data_hl(ez_tmp_off);
            cb_emit8(EZ80_POP_HL); /* a_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            cb_emit8(EZ80_POP_DE); /* b_lo */
            cb_emit8(EZ80_POP_BC); /* b_hi */
            ez_ld_hl_ind_data(ez_tmp_off + 3);
            cb_emit8(EZ80_PUSH_HL); /* a_hi */
            ez_ld_hl_ind_data(ez_tmp_off);
            cb_emit8(EZ80_PUSH_HL); /* a_lo */
            cb_emit8(EZ80_PUSH_BC); /* b_hi */
            cb_emit8(EZ80_PUSH_DE); /* b_lo */
        }
        break;
    }
}

static void ez_ensure_mul24(void) {
    size_t loop_pc;
    if (ez_mul24_addr != 0)
        return;
    cb_emit8(EZ80_JP);
    loop_pc = cb_pc();
    cb_emit24(0);
    ez_mul24_addr = cb_cur_addr();
    /* HL = DE * BC (24-bit) */
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8_8(EZ80_LD_A_N, 24);
    {
        size_t l = cb_cur_addr();
        size_t skip_add;
        cb_emit8(EZ80_ADD_HL_HL);
        cb_emit8(EZ80_EX_DE_HL);
        cb_emit8(EZ80_ADD_HL_HL);
        cb_emit8(EZ80_EX_DE_HL);
        cb_emit8(EZ80_JP_NC);
        skip_add = cb_pc();
        cb_emit24(0);
        cb_emit8(EZ80_ADD_HL_BC);
        cb_patch24(skip_add, (u32)cb_cur_addr());
        cb_emit8(EZ80_DEC_A);
        emit8_24(EZ80_JP_NZ, (u32)l);
        cb_emit8(EZ80_RET);
    }
    cb_patch24(loop_pc, (u32)cb_cur_addr());
}

static void ez_ensure_udivmod24(void) {
    size_t skip;
    if (ez_udivmod24_addr != 0)
        return;
    cb_emit8(EZ80_JP);
    skip = cb_pc();
    cb_emit24(0);
    ez_udivmod24_addr = cb_cur_addr();
    /* DE = dividend, BC = divisor -> DE = quotient, HL = remainder */
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8_8(EZ80_LD_A_N, 24);
    {
        size_t l = cb_cur_addr();
        size_t no_sub;
        cb_emit8(EZ80_EX_DE_HL);
        cb_emit8(EZ80_ADD_HL_HL);
        cb_emit8(EZ80_EX_DE_HL);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_ADC_HL_HL);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_BC);
        cb_emit8(EZ80_JP_NC);
        no_sub = cb_pc();
        cb_emit24(0);
        cb_emit8(EZ80_ADD_HL_BC);
        {
            size_t done;
            cb_emit8(EZ80_JP);
            done = cb_pc();
            cb_emit24(0);
            cb_patch24(no_sub, (u32)cb_cur_addr());
            cb_emit8(EZ80_INC_DE);
            cb_patch24(done, (u32)cb_cur_addr());
        }
        cb_emit8(EZ80_DEC_A);
        emit8_24(EZ80_JP_NZ, (u32)l);
        cb_emit8(EZ80_RET);
    }
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ez_ensure_sdivmod24(void) {
    size_t skip, pos_a, pos_b, pos_q;
    ez_ensure_udivmod24();
    if (ez_sdivmod24_addr != 0)
        return;
    cb_emit8(EZ80_JP);
    skip = cb_pc();
    cb_emit24(0);
    ez_sdivmod24_addr = cb_cur_addr();
    /* HL = dividend, DE = divisor -> DE = quotient, HL = remainder */
    ez_ensure_rt_data();
    /* HL=dividend, DE=divisor */
    ez_ld_data_hl(ez_tmp_off);
    cb_emit8(EZ80_EX_DE_HL);
    ez_ld_data_hl(ez_tmp_off + 3);
    /* Save original sign bytes in B (dividend) and C (divisor) */
    cb_emit8(EZ80_LD_A_NN);
    cb_add_data_reloc24(cb_pc(), ez_tmp_off + 2);
    cb_emit24(0);
    cb_emit8(EZ80_LD_B_A);
    cb_emit8(EZ80_LD_A_NN);
    cb_add_data_reloc24(cb_pc(), ez_tmp_off + 5);
    cb_emit24(0);
    cb_emit8(EZ80_LD_C_A);
    cb_emit8(EZ80_PUSH_BC);
    /* Negate dividend if < 0 */
    cb_emit8(EZ80_LD_A_B);
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_P);
    pos_a = cb_pc();
    cb_emit24(0);
    ez_ld_hl_ind_data(ez_tmp_off);
    cb_emit8(EZ80_EX_DE_HL);
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_ED);
    cb_emit8(EZ80_ED_SBC_HL_DE);
    ez_ld_data_hl(ez_tmp_off);
    cb_patch24(pos_a, (u32)cb_cur_addr());

    cb_emit8(EZ80_LD_A_C);
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_P);
    pos_b = cb_pc();
    cb_emit24(0);
    ez_ld_hl_ind_data(ez_tmp_off + 3);
    cb_emit8(EZ80_EX_DE_HL);
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_ED);
    cb_emit8(EZ80_ED_SBC_HL_DE);
    ez_ld_data_hl(ez_tmp_off + 3);
    cb_patch24(pos_b, (u32)cb_cur_addr());

    ez_ld_hl_ind_data(ez_tmp_off);
    cb_emit8(EZ80_EX_DE_HL); /* DE = |dividend| */
    ez_ld_hl_ind_data(ez_tmp_off + 3);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_POP_BC); /* BC = |divisor| */
    emit8_24(EZ80_CALL, (u32)ez_udivmod24_addr);
    cb_emit8(EZ80_POP_BC); /* B = dividend sign, C = divisor sign */
    cb_emit8(EZ80_LD_A_B);
    cb_emit8(EZ80_XOR_C);
    cb_emit8(EZ80_JP_P);
    pos_q = cb_pc();
    cb_emit24(0);
    cb_emit8(EZ80_PUSH_HL); /* save rem */
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_ED);
    cb_emit8(EZ80_ED_SBC_HL_DE);
    cb_emit8(EZ80_EX_DE_HL); /* DE = -quot */
    cb_emit8(EZ80_POP_HL); /* restore rem */
    cb_patch24(pos_q, (u32)cb_cur_addr());
    /* Negate remainder if dividend was negative (sign in B) */
    cb_emit8(EZ80_LD_A_B);
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_P);
    {
        size_t pos_r = cb_pc();
        cb_emit24(0);
        cb_emit8(EZ80_PUSH_DE); /* save quot */
        cb_emit8(EZ80_EX_DE_HL); /* DE = |rem| */
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE); /* HL = -|rem| */
        cb_emit8(EZ80_POP_DE); /* restore quot */
        cb_patch24(pos_r, (u32)cb_cur_addr());
    }
    cb_emit8(EZ80_RET);
    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ez_ensure_shifts(void) {
    size_t skip, done1, done2, done3;
    if (ez_shl24_addr != 0)
        return;
    ez_ensure_rt_data();
    cb_emit8(EZ80_JP);
    skip = cb_pc();
    cb_emit24(0);

    /* shl24: HL << A */
    ez_shl24_addr = cb_cur_addr();
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_Z);
    done1 = cb_pc();
    cb_emit24(0);
    {
        size_t l = cb_cur_addr();
        cb_emit8(EZ80_ADD_HL_HL);
        cb_emit8(EZ80_DEC_A);
        emit8_24(EZ80_JP_NZ, (u32)l);
    }
    cb_patch24(done1, (u32)cb_cur_addr());
    cb_emit8(EZ80_RET);

    /* shr24: HL >> A (unsigned) */
    ez_shr24_addr = cb_cur_addr();
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_Z);
    done2 = cb_pc();
    cb_emit24(0);
    ez_ld_data_hl(ez_tmp_off);
    cb_emit8(EZ80_LD_B_A);
    {
        size_t l = cb_cur_addr();
        ez_ld_hl_data(ez_tmp_off + 2);
        cb_emit8_8(EZ80_CB, EZ80_CB_SRL_MHL);
        cb_emit8(EZ80_DEC_HL);
        cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
        cb_emit8(EZ80_DEC_HL);
        cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
        cb_emit8(EZ80_DJNZ);
        cb_emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
    }
    ez_ld_hl_ind_data(ez_tmp_off);
    cb_patch24(done2, (u32)cb_cur_addr());
    cb_emit8(EZ80_RET);

    /* ashr24: HL >> A (signed) */
    ez_ashr24_addr = cb_cur_addr();
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_Z);
    done3 = cb_pc();
    cb_emit24(0);
    ez_ld_data_hl(ez_tmp_off);
    cb_emit8(EZ80_LD_B_A);
    {
        size_t l = cb_cur_addr();
        ez_ld_hl_data(ez_tmp_off + 2);
        cb_emit8_8(EZ80_CB, EZ80_CB_SRA_MHL);
        cb_emit8(EZ80_DEC_HL);
        cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
        cb_emit8(EZ80_DEC_HL);
        cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
        cb_emit8(EZ80_DJNZ);
        cb_emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
    }
    ez_ld_hl_ind_data(ez_tmp_off);
    cb_patch24(done3, (u32)cb_cur_addr());
    cb_emit8(EZ80_RET);

    cb_patch24(skip, (u32)cb_cur_addr());
}

static void ez_op(IrOp op) {
    if (ez_compact) {
        bc_emit_op(op, 3);
        return;
    }
    switch (op) {
    case OP_ADD:
    case OP_AADD:
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_ADD_HL_DE);
        ez_push_hl();
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        ez_push_hl();
        break;
    case OP_MUL:
        ez_ensure_mul24();
        cb_emit8(EZ80_POP_BC);
        cb_emit8(EZ80_POP_DE);
        emit8_24(EZ80_CALL, (u32)ez_mul24_addr);
        ez_push_hl();
        break;
    case OP_UDIV:
    case OP_UMOD:
        ez_ensure_udivmod24();
        cb_emit8(EZ80_POP_BC); /* BC = divisor */
        cb_emit8(EZ80_POP_DE); /* DE = dividend */
        emit8_24(EZ80_CALL, (u32)ez_udivmod24_addr);
        if (op == OP_UDIV)
            cb_emit8(EZ80_EX_DE_HL);
        ez_push_hl();
        break;
    case OP_DIV:
    case OP_MOD:
        ez_ensure_sdivmod24();
        ez_pop_de();
        ez_pop_hl();
        emit8_24(EZ80_CALL, (u32)ez_sdivmod24_addr);
        if (op == OP_DIV)
            cb_emit8(EZ80_EX_DE_HL);
        ez_push_hl();
        break;
    case OP_AND:
    case OP_OR:
    case OP_XOR: {
        u8 b_op = (op == OP_AND) ? 0xA0 : (op == OP_OR) ? 0xB0 : 0xA8; /* AND/OR/XOR B */
        ez_ensure_rt_data();
        ez_pop_hl();
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl();
        ez_ld_data_hl(ez_tmp_off + 3);
        {
            u8 i;
            for (i = 0; i < 3; i++) {
                cb_emit8(EZ80_LD_A_NN);
                cb_add_data_reloc24(cb_pc(), ez_tmp_off + i);
                cb_emit24(0);
                cb_emit8(EZ80_LD_B_A);
                cb_emit8(EZ80_LD_A_NN);
                cb_add_data_reloc24(cb_pc(), ez_tmp_off + 3 + i);
                cb_emit24(0);
                cb_emit8(b_op);
                cb_emit8(EZ80_LD_NN_A);
                cb_add_data_reloc24(cb_pc(), ez_tmp_off + 3 + i);
                cb_emit24(0);
            }
        }
        ez_ld_hl_ind_data(ez_tmp_off + 3);
        ez_push_hl();
        break;
    }
    case OP_INVERT:
        ez_pop_hl();
        cb_emit8(EZ80_EX_DE_HL);
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        ez_push_hl();
        break;
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        ez_ensure_shifts();
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_LD_A_E);
        cb_emit8_8(EZ80_AND_N, 31);
        emit8_24(EZ80_CALL, (u32)(op == OP_LSHIFT ? ez_shl24_addr
                                              : (op == OP_RSHIFT ? ez_shr24_addr : ez_ashr24_addr)));
        ez_push_hl();
        break;
    case OP_EQ:
    case OP_NE:
    case OP_AEQ:
    case OP_ANE:
    case OP_ULT:
    case OP_UGT:
    case OP_ULE:
    case OP_UGE:
    case OP_ALT:
    case OP_AGT:
    case OP_LT:
    case OP_GT:
    case OP_LE:
    case OP_GE: {
        size_t skip_t;
        u8 jcc;
        if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
            ez_pop_hl();
            ez_pop_de();
        } else {
            ez_pop_de();
            ez_pop_hl();
        }
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        if (op == OP_EQ || op == OP_AEQ)
            jcc = EZ80_JP_Z;
        else if (op == OP_NE || op == OP_ANE)
            jcc = EZ80_JP_NZ;
        else if (op == OP_ULT || op == OP_UGT || op == OP_ALT || op == OP_AGT)
            jcc = EZ80_JP_C;
        else if (op == OP_ULE || op == OP_UGE)
            jcc = EZ80_JP_NC;
        else {
            /* Signed: (S ^ P/V) == 1 for LT/GT, 0 for GE/LE */
            size_t ov_pc, done_pc, t_pc;
            u8 j_noov = (op == OP_LT || op == OP_GT) ? EZ80_JP_M : EZ80_JP_P;
            u8 j_ov = (op == OP_LT || op == OP_GT) ? EZ80_JP_P : EZ80_JP_M;
            cb_emit8(EZ80_JP_PE); /* ov */
            ov_pc = cb_pc();
            cb_emit24(0);
            emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
            cb_emit8(j_noov);
            t_pc = cb_pc();
            cb_emit24(0);
            emit8_24(EZ80_LD_HL_N24, 0);
            cb_emit8(EZ80_JP);
            done_pc = cb_pc();
            cb_emit24(0);
            cb_patch24(ov_pc, (u32)cb_cur_addr());
            emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
            cb_emit8(j_ov);
            skip_t = cb_pc();
            cb_emit24(0);
            emit8_24(EZ80_LD_HL_N24, 0);
            cb_patch24(t_pc, (u32)cb_cur_addr());
            cb_patch24(done_pc, (u32)cb_cur_addr());
            cb_patch24(skip_t, (u32)cb_cur_addr());
            ez_push_hl();
            break;
        }
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_emit8(jcc);
        skip_t = cb_pc();
        cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_patch24(skip_t, (u32)cb_cur_addr());
        ez_push_hl();
        break;
    }
    case OP_U8FETCH:
        ez_pop_hl();
        cb_emit8(EZ80_LD_A_MHL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        break;
    case OP_I8FETCH: {
        size_t pos;
        ez_pop_hl();
        cb_emit8(EZ80_LD_A_MHL);
        cb_emit8(EZ80_OR_A);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_JP_P);
        pos = cb_pc();
        cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_patch24(pos, (u32)cb_cur_addr());
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        break;
    }
    case OP_I8STORE:
        ez_pop_hl();
        ez_pop_de();
        cb_emit8(EZ80_LD_MHL_E);
        break;
    case OP_SI8STORE:
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_LD_MHL_E);
        break;
    case OP_FETCH:
    case OP_AFETCH:
        ez_pop_hl();
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
        ez_push_hl();
        break;
    case OP_STORE:
    case OP_ASTORE:
        ez_pop_hl();
        ez_pop_de();
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
        break;
    case OP_SSTORE:
    case OP_SASTORE:
        ez_pop_de();
        ez_pop_hl();
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
        break;
    case OP_U16FETCH:
        ez_pop_hl();
        cb_emit8(EZ80_LD_E_MHL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_D_MHL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_E);
        cb_emit8(EZ80_LD_H_D);
        ez_push_hl();
        break;
    case OP_I16FETCH: {
        size_t pos;
        ez_pop_hl();
        cb_emit8(EZ80_LD_E_MHL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_D_MHL);
        cb_emit8(EZ80_LD_A_D);
        cb_emit8(EZ80_OR_A);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_JP_P);
        pos = cb_pc();
        cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_patch24(pos, (u32)cb_cur_addr());
        cb_emit8(EZ80_LD_L_E);
        cb_emit8(EZ80_LD_H_D);
        ez_push_hl();
        break;
    }
    case OP_I16STORE:
        ez_pop_hl();
        ez_pop_de();
        cb_emit8(EZ80_LD_MHL_E);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_MHL_D);
        break;
    case OP_SEXT: {
        size_t pos;
        ez_pop_hl();
        cb_emit8(EZ80_LD_A_L);
        cb_emit8(EZ80_OR_A);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_JP_P);
        pos = cb_pc();
        cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_patch24(pos, (u32)cb_cur_addr());
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        break;
    }
    case OP_SI16STORE:
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_LD_MHL_E);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_MHL_D);
        break;
    case OP_BCOPY: {
        size_t skip;
        cb_emit8(EZ80_POP_BC); /* BC = len */
        cb_emit8(EZ80_POP_HL); /* HL = src */
        cb_emit8(EZ80_POP_DE); /* DE = dst */
        cb_emit8(EZ80_PUSH_HL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_BC);
        cb_emit8(EZ80_POP_HL);
        cb_emit8(EZ80_JP_Z);
        skip = cb_pc();
        cb_emit24(0);
        cb_emit8_8(EZ80_ED, EZ80_ED_LDIR);
        cb_patch24(skip, (u32)cb_cur_addr());
        break;
    }
    case OP_W2A:
    case OP_A2W:
        break;
    case OP_W2I32:
        ez_pop_de();
        cb_emit8(EZ80_PUSH_DE);
        cb_emit8(EZ80_POP_HL);
        cb_emit8(EZ80_ADD_HL_HL); /* C = sign */
        cb_emit8(EZ80_SBC_A_A); /* 0xFF or 0x00 */
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_DE);
        break;
    case OP_U2I32:
        ez_pop_de();
        emit8_24(EZ80_LD_HL_N24, 0);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_DE);
        break;
    case OP_I322W:
        ez_pop_hl();
        ez_pop_de();
        ez_push_hl();
        break;
    case OP_I32FETCH:
    case OP_FFETCH:
        ez_pop_hl();
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_DE_MHL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_A_MHL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_DE);
        break;
    case OP_I32STORE:
    case OP_FSTORE:
        ez_pop_hl(); /* addr */
        ez_pop_de(); /* lo 24 */
        cb_emit8(EZ80_POP_BC); /* hi 8 in C */
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_MHL_C);
        break;
    case OP_SFSTORE:
        ez_pop_de(); /* lo 24 */
        cb_emit8(EZ80_POP_BC); /* hi 8 in C */
        ez_pop_hl(); /* addr */
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_INC_HL);
        cb_emit8(EZ80_LD_MHL_C);
        break;
    case OP_ADD32:
        ez_ensure_rt_data();
        ez_pop_de(); /* b_lo */
        ez_pop_hl(); /* b_hi */
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl(); /* a_lo */
        cb_emit8(EZ80_POP_BC); /* a_hi in C */
        cb_emit8(EZ80_ADD_HL_DE);
        cb_emit8(EZ80_EX_DE_HL); /* DE = sum_lo, C preserved */
        ez_ld_hl_data(ez_tmp_off);
        cb_emit8(EZ80_LD_A_C);
        cb_emit8(EZ80_ADC_A_MHL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_DE);
        break;
    case OP_SUB32:
        ez_ensure_rt_data();
        ez_pop_de(); /* b_lo */
        ez_pop_hl(); /* b_hi */
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl(); /* a_lo */
        cb_emit8(EZ80_POP_BC); /* a_hi in C */
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        cb_emit8(EZ80_EX_DE_HL); /* DE = diff_lo, C preserved */
        ez_ld_hl_data(ez_tmp_off);
        cb_emit8(EZ80_LD_A_C);
        cb_emit8(EZ80_SBC_A_MHL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_DE);
        break;
    case OP_INV32:
        ez_pop_de(); /* lo */
        ez_pop_hl(); /* hi */
        cb_emit8(EZ80_LD_A_L);
        cb_emit8(EZ80_CPL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        ez_push_hl();
        break;
    case OP_AND32:
    case OP_OR32:
    case OP_XOR32: {
        u8 alu_op = (op == OP_AND32) ? EZ80_AND_MHL : ((op == OP_OR32) ? EZ80_OR_MHL : EZ80_XOR_MHL);
        u8 back;
        ez_ensure_rt_data();
        /* Pop b (lo 3 bytes, hi 1 byte) to ez_tmp_off..ez_tmp_off+3 */
        ez_pop_hl();
        ez_ld_data_hl(ez_tmp_off);
        ez_pop_hl();
        cb_emit8(EZ80_LD_A_L);
        cb_emit8(EZ80_LD_NN_A);
        cb_add_data_reloc24(cb_pc(), ez_tmp_off + 3);
        cb_emit24(0);
        /* Pop a_lo to ez_tmp_off+4? Use SP directly: a_lo at [SP+0..2], a_hi at [SP+3] */
        ez_ld_hl_data(ez_tmp_off); /* HL = &b[0] */
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_ADD_HL_SP); /* HL = &a[0] */
        cb_emit8(EZ80_EX_DE_HL); /* DE = &a[0] */
        ez_ld_hl_data(ez_tmp_off); /* HL = &b[0] */
        cb_emit8_8(EZ80_LD_B_N, 4);
        back = (u8)cb_cur_addr(); /* wait, use relative */
        (void)back;
        {
            size_t l = cb_cur_addr();
            cb_emit8(EZ80_LD_A_MDE);
            cb_emit8(alu_op); /* AND/OR/XOR (HL) */
            cb_emit8(EZ80_LD_MDE_A);
            cb_emit8(EZ80_INC_DE);
            cb_emit8(EZ80_INC_HL);
            cb_emit8(EZ80_DJNZ);
            cb_emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
        }
        break;
    }
    case OP_EQ32:
    case OP_NE32:
    case OP_ULT32:
    case OP_UGT32:
    case OP_ULE32:
    case OP_UGE32:
    case OP_LT32:
    case OP_GT32:
    case OP_LE32:
    case OP_GE32: {
        size_t cmp_lo_pc, ret_t_pc = 0, ret_f_pc = 0, done_pc;
        int swap = (op == OP_GT32 || op == OP_LE32 || op == OP_UGT32 || op == OP_ULE32);
        ez_ensure_rt_data();
        if (swap) {
            /* Pop rhs into HL/E, lhs into tmp -> actually pop b then a, swap via tmp */
            ez_pop_hl(); /* b_lo */
            ez_ld_data_hl(ez_tmp_off);
            ez_pop_hl(); /* b_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            ez_pop_hl(); /* a_lo -> DE */
            cb_emit8(EZ80_EX_DE_HL);
            ez_pop_hl(); /* a_hi -> C */
            cb_emit8(EZ80_LD_C_L);
            ez_ld_hl_data(ez_tmp_off); /* wait: we need lhs=b, rhs=a */
            /* Swap: b_lo -> HL, b_hi -> E, a_lo -> tmp, a_hi -> tmp+3 */
            cb_emit8(EZ80_EX_DE_HL); /* HL = a_lo */
            cb_emit8(EZ80_PUSH_BC); /* a_hi */
            cb_emit8(EZ80_PUSH_HL); /* a_lo */
            ez_ld_hl_ind_data(ez_tmp_off); /* HL = b_lo */
            cb_emit8(EZ80_EX_DE_HL); /* DE = b_lo */
            ez_ld_hl_ind_data(ez_tmp_off + 3); /* L = b_hi */
            cb_emit8(EZ80_LD_C_L);
            cb_emit8(EZ80_POP_HL); /* HL = a_lo */
            ez_ld_data_hl(ez_tmp_off);
            cb_emit8(EZ80_POP_HL); /* HL = a_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            cb_emit8(EZ80_EX_DE_HL); /* HL = b_lo */
            cb_emit8(EZ80_LD_E_C); /* b_hi) */
        } else {
            ez_pop_hl(); /* b_lo */
            ez_ld_data_hl(ez_tmp_off);
            ez_pop_hl(); /* b_hi */
            ez_ld_data_hl(ez_tmp_off + 3);
            ez_pop_hl(); /* a_lo */
            ez_pop_de(); /* a_hi in E */
        }
        /* Now HL = lhs_lo, E = lhs_hi, tmp = rhs_lo, tmp+3 = rhs_hi */
        cb_emit8(EZ80_PUSH_HL); /* save lhs_lo */
        ez_ld_hl_data(ez_tmp_off + 3);
        cb_emit8(EZ80_LD_A_E); /* lhs_hi) */
        cb_emit8(EZ80_CP_MHL); /* lhs_hi - rhs_hi */
        cb_emit8(EZ80_POP_HL); /* lhs_lo, flags preserved */
        cb_emit8(EZ80_JP_Z); /* cmp_lo */
        cmp_lo_pc = cb_pc();
        cb_emit24(0);
        /* High bytes differ */
        if (op == OP_EQ32) {
            cb_emit8(EZ80_JP);
            ret_f_pc = cb_pc();
            cb_emit24(0);
        } else if (op == OP_NE32) {
            cb_emit8(EZ80_JP);
            ret_t_pc = cb_pc();
            cb_emit24(0);
        } else if (op == OP_ULT32 || op == OP_UGT32 || op == OP_ULE32 || op == OP_UGE32) {
            u8 jcc = (op == OP_ULT32 || op == OP_UGT32) ? EZ80_JP_C : EZ80_JP_NC; /* JC or JNC */
            cb_emit8(jcc);
            ret_t_pc = cb_pc();
            cb_emit24(0);
            cb_emit8(EZ80_JP);
            ret_f_pc = cb_pc();
            cb_emit24(0);
        } else {
            /* Signed 8-bit compare on high bytes: (S ^ P/V) == 1 for LT/GT */
            size_t ov_pc;
            u8 j_noov = (op == OP_LT32 || op == OP_GT32) ? EZ80_JP_M : EZ80_JP_P;
            u8 j_ov = (op == OP_LT32 || op == OP_GT32) ? EZ80_JP_P : EZ80_JP_M;
            cb_emit8(EZ80_JP_PE); /* ov */
            ov_pc = cb_pc();
            cb_emit24(0);
            cb_emit8(j_noov);
            ret_t_pc = cb_pc();
            cb_emit24(0);
            cb_emit8(EZ80_JP);
            ret_f_pc = cb_pc();
            cb_emit24(0);
            cb_patch24(ov_pc, (u32)cb_cur_addr());
            cb_emit8(j_ov);
            /* patch both ret_t_pc? No, jump to ret_t/ret_f below */
            {
                size_t t2 = cb_pc();
                cb_emit24(0);
                cb_emit8(EZ80_JP);
                {
                    size_t f2 = cb_pc();
                    cb_emit24(0);
                    cb_patch24(cmp_lo_pc, (u32)cb_cur_addr());
                    /* Compare unsigned 24-bit HL vs (tmp) */
                    cb_emit8(EZ80_EX_DE_HL); /* DE = lhs_lo */
                    ez_ld_hl_ind_data(ez_tmp_off); /* HL = rhs_lo */
                    cb_emit8(EZ80_EX_DE_HL); /* HL = lhs_lo, DE = rhs_lo */
                    cb_emit8(EZ80_OR_A);
                    cb_emit8(EZ80_ED);
                    cb_emit8(EZ80_ED_SBC_HL_DE);
                    if (op == OP_EQ32) {
                        cb_emit8(EZ80_JP_Z);
                        ret_t_pc = cb_pc();
                        cb_emit24(0);
                        cb_emit8(EZ80_JP);
                        ret_f_pc = cb_pc();
                        cb_emit24(0);
                    } else if (op == OP_NE32) {
                        cb_emit8(EZ80_JP_NZ);
                        ret_t_pc = cb_pc();
                        cb_emit24(0);
                        cb_emit8(EZ80_JP);
                        ret_f_pc = cb_pc();
                        cb_emit24(0);
                    } else {
                        u8 jcc = (op == OP_LT32 || op == OP_GT32) ? EZ80_JP_C : EZ80_JP_NC;
                        cb_emit8(jcc);
                        {
                            size_t t3 = cb_pc();
                            cb_emit24(0);
                            cb_patch24(ret_f_pc, (u32)cb_cur_addr());
                            cb_patch24(f2, (u32)cb_cur_addr());
                            emit8_24(EZ80_LD_HL_N24, 0);
                            cb_emit8(EZ80_JP);
                            done_pc = cb_pc();
                            cb_emit24(0);
                            cb_patch24(ret_t_pc, (u32)cb_cur_addr());
                            cb_patch24(t2, (u32)cb_cur_addr());
                            cb_patch24(t3, (u32)cb_cur_addr());
                            emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
                            cb_patch24(done_pc, (u32)cb_cur_addr());
                        }
                    }
                }
            }
            if (op == OP_EQ32 || op == OP_NE32) {
                /* unreachable for signed */
            }
            ez_push_hl();
            break;
        }
        cb_patch24(cmp_lo_pc, (u32)cb_cur_addr());
        cb_emit8(EZ80_EX_DE_HL);
        ez_ld_hl_ind_data(ez_tmp_off);
        cb_emit8(EZ80_EX_DE_HL);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        {
            u8 jcc = (op == OP_EQ32)
                         ? EZ80_JP_Z
                         : ((op == OP_NE32)
                                ? EZ80_JP_NZ
                                : ((op == OP_ULT32 || op == OP_UGT32) ? EZ80_JP_C : EZ80_JP_NC));
            size_t t3;
            cb_emit8(jcc);
            t3 = cb_pc();
            cb_emit24(0);
            if (op != OP_NE32)
                cb_patch24(ret_f_pc, (u32)cb_cur_addr());
            emit8_24(EZ80_LD_HL_N24, 0);
            cb_emit8(EZ80_JP);
            done_pc = cb_pc();
            cb_emit24(0);
            if (op != OP_EQ32)
                cb_patch24(ret_t_pc, (u32)cb_cur_addr());
            cb_patch24(t3, (u32)cb_cur_addr());
            emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
            cb_patch24(done_pc, (u32)cb_cur_addr());
        }
        ez_push_hl();
        break;
    }
    case OP_FNEG:
        ez_pop_de(); /* lo */
        ez_pop_hl(); /* hi */
        cb_emit8(EZ80_LD_A_L);
        cb_emit8_8(EZ80_XOR_N, 0x80);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_DE);
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV: {
        u16 entry = (op == OP_FADD)   ? EZ80_RT_FADD
                    : (op == OP_FSUB) ? EZ80_RT_FSUB
                    : (op == OP_FMUL) ? EZ80_RT_FMUL
                                      : EZ80_RT_FDIV;
        ez_ensure_rt();
        /* Pop b into E:HL, a into A:BC */
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_EX_DE_HL); /* HL = b_lo, E = b_hi */
        cb_emit8(EZ80_POP_BC); /* BC = a_lo */
        cb_emit8(EZ80_EX_SP_HL); /* HL = a_hi, [SP] = b_lo */
        cb_emit8(EZ80_LD_A_L); /* a_hi */
        cb_emit8(EZ80_POP_HL); /* b_lo */
        emit8_24(EZ80_CALL, (u32)(ez_rt_addr + entry));
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_BC);
        break;
    }
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT: {
        size_t skip_t;
        u8 jcc = (op == OP_FEQ) ? EZ80_JP_Z : ((op == OP_FNE) ? EZ80_JP_NZ : EZ80_JP_M);
        ez_ensure_fcmp();
        if (op == OP_FLT) {
            /* ez_fcmp compares E:HL vs A:BC; for FLT we want E:HL=a, A:BC=b */
            cb_emit8(EZ80_POP_BC); /* BC = b_lo */
            cb_emit8(EZ80_POP_HL); /* L = b_hi */
            cb_emit8(EZ80_LD_A_L); /* A = b_hi */
            ez_pop_de(); /* DE = a_lo */
            ez_pop_hl(); /* L = a_hi */
            cb_emit8(EZ80_EX_DE_HL); /* HL = a_lo, E = a_hi */
        } else {
            ez_pop_de();
            ez_pop_hl();
            cb_emit8(EZ80_EX_DE_HL); /* HL = b_lo, E = b_hi */
            cb_emit8(EZ80_POP_BC); /* BC = a_lo */
            cb_emit8(EZ80_EX_SP_HL);
            cb_emit8(EZ80_LD_A_L);
            cb_emit8(EZ80_POP_HL);
        }
        emit8_24(EZ80_CALL, (u32)ez_fcmp_addr);
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_emit8(jcc);
        skip_t = cb_pc();
        cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_patch24(skip_t, (u32)cb_cur_addr());
        ez_push_hl();
        break;
    }
    case OP_W2F:
        ez_ensure_rt();
        ez_pop_hl();
        cb_emit8(EZ80_PUSH_HL);
        cb_emit8(EZ80_POP_BC); /* BC = HL */
        cb_emit8(EZ80_ADD_HL_HL); /* C = sign */
        cb_emit8(EZ80_SBC_A_A);
        emit8_24(EZ80_CALL, (u32)(ez_rt_addr + EZ80_RT_LTOF));
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        ez_push_hl();
        cb_emit8(EZ80_PUSH_BC);
        break;
    case OP_F2W:
        ez_ensure_rt();
        cb_emit8(EZ80_POP_BC); /* BC = lo */
        ez_pop_hl(); /* L = hi */
        cb_emit8(EZ80_LD_A_L);
        emit8_24(EZ80_CALL, (u32)(ez_rt_addr + EZ80_RT_FTOL));
        cb_emit8(EZ80_PUSH_BC);
        break;
    case OP_MUL32:
        ez_ensure_rt();
        ez_pop_de();
        ez_pop_hl();
        cb_emit8(EZ80_EX_DE_HL);
        cb_emit8(EZ80_POP_BC);
        cb_emit8(EZ80_EX_SP_HL);
        cb_emit8(EZ80_LD_A_L);
        cb_emit8(EZ80_POP_HL);
        emit8_24(EZ80_CALL, (u32)(ez_rt_addr + EZ80_RT_LMULS));
        /* Result in E:HL */
        cb_emit8(EZ80_LD_BC_N24);
        cb_emit24(0);
        cb_emit8(EZ80_LD_C_E);
        cb_emit8(EZ80_PUSH_BC);
        cb_emit8(EZ80_PUSH_HL);
        break;
    case OP_UDIV32:
    case OP_UMOD32:
    case OP_DIV32:
    case OP_MOD32: {
        u16 entry = (op == OP_UDIV32)   ? EZ80_RT_LDIVU
                    : (op == OP_UMOD32) ? EZ80_RT_LREMU
                    : (op == OP_DIV32)  ? EZ80_RT_LDIVS
                                        : EZ80_RT_LREMS;
        ez_ensure_rt();
        /* E:HL = a, A:BC = b */
        cb_emit8(EZ80_POP_BC); /* BC = b_lo */
        ez_pop_hl(); /* L = b_hi */
        cb_emit8(EZ80_LD_A_L); /* A = b_hi */
        ez_pop_de(); /* DE = a_lo */
        ez_pop_hl(); /* L = a_hi */
        cb_emit8(EZ80_EX_DE_HL); /* HL = a_lo, E = a_hi */
        emit8_24(EZ80_CALL, (u32)(ez_rt_addr + entry));
        cb_emit8(EZ80_LD_BC_N24);
        cb_emit24(0);
        cb_emit8(EZ80_LD_C_E);
        cb_emit8(EZ80_PUSH_BC);
        cb_emit8(EZ80_PUSH_HL);
        break;
    }
    case OP_SHL32:
    case OP_RSHR32:
    case OP_ASHR32: {
        size_t skip;
        size_t l;
        ez_pop_hl(); /* cnt */
        cb_emit8(EZ80_LD_A_L);
        cb_emit8_8(EZ80_AND_N, 31);
        cb_emit8(EZ80_JP_Z);
        skip = cb_pc();
        cb_emit24(0);
        cb_emit8(EZ80_LD_B_A);
        emit8_24(EZ80_LD_HL_N24, (op == OP_SHL32) ? 0 : 3);
        cb_emit8(EZ80_ADD_HL_SP);
        l = cb_cur_addr();
        if (op == OP_SHL32) {
            cb_emit8_8(EZ80_CB, EZ80_CB_SLA_MHL);
            cb_emit8(EZ80_INC_HL);
            cb_emit8_8(EZ80_CB, EZ80_CB_RL_MHL);
            cb_emit8(EZ80_INC_HL);
            cb_emit8_8(EZ80_CB, EZ80_CB_RL_MHL);
            cb_emit8(EZ80_INC_HL);
            cb_emit8_8(EZ80_CB, EZ80_CB_RL_MHL);
            cb_emit8(EZ80_DEC_HL);
            cb_emit8(EZ80_DEC_HL);
            cb_emit8(EZ80_DEC_HL);
        } else {
            cb_emit8_8(EZ80_CB, (op == OP_RSHR32) ? 0x3E : 0x2E);
            cb_emit8(EZ80_DEC_HL);
            cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
            cb_emit8(EZ80_DEC_HL);
            cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
            cb_emit8(EZ80_DEC_HL);
            cb_emit8_8(EZ80_CB, EZ80_CB_RR_MHL);
            cb_emit8(EZ80_INC_HL);
            cb_emit8(EZ80_INC_HL);
            cb_emit8(EZ80_INC_HL);
        }
        cb_emit8(EZ80_DJNZ);
        cb_emit8((u8)((i8)((int)l - (int)(cb_cur_addr() + 1))));
        cb_patch24(skip, (u32)cb_cur_addr());
        break;
    }
    default:
        ir4_die("unimplemented eZ80 op");
    }
}

static int ez_add_imm(int is_addr, i32 imm) {
    (void)is_addr;
    if (ez_compact)
        return bc_emit_add_imm(imm);
    return 0;
}

static int ez_cmp_imm(IrOp op, i32 imm) {
    (void)op;
    (void)imm;
    return 0;
}

static int ez_shift_imm(IrOp op, u8 count) {
    (void)op;
    (void)count;
    return 0;
}

static int ez_bit_imm(IrOp op, i32 imm) {
    u32 v = (u32)imm & 0xFFFFFFUL;
    if (ez_compact)
        return 0;
    if (op != OP_AND && op != OP_OR && op != OP_XOR)
        return 0;
    if (v > 0xFF)
        return 0;
    ez_pop_hl();
    if (op == OP_AND) {
        if (v == 0) {
            emit8_24(EZ80_LD_HL_N24, 0);
        } else {
            cb_emit8(EZ80_LD_A_L);
            if (v != 0xFF)
                cb_emit8_8(0xE6, (u8)v); /* AND imm */
            emit8_24(EZ80_LD_HL_N24, 0);
            cb_emit8(EZ80_LD_L_A);
        }
    } else if (op == OP_OR) {
        if (v != 0) {
            cb_emit8(EZ80_LD_A_L);
            cb_emit8_8(0xF6, (u8)v); /* OR imm */
            cb_emit8(EZ80_LD_L_A);
        }
    } else {
        if (v != 0) {
            cb_emit8(EZ80_LD_A_L);
            cb_emit8_8(0xEE, (u8)v); /* XOR imm */
            cb_emit8(EZ80_LD_L_A);
        }
    }
    ez_push_hl();
    return 1;
}

static int ez_mem_imm(IrOp op, i32 off) {
    (void)op;
    (void)off;
    return 0;
}

static int ez_data_mem(IrOp op, size_t data_offset) {
    (void)op;
    (void)data_offset;
    return 0;
}

static int ez_local_mem(size_t slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)slot;
    (void)off;
    (void)op;
    (void)stack_bytes;
    return 0;
}

static size_t ez_emit_br_fwd(BranchCond cond) {
    size_t pos;
    if (ez_compact)
        return bc_emit_br_fwd_op(cond, 3);
    if (cond == BR_JZ) {
        ez_pop_hl();
        cb_emit8(EZ80_EX_DE_HL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        cb_emit8(EZ80_JP_Z);
        pos = cb_pc();
        cb_emit24(0);
        return pos;
    }
    cb_emit8(EZ80_JP);
    pos = cb_pc();
    cb_emit24(0);
    return pos;
}

static void ez_emit_br_back(BranchCond cond, size_t target_addr) {
    if (ez_compact) {
        bc_emit_br_back_op(cond, target_addr, 3);
        return;
    }
    if (cond == BR_JZ) {
        ez_pop_hl();
        cb_emit8(EZ80_EX_DE_HL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        emit8_24(EZ80_JP_Z, (u32)target_addr);
    } else {
        emit8_24(EZ80_JP, (u32)target_addr);
    }
}

static int ez_cmp_br_emit(IrOp op, int has_imm, i32 imm, int is_fwd, size_t target_addr,
                          size_t *patch_offset) {
    u8 swap_ops;
    u8 jcc = 0;

    if (ez_compact)
        return 0;

    if (op < OP_EQ || op > OP_AGT)
        return 0;

    if (has_imm && imm == 0 && (op == OP_EQ || op == OP_NE || op == OP_AEQ || op == OP_ANE)) {
        ez_pop_hl();
        cb_emit8(EZ80_EX_DE_HL);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_ED);
        cb_emit8(EZ80_ED_SBC_HL_DE);
        jcc = (op == OP_EQ || op == OP_AEQ) ? EZ80_JP_NZ : EZ80_JP_Z;
        if (is_fwd) {
            cb_emit8(jcc);
            *patch_offset = cb_pc();
            cb_emit24(0);
        } else {
            emit8_24(jcc, (u32)target_addr);
        }
        return 1;
    }

    swap_ops = (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT);
    if (has_imm) {
        if (swap_ops) {
            ez_pop_de();
            emit8_24(EZ80_LD_HL_N24, (u32)imm);
        } else {
            ez_pop_hl();
            emit8_24(EZ80_LD_DE_N24, (u32)imm);
        }
    } else {
        if (swap_ops) {
            ez_pop_hl();
            ez_pop_de();
        } else {
            ez_pop_de();
            ez_pop_hl();
        }
    }

    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_ED);
    cb_emit8(EZ80_ED_SBC_HL_DE);

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        jcc = EZ80_JP_NZ;
        break;
    case OP_NE:
    case OP_ANE:
        jcc = EZ80_JP_Z;
        break;
    case OP_ULT:
    case OP_UGT:
    case OP_ALT:
    case OP_AGT:
        jcc = EZ80_JP_NC;
        break;
    case OP_UGE:
    case OP_ULE:
        jcc = EZ80_JP_C;
        break;
    case OP_LT:
    case OP_GT:
    case OP_GE:
    case OP_LE: {
        u8 is_lt = (op == OP_LT || op == OP_GT);
        size_t ov_pc;
        cb_emit8(EZ80_JP_PE);
        ov_pc = cb_pc();
        cb_emit24(0);
        if (is_fwd) {
            size_t skip1_pc, skip2_pc;
            u8 skip_noov = is_lt ? EZ80_JP_M : EZ80_JP_P;
            u8 skip_ov = is_lt ? EZ80_JP_P : EZ80_JP_M;
            cb_emit8(skip_noov);
            skip1_pc = cb_pc();
            cb_emit24(0);
            cb_emit8_8(0x18, 4); /* JR +4 */
            cb_patch24(ov_pc, (u32)cb_cur_addr());
            cb_emit8(skip_ov);
            skip2_pc = cb_pc();
            cb_emit24(0);
            cb_emit8(EZ80_JP);
            *patch_offset = cb_pc();
            cb_emit24(0);
            cb_patch24(skip1_pc, (u32)cb_cur_addr());
            cb_patch24(skip2_pc, (u32)cb_cur_addr());
        } else {
            size_t skip_pc;
            u8 j_noov = is_lt ? EZ80_JP_P : EZ80_JP_M;
            u8 j_ov = is_lt ? EZ80_JP_M : EZ80_JP_P;
            emit8_24(j_noov, (u32)target_addr);
            cb_emit8(EZ80_JP);
            skip_pc = cb_pc();
            cb_emit24(0);
            cb_patch24(ov_pc, (u32)cb_cur_addr());
            emit8_24(j_ov, (u32)target_addr);
            cb_patch24(skip_pc, (u32)cb_cur_addr());
        }
        return 1;
    }
    default:
        return 0;
    }

    if (is_fwd) {
        cb_emit8(jcc);
        *patch_offset = cb_pc();
        cb_emit24(0);
    } else {
        emit8_24(jcc, (u32)target_addr);
    }
    return 1;
}

static int ez_cmp_br_fwd(IrOp op, int has_imm, i32 imm, size_t *patch_offset) {
    return ez_cmp_br_emit(op, has_imm, imm, 1, 0, patch_offset);
}

static int ez_cmp_br_back(IrOp op, int has_imm, i32 imm, size_t target_addr) {
    return ez_cmp_br_emit(op, has_imm, imm, 0, target_addr, NULL);
}

static size_t ez_emit_jtab(size_t count) {
    size_t tbl_addr_pc, tbl_off, k;
    if (ez_compact) {
        cb_emit8(BC_JTAB);
        cb_emit24((u32)count);
        tbl_off = cb_pc();
        for (k = 0; k < count; k++)
            cb_emit24(0);
        return tbl_off;
    }
    ez_pop_hl();
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_POP_DE);
    cb_emit8(EZ80_ADD_HL_HL);
    cb_emit8(EZ80_ADD_HL_DE);
    cb_emit8(EZ80_LD_DE_N24);
    tbl_addr_pc = cb_pc();
    cb_emit24(0);
    cb_emit8(EZ80_ADD_HL_DE);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_JP_MHL);
    cb_patch24(tbl_addr_pc, (u32)cb_cur_addr());
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit24(0);
    return tbl_off;
}

static void ez_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    cb_patch24(patch_offset + idx * 3, (u32)target_addr);
}

static void ez_patch_fwd(size_t patch_offset) { cb_patch24(patch_offset, (u32)cb_cur_addr()); }

static void ez_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    cb_patch24(call_pc + 1, (u32)target_addr);
}

static size_t ez_push_fn(size_t target_addr) {
    size_t pos;
    if (ez_compact) {
        cb_emit8(BC_PUSH_DATA);
        pos = cb_pc();
        cb_emit24((u32)target_addr);
        return pos;
    }
    cb_emit8(EZ80_LD_HL_N24);
    pos = cb_pc();
    cb_emit24((u32)target_addr);
    ez_push_hl();
    return pos;
}

static void ez_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    cb_patch24(patch_pc, (u32)target_addr);
}

static void ez_icall(void) {
    size_t ret_pc;
    if (ez_compact) {
        cb_emit8(BC_ICALL);
        return;
    }
    ez_pop_hl();
    emit8_24(EZ80_LD_DE_N24, 0); /* ret_addr */
    ret_pc = cb_pc() - 3;
    cb_emit8(EZ80_PUSH_DE);
    cb_emit8(EZ80_JP_MHL);
    cb_patch24(ret_pc, (u32)cb_cur_addr());
}

static size_t ez_stack_data_off(void) { return 0; }

static void ez_vm_emit_read_a_ip(void) {
    ez_ld_hl_ind_data(ez_vm_ip_off);
    cb_emit8(EZ80_LD_A_MHL);
    cb_emit8(EZ80_INC_HL);
    ez_ld_data_hl(ez_vm_ip_off);
}

static void ez_ensure_bc_vm(void) {
    int saved = ez_compact;
    size_t skip_vm, vm_tbl_pc, vm_tbl_addr;
    size_t rpush_addr, rpop_addr, read_u24_addr;
    size_t vm_next_addr, vm_restore_ip_addr, vm_call_tmp_addr;
    size_t bmi_imm7_pc;
    size_t handlers[BC_STORE16 + 1];
    size_t i;

    if (ez_vm_enter_addr != 0)
        return;

    ez_compact = 0;
    ez_ensure_rt_data();
    for (i = 0; i <= BC_STORE16; i++)
        handlers[i] = 0;

    cb_emit8(EZ80_JP);
    skip_vm = cb_pc();
    cb_emit24(0);

    /* rpush_addr: push DE onto ez_rstack (clobbers HL) */
    rpush_addr = cb_cur_addr();
    ez_ld_hl_ind_data(ez_rsp_off);
    cb_emit8(EZ80_DEC_HL);
    cb_emit8(EZ80_DEC_HL);
    cb_emit8(EZ80_DEC_HL);
    ez_ld_data_hl(ez_rsp_off);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    cb_emit8(EZ80_RET);

    /* rpop_addr: pop ez_rstack into DE (clobbers HL) */
    rpop_addr = cb_cur_addr();
    ez_ld_hl_ind_data(ez_rsp_off);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_DE_MHL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_INC_HL);
    ez_ld_data_hl(ez_rsp_off);
    cb_emit8(EZ80_RET);

    /* read_u24_addr: read 24-bit value at (ez_vm_ip) into HL, advance by 3 (clobbers DE) */
    read_u24_addr = cb_cur_addr();
    ez_ld_hl_ind_data(ez_vm_ip_off);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_DE_MHL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_INC_HL);
    ez_ld_data_hl(ez_vm_ip_off);
    cb_emit8(EZ80_EX_DE_HL);
    cb_emit8(EZ80_RET);

    /* vm_enter: SP = [new_ip, caller_ret, ...] */
    ez_vm_enter_addr = cb_cur_addr();
    cb_emit8(EZ80_POP_HL);
    ez_ld_data_hl(ez_vm_ip_off);
    cb_emit8(EZ80_POP_DE);
    emit8_24(EZ80_CALL, (u32)rpush_addr);

    /* vm_next */
    vm_next_addr = cb_cur_addr();
    ez_vm_emit_read_a_ip();
    cb_emit8(EZ80_OR_A);
    cb_emit8(EZ80_JP_M);
    bmi_imm7_pc = cb_pc();
    cb_emit24(0);
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_LD_L_A);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_POP_DE);
    cb_emit8(EZ80_ADD_HL_HL);
    cb_emit8(EZ80_ADD_HL_DE);
    cb_emit8(EZ80_LD_DE_N24);
    {
        size_t tbl_ref_pc = cb_pc();
        cb_emit24(0);
        cb_emit8(EZ80_ADD_HL_DE);
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
        cb_emit8(EZ80_JP_MHL);

        /* push_imm7 */
        cb_patch24(bmi_imm7_pc, (u32)cb_cur_addr());
        cb_emit8_8(EZ80_AND_N, 0x7F);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_LD_L_A);
        cb_emit8(EZ80_PUSH_HL);
        emit8_24(EZ80_JP, (u32)vm_next_addr);

        /* vm_restore_ip */
        vm_restore_ip_addr = cb_cur_addr();
        emit8_24(EZ80_CALL, (u32)rpop_addr);
        cb_emit8(EZ80_EX_DE_HL);
        ez_ld_data_hl(ez_vm_ip_off);
        emit8_24(EZ80_JP, (u32)vm_next_addr);

        /* vm_call_tmp: target in HL */
        vm_call_tmp_addr = cb_cur_addr();
        ez_ld_data_hl(ez_tmp_off);
        ez_ld_hl_ind_data(ez_vm_ip_off);
        cb_emit8(EZ80_EX_DE_HL);
        emit8_24(EZ80_CALL, (u32)rpush_addr);
        emit8_24(EZ80_LD_DE_N24, (u32)vm_restore_ip_addr);
        cb_emit8(EZ80_PUSH_DE);
        ez_ld_hl_ind_data(ez_tmp_off);
        cb_emit8(EZ80_JP_MHL);

        /* Jump table */
        vm_tbl_pc = cb_pc();
        vm_tbl_addr = cb_cur_addr();
        cb_patch24(tbl_ref_pc, (u32)vm_tbl_addr);
        for (i = 0; i <= BC_STORE16; i++)
            cb_emit24(0);
    }

    /* BC_RET */
    handlers[BC_RET] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)rpop_addr);
    cb_emit8(EZ80_EX_DE_HL);
    cb_emit8(EZ80_JP_MHL);

    /* BC_ENTER <u24 frame_bytes> */
    handlers[BC_ENTER] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_POP_BC); /* BC = frame_bytes */
    cb_emit8(EZ80_EX_DE_HL);
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_OR_A);
    cb_emit8_8(EZ80_ED, EZ80_ED_SBC_HL_DE);
    cb_emit8(EZ80_ADD_HL_SP);
    cb_emit8(EZ80_LD_SP_HL);
    cb_emit8_8(0x36, 0x00); /* LD (HL), 0 */
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_POP_DE);
    cb_emit8(EZ80_INC_DE);
    cb_emit8(0x0B); /* DEC BC */
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_OR_A);
    cb_emit8_8(EZ80_ED, 0x4A); /* ADC HL, BC */
    cb_emit8(EZ80_POP_HL);
    emit8_24(EZ80_JP_Z, (u32)vm_next_addr);
    cb_emit8_8(EZ80_ED, EZ80_ED_LDIR);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_LEAVE <u24 frame_bytes> <u8 out_words> */
    handlers[BC_LEAVE] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    ez_ld_data_hl(ez_vm_leave_off);
    ez_vm_emit_read_a_ip();
    cb_emit8(0x32);
    cb_add_data_reloc24(cb_pc(), ez_vm_leave_off + 3);
    cb_emit24(0);
    {
        size_t s1, s2, s3, s4, r4, r3, r2, r1;
        cb_emit8(EZ80_OR_A);
        cb_emit8(EZ80_JP_Z); s1 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_POP_DE);
        cb_emit8_8(0xFE, 2); cb_emit8(EZ80_JP_C); s2 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_POP_BC);
        cb_emit8_8(0xFE, 3); cb_emit8(EZ80_JP_C); s3 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_POP_HL);
        ez_ld_data_hl(ez_tmp_off);
        cb_emit8_8(0xFE, 4); cb_emit8(EZ80_JP_C); s4 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_POP_HL);
        ez_ld_data_hl(ez_tmp_off + 3);
        cb_patch24(s1, (u32)cb_cur_addr());
        cb_patch24(s2, (u32)cb_cur_addr());
        cb_patch24(s3, (u32)cb_cur_addr());
        cb_patch24(s4, (u32)cb_cur_addr());

        ez_ld_hl_ind_data(ez_vm_leave_off);
        cb_emit8(EZ80_ADD_HL_SP);
        cb_emit8(EZ80_LD_SP_HL);

        cb_emit8(0x3A);
        cb_add_data_reloc24(cb_pc(), ez_vm_leave_off + 3);
        cb_emit24(0);
        cb_emit8_8(0xFE, 4); cb_emit8(EZ80_JP_C); r4 = cb_pc(); cb_emit24(0);
        ez_ld_hl_ind_data(ez_tmp_off + 3);
        cb_emit8(EZ80_PUSH_HL);
        cb_patch24(r4, (u32)cb_cur_addr());
        cb_emit8_8(0xFE, 3); cb_emit8(EZ80_JP_C); r3 = cb_pc(); cb_emit24(0);
        ez_ld_hl_ind_data(ez_tmp_off);
        cb_emit8(EZ80_PUSH_HL);
        cb_patch24(r3, (u32)cb_cur_addr());
        cb_emit8_8(0xFE, 2); cb_emit8(EZ80_JP_C); r2 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_PUSH_BC);
        cb_patch24(r2, (u32)cb_cur_addr());
        cb_emit8(EZ80_OR_A); cb_emit8(EZ80_JP_Z); r1 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_PUSH_DE);
        cb_patch24(r1, (u32)cb_cur_addr());
        emit8_24(EZ80_JP, (u32)vm_next_addr);
    }

    /* BC_PUSH_0_32 */
    handlers[BC_PUSH_0_32] = cb_cur_addr();
    emit8_24(EZ80_LD_HL_N24, 0);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_PUSH_I8 <i8> */
    handlers[BC_PUSH_I8] = cb_cur_addr();
    ez_vm_emit_read_a_ip();
    cb_emit8(EZ80_OR_A);
    {
        size_t neg_pc, done_pc;
        cb_emit8(EZ80_JP_M); neg_pc = cb_pc(); cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_JP); done_pc = cb_pc(); cb_emit24(0);
        cb_patch24(neg_pc, (u32)cb_cur_addr());
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_patch24(done_pc, (u32)cb_cur_addr());
    }
    cb_emit8(EZ80_LD_L_A);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_PUSH_W <i16> */
    handlers[BC_PUSH_W] = cb_cur_addr();
    ez_ld_hl_ind_data(ez_vm_ip_off);
    cb_emit8(EZ80_LD_E_MHL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_LD_D_MHL);
    cb_emit8(EZ80_INC_HL);
    ez_ld_data_hl(ez_vm_ip_off);
    cb_emit8(EZ80_LD_A_D);
    cb_emit8(EZ80_OR_A);
    {
        size_t neg_pc, done_pc;
        cb_emit8(EZ80_JP_M); neg_pc = cb_pc(); cb_emit24(0);
        emit8_24(EZ80_LD_HL_N24, 0);
        cb_emit8(EZ80_JP); done_pc = cb_pc(); cb_emit24(0);
        cb_patch24(neg_pc, (u32)cb_cur_addr());
        emit8_24(EZ80_LD_HL_N24, 0xFFFFFFUL);
        cb_patch24(done_pc, (u32)cb_cur_addr());
    }
    cb_emit8(EZ80_LD_H_D);
    cb_emit8(EZ80_LD_L_E);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_PUSH_I32 / BC_PUSH_F <u32:4> */
    handlers[BC_PUSH_I32] = cb_cur_addr();
    handlers[BC_PUSH_F] = handlers[BC_PUSH_I32];
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_PUSH_HL); /* save lo24 temporarily */
    ez_vm_emit_read_a_ip();
    cb_emit8(EZ80_POP_HL); /* lo24 */
    emit8_24(EZ80_LD_DE_N24, 0);
    cb_emit8(0x5F); /* LD E, A (hi8) */
    cb_emit8(EZ80_PUSH_DE);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_PUSH_DATA / BC_PUSH_FN <u24> */
    handlers[BC_PUSH_DATA] = cb_cur_addr();
    handlers[BC_PUSH_FN] = handlers[BC_PUSH_DATA];
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_INC / BC_DEC / BC_ADD_IMM8 */
    handlers[BC_INC] = cb_cur_addr();
    cb_emit8(EZ80_POP_HL);
    cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_DEC] = cb_cur_addr();
    cb_emit8(EZ80_POP_HL);
    cb_emit8(EZ80_DEC_HL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_ADD_IMM8] = cb_cur_addr();
    ez_vm_emit_read_a_ip();
    cb_emit8(EZ80_OR_A);
    {
        size_t neg_pc, done_pc;
        cb_emit8(EZ80_JP_M); neg_pc = cb_pc(); cb_emit24(0);
        emit8_24(EZ80_LD_DE_N24, 0);
        cb_emit8(EZ80_JP); done_pc = cb_pc(); cb_emit24(0);
        cb_patch24(neg_pc, (u32)cb_cur_addr());
        emit8_24(EZ80_LD_DE_N24, 0xFFFFFFUL);
        cb_patch24(done_pc, (u32)cb_cur_addr());
    }
    cb_emit8(0x5F); /* LD E, A */
    cb_emit8(EZ80_POP_HL);
    cb_emit8(EZ80_ADD_HL_DE);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_STATIC_GET / GET32 / SET / SET32 */
    handlers[BC_STATIC_GET] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_STATIC_GET32] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_EX_SP_HL);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_STATIC_SET] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_POP_DE);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_STATIC_SET32] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_POP_DE);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_POP_DE);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_LOCAL_ADDR / GET / GET32 / SET / SET32 */
    handlers[BC_LOCAL_ADDR] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_ADD_HL_SP);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_LOCAL_GET] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_ADD_HL_SP);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_LOCAL_GET32] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_ADD_HL_SP);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_EX_SP_HL);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
    cb_emit8(EZ80_PUSH_HL);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_LOCAL_SET] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_POP_DE);
    cb_emit8(EZ80_ADD_HL_SP);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_LOCAL_SET32] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_POP_DE);
    cb_emit8(EZ80_POP_BC);
    cb_emit8(EZ80_ADD_HL_SP);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL); cb_emit8(EZ80_INC_HL);
    cb_emit8(EZ80_PUSH_BC);
    cb_emit8(EZ80_POP_DE);
    cb_emit8_8(EZ80_ED, EZ80_ED_LD_MHL_DE);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* BC_JMP / BC_JZ / BC_JTAB / BC_CALL / BC_ICALL */
    handlers[BC_JMP] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    ez_ld_data_hl(ez_vm_ip_off);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_JZ] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_EX_DE_HL); /* DE = target */
    cb_emit8(EZ80_POP_HL);
    emit8_24(EZ80_LD_BC_N24, 0);
    cb_emit8(EZ80_OR_A);
    cb_emit8_8(EZ80_ED, EZ80_ED_SBC_HL_BC);
    emit8_24(EZ80_JP_NZ, (u32)vm_next_addr);
    cb_emit8(EZ80_EX_DE_HL);
    ez_ld_data_hl(ez_vm_ip_off);
    emit8_24(EZ80_JP, (u32)vm_next_addr);

    handlers[BC_JTAB] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    cb_emit8(EZ80_PUSH_HL);
    cb_emit8(EZ80_POP_BC); /* BC = count */
    cb_emit8(EZ80_POP_DE); /* DE = idx */
    cb_emit8(EZ80_OR_A);
    cb_emit8_8(EZ80_ED, EZ80_ED_SBC_HL_DE);
    {
        size_t oob1, oob2;
        cb_emit8(EZ80_JP_Z); oob1 = cb_pc(); cb_emit24(0);
        cb_emit8(EZ80_JP_C); oob2 = cb_pc(); cb_emit24(0);
        ez_ld_hl_ind_data(ez_vm_ip_off);
        cb_emit8(EZ80_ADD_HL_DE);
        cb_emit8(EZ80_ADD_HL_DE);
        cb_emit8(EZ80_ADD_HL_DE);
        cb_emit8_8(EZ80_ED, EZ80_ED_LD_HL_MHL);
        ez_ld_data_hl(ez_vm_ip_off);
        emit8_24(EZ80_JP, (u32)vm_next_addr);
        cb_patch24(oob1, (u32)cb_cur_addr());
        cb_patch24(oob2, (u32)cb_cur_addr());
        ez_ld_hl_ind_data(ez_vm_ip_off);
        cb_emit8(EZ80_ADD_HL_BC);
        cb_emit8(EZ80_ADD_HL_BC);
        cb_emit8(EZ80_ADD_HL_BC);
        ez_ld_data_hl(ez_vm_ip_off);
        emit8_24(EZ80_JP, (u32)vm_next_addr);
    }

    handlers[BC_CALL] = cb_cur_addr();
    emit8_24(EZ80_CALL, (u32)read_u24_addr);
    emit8_24(EZ80_JP, (u32)vm_call_tmp_addr);

    handlers[BC_ICALL] = cb_cur_addr();
    cb_emit8(EZ80_POP_HL);
    emit8_24(EZ80_JP, (u32)vm_call_tmp_addr);

    /* Stack ops */
    handlers[BC_DUP] = cb_cur_addr(); ez_stack_op(OP_DUP, TY_W, TY_W); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_DUP32] = cb_cur_addr(); ez_stack_op(OP_DUP, TY_I32, TY_I32); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_DROP] = cb_cur_addr(); ez_stack_op(OP_DROP, TY_W, TY_W); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_DROP32] = cb_cur_addr(); ez_stack_op(OP_DROP, TY_I32, TY_I32); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_SWAP] = cb_cur_addr(); ez_stack_op(OP_SWAP, TY_W, TY_W); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_SWAP_W32] = cb_cur_addr(); ez_stack_op(OP_SWAP, TY_I32, TY_W); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_SWAP_32W] = cb_cur_addr(); ez_stack_op(OP_SWAP, TY_W, TY_I32); emit8_24(EZ80_JP, (u32)vm_next_addr);
    handlers[BC_SWAP32] = cb_cur_addr(); ez_stack_op(OP_SWAP, TY_I32, TY_I32); emit8_24(EZ80_JP, (u32)vm_next_addr);

    /* ALU, memory, float, i32 ops via native ez_op */
    {
        static const struct { u8 bc; IrOp op; } map[] = {
            {BC_ADD, OP_ADD}, {BC_SUB, OP_SUB}, {BC_MUL, OP_MUL}, {BC_DIV, OP_DIV},
            {BC_MOD, OP_MOD}, {BC_UDIV, OP_UDIV}, {BC_UMOD, OP_UMOD}, {BC_AND, OP_AND},
            {BC_OR, OP_OR}, {BC_XOR, OP_XOR}, {BC_INVERT, OP_INVERT}, {BC_LSHIFT, OP_LSHIFT},
            {BC_ARSHIFT, OP_ARSHIFT}, {BC_RSHIFT, OP_RSHIFT},
            {BC_FADD, OP_FADD}, {BC_FSUB, OP_FSUB}, {BC_FMUL, OP_FMUL}, {BC_FDIV, OP_FDIV},
            {BC_FNEG, OP_FNEG},
            {BC_ADD32, OP_ADD32}, {BC_SUB32, OP_SUB32}, {BC_MUL32, OP_MUL32}, {BC_DIV32, OP_DIV32},
            {BC_MOD32, OP_MOD32}, {BC_UDIV32, OP_UDIV32}, {BC_UMOD32, OP_UMOD32},
            {BC_AND32, OP_AND32}, {BC_OR32, OP_OR32}, {BC_XOR32, OP_XOR32}, {BC_INV32, OP_INV32},
            {BC_SHL32, OP_SHL32}, {BC_ASHR32, OP_ASHR32}, {BC_RSHR32, OP_RSHR32},
            {BC_EQ, OP_EQ}, {BC_NE, OP_NE}, {BC_LT, OP_LT}, {BC_GT, OP_GT},
            {BC_LE, OP_LE}, {BC_GE, OP_GE}, {BC_ULT, OP_ULT}, {BC_UGT, OP_UGT},
            {BC_ULE, OP_ULE}, {BC_UGE, OP_UGE},
            {BC_FEQ, OP_FEQ}, {BC_FNE, OP_FNE}, {BC_FLT, OP_FLT}, {BC_FGT, OP_FGT},
            {BC_EQ32, OP_EQ32}, {BC_NE32, OP_NE32}, {BC_LT32, OP_LT32}, {BC_GT32, OP_GT32},
            {BC_LE32, OP_LE32}, {BC_GE32, OP_GE32}, {BC_ULT32, OP_ULT32}, {BC_UGT32, OP_UGT32},
            {BC_ULE32, OP_ULE32}, {BC_UGE32, OP_UGE32},
            {BC_I8FETCH, OP_I8FETCH}, {BC_U8FETCH, OP_U8FETCH}, {BC_I8STORE, OP_I8STORE},
            {BC_U16FETCH, OP_U16FETCH}, {BC_I16FETCH, OP_I16FETCH}, {BC_STORE16, OP_I16STORE},
            {BC_FETCH, OP_FETCH}, {BC_STORE, OP_STORE},
            {BC_FETCH32, OP_I32FETCH}, {BC_STORE32, OP_I32STORE},
            {BC_BCOPY, OP_BCOPY}, {BC_SEXT, OP_SEXT},
            {BC_W2F, OP_W2F}, {BC_F2W, OP_F2W},
            {BC_W2I32, OP_W2I32}, {BC_U2I32, OP_U2I32}, {BC_I322W, OP_I322W}
        };
        for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
            handlers[map[i].bc] = cb_cur_addr();
            ez_op(map[i].op);
            emit8_24(EZ80_JP, (u32)vm_next_addr);
        }
    }

    for (i = 0; i <= BC_STORE16; i++)
        cb_patch24(vm_tbl_pc + i * 3, (u32)handlers[i]);
    cb_patch24(skip_vm, (u32)cb_cur_addr());
    ez_compact = saved;
}

const Backend be_ez80 = {"ez80",
                         3,
                         3,
                         4,
                         ez_cur_addr,
                         ez_fn_begin,
                         ez_fn_end,
                         ez_call,
                         NULL,
                         ez_call_extern,
                         ez_push_w,
                         ez_push_a,
                         ez_push_f,
                         ez_push_i32,
                         ez_push_data,
                         ez_local_op,
                         ez_local_add_imm,
                         ez_stack_op,
                         ez_op,
                         ez_add_imm,
                         ez_cmp_imm,
                         ez_shift_imm,
                         ez_bit_imm,
                         ez_mem_imm,
                         ez_data_mem,
                         ez_local_mem,
                         NULL,
                         ez_emit_br_fwd,
                         ez_emit_br_back,
                         ez_cmp_br_fwd,
                         ez_cmp_br_back,
                         ez_patch_fwd,
                         ez_emit_jtab,
                         ez_patch_jtab,
                         ez_patch_call,
                         ez_push_fn,
                         ez_patch_fn,
                         ez_icall,
                         ez_stack_data_off,
                         ez_peep_fence};
