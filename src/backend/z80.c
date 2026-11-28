/* z80.c - Z80 Direct Binary Machine-Code Backend for IR4th
 * Calling & Register Convention:
 *   - Data stack: Hardware SP (16-bit slots for W: and A:, 48-bit for F:)
 *   - Locals & params: SP-relative offsets (no IX frame pointer)
 */
#include "codebuf.h"
#include "backend/z80.h"
#include "backend/z80_math48.h"
#include <math.h>

static u16 rt_mul_addr = 0;
static u16 rt_udivmod_addr = 0;
static u16 rt_sdivmod_addr = 0;
static u16 rt_icall_addr = 0;
static u16 rt_math48_addr = 0;
VEC_OF(U16Vec, u16);
VEC_OF(U8Vec, u8);
static U16Vec z80_local_off;
static u16 z80_local_bytes = 0;
static u16 z80_last_push_pc = 0;
static u8 z80_last_push_op = 0;

static u8 z80_type_bytes(StackType t) {
    if (t == TY_F)
        return 6;
    if (t == TY_I32)
        return 4;
    return 2;
}

static void z80_peep_barrier(void) { z80_last_push_op = 0; }

static void z80_push(u8 op) {
    cb_emit8(op);
    if (ir4_get_opt_level()) {
        z80_last_push_op = op;
        z80_last_push_pc = cb_pc();
    }
}

static void z80_pop(u8 op) {
    if (z80_last_push_op != 0 && cb_pc() == z80_last_push_pc) {
        if ((op == Z80_POP_HL && z80_last_push_op == Z80_PUSH_HL) ||
            (op == Z80_POP_DE && z80_last_push_op == Z80_PUSH_DE)) {
            cb_rewind(1);
            z80_last_push_op = 0;
            return;
        }
        if (op == Z80_POP_DE && z80_last_push_op == Z80_PUSH_HL) {
            cb_rewind(1);
            cb_emit8(Z80_LD_D_H);
            cb_emit8(Z80_LD_E_L);
            z80_last_push_op = 0;
            return;
        }
        if (op == Z80_POP_HL && z80_last_push_op == Z80_PUSH_DE) {
            cb_rewind(1);
            cb_emit8(Z80_LD_H_D);
            cb_emit8(Z80_LD_L_E);
            z80_last_push_op = 0;
            return;
        }
    }
    z80_last_push_op = 0;
    cb_emit8(op);
}

/* --- Prefixed & Multi-byte Z80 Helpers --- */
static void z80_sbc_hl_de(void) { cb_emit8_8(0xED, 0x52); }
static void z80_adc_hl_hl(void) { cb_emit8_8(0xED, 0x6A); }

static void z80_rl_b(void) { cb_emit8_8(0xCB, 0x10); }
static void z80_rl_c(void) { cb_emit8_8(0xCB, 0x11); }
static void z80_rr_l(void) { cb_emit8_8(0xCB, 0x1D); }
static void z80_sla_c(void) { cb_emit8_8(0xCB, 0x21); }
static void z80_sra_h(void) { cb_emit8_8(0xCB, 0x2C); }
static void z80_srl_h(void) { cb_emit8_8(0xCB, 0x3C); }

static void z80_neg_hl(void) {
    cb_emit8(Z80_XOR_A);
    cb_emit8(Z80_SUB_L);
    cb_emit8(Z80_LD_L_A);
    cb_emit8(Z80_SBC_A_A);
    cb_emit8(Z80_SUB_H);
    cb_emit8(Z80_LD_H_A);
}

static void z80_neg_de(void) {
    cb_emit8(Z80_XOR_A);
    cb_emit8(Z80_SUB_E);
    cb_emit8(Z80_LD_E_A);
    cb_emit8(Z80_SBC_A_A);
    cb_emit8(Z80_SUB_D);
    cb_emit8(Z80_LD_D_A);
}

/* --- Backend Interface Implementation --- */
static size_t z80_cur_addr(void) { return cb_cur_addr(); }

static U8Vec z80_is_static_slot;
static u16 z80_static_base = 0;

static void z80_fn_begin(const char *name, size_t param_cnt, const LocalVar *locals,
                         size_t total_locals, Bool is_recursive) {
    size_t i;
    u16 off = 0;
    (void)name;
    z80_peep_barrier();

    if (cb_pc() <= 3) {
        rt_mul_addr = 0;
        rt_udivmod_addr = 0;
        rt_sdivmod_addr = 0;
        rt_math48_addr = 0;
    }

    VEC_ENSURE(z80_local_off, total_locals);
    VEC_ENSURE(z80_is_static_slot, total_locals);
    for (i = 0; i < total_locals; i++)
        z80_is_static_slot.data[i] = 0;
    z80_static_base = 0;

    if (!is_recursive) {
        u16 static_bytes = 0;
        z80_local_bytes = 0;
        for (i = param_cnt; i < total_locals; i++) {
            if (locals[i].size > 0) {
                u16 sz = (u16)((locals[i].size + 1) & ~1);
                z80_local_off.data[i] = z80_local_bytes;
                z80_is_static_slot.data[i] = 0;
                z80_local_bytes += sz;
            } else {
                u16 sz = (u16)z80_type_bytes(locals[i].type);
                z80_local_off.data[i] = static_bytes;
                z80_is_static_slot.data[i] = 1;
                static_bytes += sz;
            }
        }
        if (static_bytes > 0) {
            z80_static_base = cb_alloc_data(static_bytes, 1);
            for (i = param_cnt; i < total_locals; i++) {
                if (locals[i].size > 0)
                    continue;
                if (locals[i].type == TY_F || locals[i].type == TY_I32) {
                    u8 k;
                    u8 nbytes = z80_type_bytes(locals[i].type);
                    cb_emit8(Z80_LD_HL_NN);
                    cb_add_data_reloc(cb_pc(), (u16)(z80_static_base + z80_local_off.data[i]));
                    cb_emit16(0x0000);
                    for (k = 0; k < nbytes; k++) {
                        cb_emit8_8(0x36, 0x00); /* LD (HL), 0 */
                        if (k + 1 < nbytes)
                            cb_emit8(Z80_INC_HL);
                    }
                } else {
                    cb_emit8_16(Z80_LD_HL_NN, 0x0000);
                    cb_emit8(0x22);
                    cb_add_data_reloc(cb_pc(), (u16)(z80_static_base + z80_local_off.data[i]));
                    cb_emit16(0x0000); /* LD (nn), HL */
                }
            }
        }
    } else {
        z80_local_bytes = 0;
        for (i = param_cnt; i < total_locals; i++) {
            u16 sz = (locals[i].size > 0) ? (u16)((locals[i].size + 1) & ~1)
                                          : (u16)z80_type_bytes(locals[i].type);
            z80_local_off.data[i] = z80_local_bytes;
            z80_local_bytes += sz;
        }
    }

    /* Parameters stay in place above [local_bytes] + [2-byte return addr] */
    off = (u16)(z80_local_bytes + 2);
    for (i = param_cnt; i > 0; i--) {
        u8 idx = (u8)(i - 1);
        u8 sz = z80_type_bytes(locals[idx].type);
        z80_local_off.data[idx] = off;
        off += sz;
    }

    /* Allocate & zero-initialize non-parameter SP locals */
    if (z80_local_bytes > 0) {
        u16 words = (u16)(z80_local_bytes / 2);
        if (words <= 8) {
            cb_emit8_16(Z80_LD_HL_NN, 0x0000);
            for (off = 0; off < words; off++) {
                cb_emit8(Z80_PUSH_HL);
            }
        } else {
            cb_emit8_16(Z80_LD_HL_NN, (u16)(0 - z80_local_bytes));
            cb_emit8(Z80_ADD_HL_SP);
            cb_emit8(Z80_LD_SP_HL);
            /* Zero scalar SP locals only */
            for (i = param_cnt; i < total_locals; i++) {
                if (locals[i].size == 0 && !z80_is_static_slot.data[i]) {
                    u8 k;
                    u8 sz = z80_type_bytes(locals[i].type);
                    cb_emit8_16(Z80_LD_HL_NN, z80_local_off.data[i]);
                    cb_emit8(Z80_ADD_HL_SP);
                    for (k = 0; k < sz; k++) {
                        cb_emit8_8(0x36, 0x00); /* LD (HL), 0 */
                        if (k + 1 < sz)
                            cb_emit8(Z80_INC_HL);
                    }
                }
            }
        }
    }
}

static void z80_fn_end(const FuncSig *sig, const LocalVar *locals, size_t total_locals) {
    size_t i;
    u16 param_bytes = 0;
    u16 out_bytes = 0;
    u8 out_words;
    (void)locals;
    (void)total_locals;

    for (i = 0; i < sig->in_types.len; i++) {
        param_bytes += z80_type_bytes(sig->in_types.data[i]);
    }
    for (i = 0; i < sig->out_types.len; i++) {
        out_bytes += z80_type_bytes(sig->out_types.data[i]);
    }
    out_words = (u8)(out_bytes / 2);
    if (out_words > 4)
        ir4_die("Z80 return values exceed 8 bytes");

    /* Pop return values from evaluation stack before unwinding locals */
    if (out_words >= 1)
        z80_pop(Z80_POP_DE);
    if (out_words >= 2)
        cb_emit8(Z80_POP_BC);
    if (out_words >= 3) {
        cb_emit8(Z80_EXX);
        cb_emit8(Z80_POP_DE);
        if (out_words >= 4)
            cb_emit8(Z80_POP_BC);
        cb_emit8(Z80_EXX);
    }

    /* Drop non-parameter locals without disturbing DE/BC/DE'/BC' */
    if (z80_local_bytes > 0 && z80_local_bytes <= 8) {
        u16 words = (u16)(z80_local_bytes / 2);
        u16 k;
        for (k = 0; k < words; k++) {
            cb_emit8(Z80_POP_AF);
        }
    } else if (z80_local_bytes > 8) {
        cb_emit8_16(Z80_LD_HL_NN, z80_local_bytes);
        cb_emit8(Z80_ADD_HL_SP);
        cb_emit8(Z80_LD_SP_HL);
    }

    if (param_bytes == 0 && out_words == 0) {
        cb_emit8(Z80_RET);
        return;
    }

    cb_emit8(Z80_POP_HL); /* Return address */
    for (i = 0; i < (u8)(param_bytes / 2); i++) {
        cb_emit8(Z80_POP_AF);
    }

    if (out_words >= 3) {
        cb_emit8(Z80_EXX);
        if (out_words >= 4)
            cb_emit8(Z80_PUSH_BC);
        cb_emit8(Z80_PUSH_DE);
        cb_emit8(Z80_EXX);
    }
    if (out_words >= 2)
        cb_emit8(Z80_PUSH_BC);
    if (out_words >= 1)
        cb_emit8(Z80_PUSH_DE);

    cb_emit8(Z80_JP_HL);
}

static void z80_call(size_t target_addr) { cb_emit8_16(Z80_CALL, (u16)target_addr); }

static int z80_tail_call(size_t target_addr, const FuncSig *sig, const FuncSig *callee,
                         const LocalVar *locals, size_t total_locals) {
    (void)locals;
    if (sig->in_types.len != 0 || callee->in_types.len != 0 ||
        (sig->is_recursive && total_locals > 0))
        return 0;
    z80_peep_barrier();
    cb_emit8_16(Z80_JP, (u16)target_addr);
    return 1;
}

static void z80_call_extern(const FuncSig *sig) {
    u8 used_bc = 0;
    u8 used_de = 0;
    u8 used_hl = 0;
    size_t i;

    if (!sig->has_addr)
        ir4_die("Z80 extern requires a fixed address");
    if (sig->out_types.len > 1)
        ir4_die("Z80 extern supports at most 1 return value");

    z80_peep_barrier();
    for (i = sig->in_types.len; i > 0; i--) {
        const char *r = sig->in_regs.data[i - 1];
        if (strcmp(r, "BC") == 0 || strcmp(r, "C") == 0) {
            cb_emit8(Z80_POP_BC);
            used_bc = 1;
        } else if (strcmp(r, "B") == 0) {
            cb_emit8(Z80_POP_BC);
            cb_emit8(0x41);
            used_bc = 1; /* LD B, C */
        } else if (strcmp(r, "DE") == 0 || strcmp(r, "E") == 0) {
            cb_emit8(Z80_POP_DE);
            used_de = 1;
        } else if (strcmp(r, "D") == 0) {
            cb_emit8(Z80_POP_DE);
            cb_emit8(0x53);
            used_de = 1; /* LD D, E */
        } else if (strcmp(r, "HL") == 0 || strcmp(r, "L") == 0) {
            cb_emit8(Z80_POP_HL);
            used_hl = 1;
        } else if (strcmp(r, "H") == 0) {
            cb_emit8(Z80_POP_HL);
            cb_emit8(0x65);
            used_hl = 1; /* LD H, L */
        } else if (strcmp(r, "A") == 0) {
            if (!used_hl) {
                cb_emit8(Z80_POP_HL);
                cb_emit8(Z80_LD_A_L);
            } else if (!used_de) {
                cb_emit8(Z80_POP_DE);
                cb_emit8(0x7B);
            } /* LD A, E */
            else if (!used_bc) {
                cb_emit8(Z80_POP_BC);
                cb_emit8(0x79);
            } /* LD A, C */
            else
                ir4_die("Z80 extern: no free register pair for A");
        } else {
            ir4_die("unknown Z80 extern register");
        }
    }

    cb_emit8_16(Z80_CALL, sig->addr);

    if (sig->out_types.len == 1) {
        const char *r = sig->out_reg;
        if (strcmp(r, "HL") == 0) {
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "DE") == 0) {
            z80_push(Z80_PUSH_DE);
        } else if (strcmp(r, "BC") == 0) {
            cb_emit8(Z80_PUSH_BC);
        } else if (strcmp(r, "A") == 0) {
            cb_emit8(Z80_LD_L_A);
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "B") == 0) {
            cb_emit8(0x68);
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "C") == 0) {
            cb_emit8(Z80_LD_L_C);
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "D") == 0) {
            cb_emit8(0x6A);
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "E") == 0) {
            cb_emit8(Z80_LD_L_E);
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "H") == 0) {
            cb_emit8(0x6C);
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else if (strcmp(r, "L") == 0) {
            cb_emit8_8(Z80_LD_H_N, 0x00);
            z80_push(Z80_PUSH_HL);
        } else {
            ir4_die("unknown Z80 extern return register");
        }
    }
}

static void z80_push_w(i32 val) {
    cb_emit8_16(Z80_LD_HL_NN, (u16)val);
    z80_push(Z80_PUSH_HL);
}

static void z80_push_a(u32 val) {
    cb_emit8_16(Z80_LD_HL_NN, (u16)val);
    z80_push(Z80_PUSH_HL);
}

static void z80_push_f(double val) {
    u8 bytes[6];
    if (val == 0.0) {
        memset(bytes, 0, sizeof(bytes));
    } else {
        int e, i;
        int sign = (val < 0.0);
        double m = frexp(sign ? -val : val, &e);
        m += 0.5 / 1099511627776.0; /* round to 40-bit mantissa (2^-41) */
        if (m >= 1.0) {
            m *= 0.5;
            e++;
        }
        if (e + 128 <= 0) {
            memset(bytes, 0, sizeof(bytes));
        } else if (e + 128 > 255) {
            ir4_die("Z80 float literal overflow");
        } else {
            for (i = 0; i < 5; i++) {
                int b;
                m *= 256.0;
                b = (int)m;
                if (b > 255)
                    b = 255;
                m -= (double)b;
                bytes[5 - i] = (u8)b;
            }
            bytes[5] = (u8)((bytes[5] & 0x7F) | (sign ? 0x80 : 0x00));
            bytes[0] = (u8)(e + 128);
        }
    }
    cb_emit8_16(Z80_LD_HL_NN, (u16)(bytes[4] | ((u16)bytes[5] << 8)));
    cb_emit8(Z80_PUSH_HL);
    cb_emit8_16(Z80_LD_HL_NN, (u16)(bytes[2] | ((u16)bytes[3] << 8)));
    cb_emit8(Z80_PUSH_HL);
    cb_emit8_16(Z80_LD_HL_NN, (u16)(bytes[0] | ((u16)bytes[1] << 8)));
    z80_push(Z80_PUSH_HL);
}

static void z80_push_i32(i32 val) {
    u32 u = (u32)val;
    cb_emit8_16(Z80_LD_HL_NN, (u16)((u >> 16) & 0xFFFFU));
    cb_emit8(Z80_PUSH_HL);
    cb_emit8_16(Z80_LD_HL_NN, (u16)(u & 0xFFFFU));
    z80_push(Z80_PUSH_HL);
}

static void z80_push_data(size_t offset) {
    cb_emit8(Z80_LD_HL_NN);
    cb_add_data_reloc(cb_pc(), offset);
    cb_emit16(0x0000);
    z80_push(Z80_PUSH_HL);
}

static void z80_local_get(size_t slot, StackType type, u16 stack_bytes) {
    u16 off;
    if (z80_is_static_slot.data[slot]) {
        u16 addr_off = (u16)(z80_static_base + z80_local_off.data[slot]);
        if (type == TY_F || type == TY_I32) {
            u8 i;
            u8 w = (type == TY_F) ? 3 : 2;
            cb_emit8(Z80_LD_HL_NN);
            cb_add_data_reloc(cb_pc(), (u16)(addr_off + (u16)(w * 2 - 1)));
            cb_emit16(0x0000);
            for (i = 0; i < w; i++) {
                cb_emit8(Z80_LD_D_MHL);
                cb_emit8(Z80_DEC_HL);
                cb_emit8(Z80_LD_E_MHL);
                if (i + 1 < w)
                    cb_emit8(Z80_DEC_HL);
                z80_push(Z80_PUSH_DE);
            }
        } else {
            cb_emit8(0x2A);
            cb_add_data_reloc(cb_pc(), addr_off);
            cb_emit16(0x0000); /* LD HL, (nn) */
            z80_push(Z80_PUSH_HL);
        }
        return;
    }
    off = (u16)(z80_local_off.data[slot] + stack_bytes);
    if (type == TY_F || type == TY_I32) {
        u8 i;
        u8 w = (type == TY_F) ? 3 : 2;
        cb_emit8_16(Z80_LD_HL_NN, (u16)(off + (u16)(w * 2 - 1)));
        cb_emit8(Z80_ADD_HL_SP);
        for (i = 0; i < w; i++) {
            cb_emit8(Z80_LD_D_MHL);
            cb_emit8(Z80_DEC_HL);
            cb_emit8(Z80_LD_E_MHL);
            if (i + 1 < w)
                cb_emit8(Z80_DEC_HL);
            z80_push(Z80_PUSH_DE);
        }
    } else {
        cb_emit8_16(Z80_LD_HL_NN, off);
        cb_emit8(Z80_ADD_HL_SP);
        cb_emit8(Z80_LD_E_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_D_MHL);
        z80_push(Z80_PUSH_DE);
    }
}

static void z80_local_set(size_t slot, StackType type, u16 stack_bytes) {
    u16 off;
    if (z80_is_static_slot.data[slot]) {
        u16 addr_off = (u16)(z80_static_base + z80_local_off.data[slot]);
        if (type == TY_F || type == TY_I32) {
            u8 i;
            u8 w = (type == TY_F) ? 3 : 2;
            cb_emit8(Z80_LD_HL_NN);
            cb_add_data_reloc(cb_pc(), addr_off);
            cb_emit16(0x0000);
            for (i = 0; i < w; i++) {
                cb_emit8(Z80_POP_DE);
                cb_emit8(Z80_LD_MHL_E);
                cb_emit8(Z80_INC_HL);
                cb_emit8(Z80_LD_MHL_D);
                if (i + 1 < w)
                    cb_emit8(Z80_INC_HL);
            }
        } else {
            z80_pop(Z80_POP_HL);
            cb_emit8(0x22);
            cb_add_data_reloc(cb_pc(), addr_off);
            cb_emit16(0x0000); /* LD (nn), HL */
        }
        return;
    }
    off = (u16)(z80_local_off.data[slot] + stack_bytes);
    if (type == TY_F || type == TY_I32) {
        u8 i;
        u8 w = (type == TY_F) ? 3 : 2;
        cb_emit8_16(Z80_LD_HL_NN, (u16)(off + (u16)(w * 2)));
        cb_emit8(Z80_ADD_HL_SP);
        for (i = 0; i < w; i++) {
            cb_emit8(Z80_POP_DE);
            cb_emit8(Z80_LD_MHL_E);
            cb_emit8(Z80_INC_HL);
            cb_emit8(Z80_LD_MHL_D);
            if (i + 1 < w)
                cb_emit8(Z80_INC_HL);
        }
    } else {
        z80_pop(Z80_POP_DE);
        cb_emit8_16(Z80_LD_HL_NN, off);
        cb_emit8(Z80_ADD_HL_SP);
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
    }
}

static void z80_pop_f_ac(void) {
    z80_pop(Z80_POP_HL);
    cb_emit8(Z80_POP_DE);
    cb_emit8(Z80_POP_BC);
}

static void z80_push_f_ac(void) {
    cb_emit8(Z80_PUSH_BC);
    cb_emit8(Z80_PUSH_DE);
    z80_push(Z80_PUSH_HL);
}

static void z80_local_addr(size_t slot, u16 stack_bytes) {
    u16 off;
    if (z80_is_static_slot.data[slot]) {
        z80_push_data((u16)(z80_static_base + z80_local_off.data[slot]));
        return;
    }
    off = (u16)(z80_local_off.data[slot] + stack_bytes);
    cb_emit8_16(Z80_LD_HL_NN, off);
    cb_emit8(Z80_ADD_HL_SP);
    z80_push(Z80_PUSH_HL);
}

static void z80_local_add(size_t slot, u16 stack_bytes) {
    u16 off;
    if (z80_is_static_slot.data[slot]) {
        u16 addr_off = (u16)(z80_static_base + z80_local_off.data[slot]);
        z80_pop(Z80_POP_DE);
        cb_emit8(0x2A);
        cb_add_data_reloc(cb_pc(), addr_off);
        cb_emit16(0x0000); /* LD HL, (nn) */
        cb_emit8(Z80_ADD_HL_DE);
        cb_emit8(0x22);
        cb_add_data_reloc(cb_pc(), addr_off);
        cb_emit16(0x0000); /* LD (nn), HL */
        return;
    }
    off = (u16)(z80_local_off.data[slot] + stack_bytes);
    z80_pop(Z80_POP_DE);
    cb_emit8_16(Z80_LD_HL_NN, off);
    cb_emit8(Z80_ADD_HL_SP);
    cb_emit8(0x7E); /* LD A, (HL) */
    cb_emit8(0x83); /* ADD A, E */
    cb_emit8(0x77); /* LD (HL), A */
    cb_emit8(Z80_INC_HL);
    cb_emit8(0x7E); /* LD A, (HL) */
    cb_emit8(0x8A); /* ADC A, D */
    cb_emit8(0x77); /* LD (HL), A */
}

static int z80_local_add_imm(size_t slot, StackType type, size_t stack_bytes, i32 imm) {
    i16 d = (i16)imm;
    (void)type;
    if (d == 0)
        return 1;
    if (z80_is_static_slot.data[slot]) {
        u16 addr_off = (u16)(z80_static_base + z80_local_off.data[slot]);
        cb_emit8(0x2A);
        cb_add_data_reloc(cb_pc(), addr_off);
        cb_emit16(0x0000); /* LD HL, (nn) */
        if (d == 1) {
            cb_emit8(Z80_INC_HL);
        } else if (d == -1) {
            cb_emit8(Z80_DEC_HL);
        } else if (d == 2) {
            cb_emit8(Z80_INC_HL);
            cb_emit8(Z80_INC_HL);
        } else if (d == -2) {
            cb_emit8(Z80_DEC_HL);
            cb_emit8(Z80_DEC_HL);
        } else {
            cb_emit8_16(0x11, (u16)d); /* LD DE, d */
            cb_emit8(Z80_ADD_HL_DE);
        }
        cb_emit8(0x22);
        cb_add_data_reloc(cb_pc(), addr_off);
        cb_emit16(0x0000); /* LD (nn), HL */
        return 1;
    }
    if (d > 0 && d <= 255) {
        u16 off = (u16)(z80_local_off.data[slot] + stack_bytes);
        cb_emit8_16(Z80_LD_HL_NN, off);
        cb_emit8(Z80_ADD_HL_SP);
        cb_emit8(0x7E);        /* LD A, (HL) */
        cb_emit8_8(0xC6, (u8)d); /* ADD A, d */
        cb_emit8(0x77);        /* LD (HL), A */
        cb_emit8(Z80_INC_HL);
        cb_emit8_8(0x30, 1);   /* JR NC, +1 */
        cb_emit8(0x34);        /* INC (HL) */
        return 1;
    }
    return 0;
}

static void z80_local_op(LocalOp op, size_t slot, StackType type, size_t stack_bytes) {
    if (op == LOCAL_GET)
        z80_local_get(slot, type, (u16)stack_bytes);
    else if (op == LOCAL_ADDR)
        z80_local_addr(slot, (u16)stack_bytes);
    else if (op == LOCAL_ADD)
        z80_local_add(slot, (u16)stack_bytes);
    else
        z80_local_set(slot, type, (u16)stack_bytes);
}

static void z80_pop_any(StackType t) {
    if (t == TY_F) {
        z80_pop_f_ac();
    } else if (t == TY_I32) {
        z80_pop(Z80_POP_DE);
        cb_emit8(Z80_POP_BC);
    } else {
        z80_pop(Z80_POP_HL);
    }
}

static void z80_push_any(StackType t) {
    if (t == TY_F) {
        z80_push_f_ac();
    } else if (t == TY_I32) {
        cb_emit8(Z80_PUSH_BC);
        z80_push(Z80_PUSH_DE);
    } else {
        z80_push(Z80_PUSH_HL);
    }
}

static void z80_stack_op(StackOp op, StackType t1, StackType t2) {
    u8 is4_1 = (t1 == TY_F || t1 == TY_I32);
    u8 is4_2 = (t2 == TY_F || t2 == TY_I32);
    switch (op) {
    case OP_DUP:
        if (is4_1) {
            z80_pop_any(t1);
            z80_push_any(t1);
            z80_push_any(t1);
        } else {
            z80_pop(Z80_POP_HL);
            cb_emit8(Z80_PUSH_HL);
            z80_push(Z80_PUSH_HL);
        }
        break;
    case OP_DROP:
        z80_pop(Z80_POP_HL);
        if (t1 == TY_F) {
            cb_emit8(Z80_POP_HL);
            cb_emit8(Z80_POP_HL);
        } else if (t1 == TY_I32) {
            cb_emit8(Z80_POP_HL);
        }
        break;
    case OP_SWAP:
        if (!is4_1 && !is4_2) {
            z80_pop(Z80_POP_HL);
            cb_emit8(Z80_EX_SP_HL);
            z80_push(Z80_PUSH_HL);
        } else {
            z80_pop_any(t1);
            cb_emit8(Z80_EXX);
            z80_pop_any(t2);
            cb_emit8(Z80_EXX);
            z80_push_any(t1);
            cb_emit8(Z80_EXX);
            z80_push_any(t2);
            cb_emit8(Z80_EXX);
        }
        break;
    default:
        ir4_die("unimplemented Z80 stack op");
    }
}

static void z80_emit_mul(void) {
    if (rt_mul_addr == 0) {
        cb_emit8_8(Z80_JR, 19);
        rt_mul_addr = cb_cur_addr();
        /* HL = HL * DE (16-bit) */
        cb_emit8(Z80_LD_B_H);
        cb_emit8(Z80_LD_C_L);
        cb_emit8_16(Z80_LD_HL_NN, 0);
        cb_emit8_8(Z80_LD_A_N, 16);
        /* mul_loop: */
        cb_emit8(Z80_ADD_HL_HL);
        z80_rl_c();
        z80_rl_b();
        cb_emit8_8(Z80_JR_NC, 1);
        cb_emit8(Z80_ADD_HL_DE);
        cb_emit8(Z80_DEC_A);
        cb_emit8_8(Z80_JR_NZ, (u8)-11);
        cb_emit8(Z80_RET);
    }
    z80_call(rt_mul_addr);
}

static void z80_emit_udivmod_helper(void) {
    rt_udivmod_addr = cb_cur_addr();
    /* Input: HL = dividend, DE = divisor. Output: HL = quot, DE = rem */
    cb_emit8(Z80_LD_B_H);
    cb_emit8(Z80_LD_C_L);
    cb_emit8_16(Z80_LD_HL_NN, 0);
    cb_emit8_8(Z80_LD_A_N, 16);
    /* udiv_loop: */
    z80_sla_c();
    z80_rl_b();
    z80_adc_hl_hl();
    cb_emit8(Z80_OR_A);
    z80_sbc_hl_de();
    cb_emit8(Z80_INC_C);
    cb_emit8_8(Z80_JR_NC, 2);
    cb_emit8(Z80_ADD_HL_DE);
    cb_emit8(Z80_DEC_C);
    cb_emit8(Z80_DEC_A);
    cb_emit8_8(Z80_JR_NZ, (u8)-17);
    cb_emit8(Z80_EX_DE_HL); /* DE = rem */
    cb_emit8(Z80_LD_H_B);
    cb_emit8(Z80_LD_L_C); /* HL = quot */
    cb_emit8(Z80_RET);
}

static void z80_emit_udivmod(void) {
    if (rt_udivmod_addr == 0) {
        cb_emit8_8(Z80_JR, 28);
        z80_emit_udivmod_helper();
    }
    z80_call(rt_udivmod_addr);
}

static void z80_emit_sdivmod(void) {
    if (rt_sdivmod_addr == 0) {
        u16 patch_over;
        cb_emit8(Z80_JP);
        patch_over = cb_pc();
        cb_emit16(0x0000);
        if (rt_udivmod_addr == 0) {
            z80_emit_udivmod_helper();
        }
        rt_sdivmod_addr = cb_cur_addr();
        /* Input: HL = a, DE = b. Output: HL = a / b, DE = a % b (trunc toward 0) */
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_XOR_D); /* bit 7 = quot sign */
        cb_emit8(Z80_PUSH_AF);
        cb_emit8(Z80_LD_A_H); /* bit 7 = rem sign */
        cb_emit8(Z80_PUSH_AF);
        /* abs(HL) */
        cb_emit8(Z80_OR_A);
        cb_emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_hl();
        /* abs(DE) */
        cb_emit8(Z80_LD_A_D);
        cb_emit8(Z80_OR_A);
        cb_emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_de();
        z80_call(rt_udivmod_addr);
        /* sign(rem) on DE */
        cb_emit8(Z80_POP_AF);
        cb_emit8(Z80_OR_A);
        cb_emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_de();
        /* sign(quot) on HL */
        cb_emit8(Z80_POP_AF);
        cb_emit8(Z80_OR_A);
        cb_emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 9));
        z80_neg_hl();
        cb_emit8(Z80_RET);
        cb_patch16(patch_over, cb_cur_addr());
    }
    z80_call(rt_sdivmod_addr);
}

static void z80_ensure_rt_blob(u16 *addr, const u8 *code, u16 code_len, const u16 *relocs,
                               u16 reloc_cnt) {
    u16 skip_patch;
    u16 base_off;
    u16 i;

    if (*addr != 0)
        return;

    cb_emit8(Z80_JP);
    skip_patch = cb_pc();
    cb_emit16(0x0000);

    *addr = cb_cur_addr();
    base_off = cb_pc();
    for (i = 0; i < code_len; i++) {
        cb_emit8(code[i]);
    }
    for (i = 0; i < reloc_cnt; i++) {
        u16 rel_off = relocs[i];
        u16 rel_val = (u16)(code[rel_off] | ((u16)code[rel_off + 1] << 8));
        cb_patch16((u16)(base_off + rel_off), (u16)(*addr + rel_val));
    }

    cb_patch16(skip_patch, cb_cur_addr());
}

static void z80_ensure_math48(void) {
    z80_ensure_rt_blob(&rt_math48_addr, be_z80_math48, (u16)sizeof(be_z80_math48),
                       be_z80_math48_relocs, (u16)BE_Z80_MATH48_RELOC_CNT);
}

static void z80_call_math48(u16 entry_offset) {
    z80_ensure_math48();
    z80_call((u16)(rt_math48_addr + entry_offset));
}

static void z80_int_alu(IrOp op) {
    z80_pop(Z80_POP_DE); /* rhs */
    cb_emit8(Z80_POP_HL);   /* lhs */

    switch (op) {
    case OP_ADD:
    case OP_AADD:
        cb_emit8(Z80_ADD_HL_DE);
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit8(Z80_OR_A);
        z80_sbc_hl_de();
        break;
    case OP_MUL:
        z80_emit_mul();
        break;
    case OP_DIV:
        z80_emit_sdivmod();
        break;
    case OP_MOD:
        z80_emit_sdivmod();
        cb_emit8(Z80_EX_DE_HL);
        break;
    case OP_UDIV:
        z80_emit_udivmod();
        break;
    case OP_UMOD:
        z80_emit_udivmod();
        cb_emit8(Z80_EX_DE_HL);
        break;
    case OP_AND:
        cb_emit8(Z80_LD_A_L);
        cb_emit8(Z80_AND_E);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_AND_D);
        cb_emit8(Z80_LD_H_A);
        break;
    case OP_OR:
        cb_emit8(Z80_LD_A_L);
        cb_emit8(Z80_OR_E);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_D);
        cb_emit8(Z80_LD_H_A);
        break;
    case OP_XOR:
        cb_emit8(Z80_LD_A_L);
        cb_emit8(Z80_XOR_E);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_XOR_D);
        cb_emit8(Z80_LD_H_A);
        break;
    case OP_LSHIFT:
        cb_emit8(Z80_LD_B_E);
        cb_emit8(Z80_INC_B);
        cb_emit8_8(Z80_JR, 1);
        cb_emit8(Z80_ADD_HL_HL);
        cb_emit8_8(Z80_DJNZ, (u8)-3);
        break;
    case OP_RSHIFT:
        cb_emit8(Z80_LD_B_E);
        cb_emit8(Z80_INC_B);
        cb_emit8_8(Z80_JR, 4);
        z80_srl_h();
        z80_rr_l();
        cb_emit8_8(Z80_DJNZ, (u8)-6);
        break;
    case OP_ARSHIFT:
        cb_emit8(Z80_LD_B_E);
        cb_emit8(Z80_INC_B);
        cb_emit8_8(Z80_JR, 4);
        z80_sra_h();
        z80_rr_l();
        cb_emit8_8(Z80_DJNZ, (u8)-6);
        break;
    default:
        ir4_die("unimplemented Z80 ALU op");
    }
    z80_push(Z80_PUSH_HL);
}

static void z80_int_cmp(IrOp op) {
    if (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT) {
        z80_pop(Z80_POP_HL); /* rhs */
        cb_emit8(Z80_POP_DE);   /* lhs */
    } else {
        z80_pop(Z80_POP_DE); /* rhs */
        cb_emit8(Z80_POP_HL);   /* lhs */
    }

    cb_emit8(Z80_OR_A);
    z80_sbc_hl_de(); /* HL = a - b; sets Z, C, S, P/V flags */

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        cb_emit8_8(Z80_JR_Z, 3);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_NE:
    case OP_ANE:
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        cb_emit8_8(Z80_JR_NZ, 3);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_ULT:
    case OP_UGT:
    case OP_ALT:
    case OP_AGT:
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        cb_emit8_8(Z80_JR_C, 3);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_UGE:
    case OP_ULE:
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        cb_emit8_8(Z80_JR_NC, 3);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        break;
    case OP_LT:
    case OP_GT:
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        cb_emit8_16(Z80_JP_PE, (u16)(cb_cur_addr() + 8)); /* ovf */
        cb_emit8_16(Z80_JP_M, (u16)(cb_cur_addr() + 11)); /* done */
        cb_emit8_8(Z80_JR, 3);                            /* false */
        /* ovf: */
        cb_emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 6)); /* done */
        /* false: */
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        /* done: */
        break;
    case OP_GE:
    case OP_LE:
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        cb_emit8_16(Z80_JP_PE, (u16)(cb_cur_addr() + 8)); /* ovf */
        cb_emit8_16(Z80_JP_M, (u16)(cb_cur_addr() + 11)); /* done */
        cb_emit8_8(Z80_JR, 3);                            /* true */
        /* ovf: */
        cb_emit8_16(Z80_JP_P, (u16)(cb_cur_addr() + 6)); /* done */
        /* true: */
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        /* done: */
        break;
    default:
        ir4_die("unimplemented Z80 comparison op");
    }
    z80_push(Z80_PUSH_HL);
}

static int z80_data_mem(IrOp op, size_t data_offset) {
    switch (op) {
    case OP_U8FETCH:
        cb_emit8(0x3A);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000); /* LD A, (nn) */
        cb_emit8(0x6F);    /* LD L, A */
        cb_emit8_8(Z80_LD_H_N, 0x00);
        z80_push(Z80_PUSH_HL);
        return 1;
    case OP_I8FETCH:
        cb_emit8(0x3A);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000); /* LD A, (nn) */
        cb_emit8(0x6F);    /* LD L, A */
        cb_emit8(Z80_RLA);
        cb_emit8(Z80_SBC_A_A);
        cb_emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        return 1;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        cb_emit8(0x2A);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000); /* LD HL, (nn) */
        z80_push(Z80_PUSH_HL);
        return 1;
    case OP_I8STORE:
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_LD_A_L);
        cb_emit8(0x32);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000); /* LD (nn), A */
        return 1;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
        z80_pop(Z80_POP_HL);
        cb_emit8(0x22);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000); /* LD (nn), HL */
        return 1;
    case OP_I32FETCH:
        cb_emit8(0x2A);
        cb_add_data_reloc(cb_pc(), (u16)(data_offset + 2));
        cb_emit16(0x0000);
        cb_emit8(Z80_PUSH_HL);
        cb_emit8(0x2A);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000);
        z80_push(Z80_PUSH_HL);
        return 1;
    case OP_I32STORE:
        z80_pop(Z80_POP_HL);
        cb_emit8(0x22);
        cb_add_data_reloc(cb_pc(), data_offset);
        cb_emit16(0x0000);
        cb_emit8(Z80_POP_HL);
        cb_emit8(0x22);
        cb_add_data_reloc(cb_pc(), (u16)(data_offset + 2));
        cb_emit16(0x0000);
        return 1;
    default:
        return 0;
    }
}

static int z80_mem_imm(IrOp op, i32 off) {
    int swapped = (op >= OP_SI8STORE && op <= OP_SFSTORE);
    int is_i32 = (op == OP_I32FETCH || op == OP_I32STORE);
    if ((op < OP_I8FETCH || op > OP_FSTORE) && !swapped && !is_i32)
        return 0;

    if (swapped) {
        if (op == OP_SFSTORE) {
            z80_pop(Z80_POP_DE);
            cb_emit8(Z80_POP_BC);
            cb_emit8(Z80_EXX);
            cb_emit8(Z80_POP_DE);
            cb_emit8(Z80_EXX);
            cb_emit8(Z80_POP_HL);
        } else {
            z80_pop(Z80_POP_DE); /* val */
            cb_emit8(Z80_POP_HL);   /* addr */
        }
        if (off >= 1 && off <= 3) {
            i32 k;
            for (k = 0; k < off; k++)
                cb_emit8(Z80_INC_HL);
        } else if (off != 0) {
            if (op == OP_SFSTORE) {
                cb_emit8(Z80_PUSH_BC);
                cb_emit8_16(0x01, (u16)(off & 0xFFFF)); /* LD BC, nn */
                cb_emit8(0x09);                         /* ADD HL, BC */
                cb_emit8(Z80_POP_BC);
            } else {
                cb_emit8_16(0x01, (u16)(off & 0xFFFF)); /* LD BC, nn */
                cb_emit8(0x09);                         /* ADD HL, BC */
            }
        }
        if (op == OP_SI8STORE) {
            cb_emit8(Z80_LD_MHL_E);
        } else if (op == OP_SFSTORE) {
            cb_emit8(Z80_LD_MHL_E);
            cb_emit8(Z80_INC_HL);
            cb_emit8(Z80_LD_MHL_D);
            cb_emit8(Z80_INC_HL);
            cb_emit8(0x71);
            cb_emit8(Z80_INC_HL); /* LD (HL), C */
            cb_emit8(0x70);
            cb_emit8(Z80_INC_HL); /* LD (HL), B */
            cb_emit8(Z80_EXX);
            cb_emit8(Z80_PUSH_DE);
            cb_emit8(Z80_EXX);
            cb_emit8(Z80_POP_DE);
            cb_emit8(Z80_LD_MHL_E);
            cb_emit8(Z80_INC_HL);
            cb_emit8(Z80_LD_MHL_D);
        } else {
            cb_emit8(Z80_LD_MHL_E);
            cb_emit8(Z80_INC_HL);
            cb_emit8(Z80_LD_MHL_D);
        }
        return 1;
    }

    z80_pop(Z80_POP_HL);
    if (off >= 1 && off <= 3) {
        i32 k;
        for (k = 0; k < off; k++)
            cb_emit8(Z80_INC_HL);
    } else if (off != 0) {
        cb_emit8_16(Z80_LD_DE_NN, (u16)(off & 0xFFFF));
        cb_emit8(Z80_ADD_HL_DE);
    }

    switch (op) {
    case OP_U8FETCH:
        cb_emit8(Z80_LD_L_MHL);
        cb_emit8_8(Z80_LD_H_N, 0x00);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_I8FETCH:
        cb_emit8(Z80_LD_L_MHL);
        cb_emit8(Z80_LD_A_L);
        cb_emit8(Z80_RLA);
        cb_emit8(Z80_SBC_A_A);
        cb_emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_I8STORE:
        cb_emit8(Z80_POP_DE); /* val */
        cb_emit8(Z80_LD_MHL_E);
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        cb_emit8(Z80_LD_E_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_D_MHL);
        cb_emit8(Z80_EX_DE_HL);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
        cb_emit8(Z80_POP_DE); /* val */
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
        break;
    case OP_FFETCH:
        cb_emit8(Z80_LD_E_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_D_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_C_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_B_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_A_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_H_MHL);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_PUSH_HL);
        cb_emit8(Z80_PUSH_BC);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_FSTORE:
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
        break;
    case OP_I32FETCH:
        cb_emit8(Z80_LD_E_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_D_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_C_MHL);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_B_MHL);
        cb_emit8(Z80_PUSH_BC);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_I32STORE:
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_LD_MHL_E);
        cb_emit8(Z80_INC_HL);
        cb_emit8(Z80_LD_MHL_D);
        break;
    default:
        break;
    }
    return 1;
}

static void z80_op(IrOp op) {
    switch (op) {
    case OP_ADD:
    case OP_AADD:
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
    case OP_MUL:
    case OP_DIV:
    case OP_MOD:
    case OP_UDIV:
    case OP_UMOD:
    case OP_AND:
    case OP_OR:
    case OP_XOR:
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        z80_int_alu(op);
        break;
    case OP_INVERT:
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_LD_A_L);
        cb_emit8(Z80_CPL);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_CPL);
        cb_emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_FNEG:
        z80_pop_f_ac();
        z80_call_math48(RT_FP_NEG);
        z80_push_f_ac();
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV: {
        u16 entry = (op == OP_FADD)   ? RT_FP_ADD
                    : (op == OP_FSUB) ? RT_FP_SUB
                    : (op == OP_FMUL) ? RT_FP_MUL
                                      : RT_FP_DIV;
        cb_emit8(Z80_EXX);
        z80_pop_f_ac();
        cb_emit8(Z80_EXX); /* rhs -> AC' */
        z80_pop_f_ac(); /* lhs -> AC */
        z80_call_math48(entry);
        z80_push_f_ac();
        break;
    }
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
        z80_int_cmp(op);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT:
        if (op == OP_FGT) {
            z80_pop_f_ac(); /* rhs -> AC */
            cb_emit8(Z80_EXX);
            z80_pop_f_ac();
            cb_emit8(Z80_EXX); /* lhs -> AC' */
        } else {
            cb_emit8(Z80_EXX);
            z80_pop_f_ac();
            cb_emit8(Z80_EXX); /* rhs -> AC' */
            z80_pop_f_ac(); /* lhs -> AC */
        }
        z80_call_math48(RT_FP_CMP);
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        cb_emit8_8((op == OP_FEQ) ? Z80_JR_Z : (op == OP_FNE) ? Z80_JR_NZ : Z80_JR_C, 3);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_U8FETCH:
    case OP_I8FETCH:
    case OP_I8STORE:
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
    case OP_FFETCH:
    case OP_FSTORE:
    case OP_SI8STORE:
    case OP_SI16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SFSTORE:
    case OP_I32FETCH:
    case OP_I32STORE:
        z80_mem_imm(op, 0);
        break;
    case OP_BCOPY:
        z80_pop(Z80_POP_BC); /* len */
        cb_emit8(Z80_POP_HL);   /* src */
        cb_emit8(Z80_POP_DE);   /* dst */
        cb_emit8(0x78);         /* LD A, B */
        cb_emit8(0xB1);         /* OR C */
        cb_emit8_8(Z80_JR_Z, 0x02);
        cb_emit8(0xED);
        cb_emit8(0xB0); /* LDIR */
        break;
    case OP_SEXT:
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_LD_A_L);
        cb_emit8(Z80_RLA);
        cb_emit8(Z80_SBC_A_A);
        cb_emit8(Z80_LD_H_A);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_W2A:
    case OP_A2W:
        /* 16-bit word and 16-bit address are identical on Z80: zero-cost no-op */
        break;
    case OP_W2F:
        z80_pop(Z80_POP_HL);
        z80_call_math48(RT_FP_ITOF);
        z80_push_f_ac();
        break;
    case OP_F2W:
        z80_pop_f_ac();
        z80_call_math48(RT_FP_FTOI);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_W2I32:
        z80_pop(Z80_POP_DE);
        cb_emit8(Z80_LD_A_D);
        cb_emit8(Z80_RLA);
        cb_emit8(Z80_SBC_A_A);
        cb_emit8(Z80_LD_H_A);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_PUSH_HL);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_U2I32:
        z80_pop(Z80_POP_DE);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        cb_emit8(Z80_PUSH_HL);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_I322W:
        z80_pop(Z80_POP_DE);
        cb_emit8(Z80_POP_HL);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_INV32:
        z80_pop(Z80_POP_DE);
        cb_emit8(Z80_POP_HL);
        cb_emit8(0x7B);
        cb_emit8(0x2F);
        cb_emit8(0x5F); /* LD A,E; CPL; LD E,A */
        cb_emit8(Z80_LD_A_D);
        cb_emit8(0x2F);
        cb_emit8(Z80_LD_D_A);
        cb_emit8(Z80_LD_A_L);
        cb_emit8(0x2F);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(0x2F);
        cb_emit8(Z80_LD_H_A);
        cb_emit8(Z80_PUSH_HL);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_ADD32:
        z80_pop(Z80_POP_BC);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_POP_HL);
        cb_emit8(0x09); /* ADD HL, BC */
        cb_emit8(0xE3); /* EX (SP), HL */
        cb_emit8(0xED);
        cb_emit8(0x5A); /* ADC HL, DE */
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_PUSH_HL);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_SUB32:
        z80_pop(Z80_POP_BC);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_POP_HL);
        cb_emit8(Z80_OR_A);
        cb_emit8(0xED);
        cb_emit8(0x42); /* SBC HL, BC */
        cb_emit8(0xE3); /* EX (SP), HL */
        cb_emit8(0xED);
        cb_emit8(0x52); /* SBC HL, DE */
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_PUSH_HL);
        z80_push(Z80_PUSH_DE);
        break;
    case OP_AND32:
    case OP_OR32:
    case OP_XOR32: {
        u8 alu_e = (op == OP_AND32) ? 0xA3 : (op == OP_OR32) ? 0xB3 : 0xAB;
        u8 alu_d = (op == OP_AND32) ? 0xA2 : (op == OP_OR32) ? 0xB2 : 0xAA;
        z80_pop(Z80_POP_BC); /* rhs_lo */
        cb_emit8(Z80_POP_DE);   /* rhs_hi */
        cb_emit8(Z80_POP_HL);   /* lhs_lo */
        cb_emit8(Z80_LD_A_L);
        cb_emit8(0xA1 + (alu_e - 0xA3));
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(0xA0 + (alu_d - 0xA2));
        cb_emit8(Z80_LD_H_A);
        cb_emit8(0xE3); /* EX (SP), HL -> (SP)=res_lo, HL=lhs_hi */
        cb_emit8(Z80_LD_A_L);
        cb_emit8(alu_e);
        cb_emit8(Z80_LD_L_A);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(alu_d);
        cb_emit8(Z80_LD_H_A);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_PUSH_HL);
        z80_push(Z80_PUSH_DE);
        break;
    }
    case OP_EQ32:
    case OP_NE32:
        z80_pop(Z80_POP_BC);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_POP_HL);
        cb_emit8(Z80_OR_A);
        cb_emit8(0xED);
        cb_emit8(0x42); /* SBC HL, BC */
        cb_emit8(0xE3); /* EX (SP), HL */
        cb_emit8(0xED);
        cb_emit8(0x52); /* SBC HL, DE */
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_L);
        cb_emit8(Z80_OR_D);
        cb_emit8(Z80_OR_E);
        cb_emit8_8(op == OP_EQ32 ? Z80_JR_Z : Z80_JR_NZ, 0x05);
        cb_emit8_16(Z80_LD_HL_NN, 0x0000);
        cb_emit8_8(Z80_JR, 0x03);
        cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        z80_push(Z80_PUSH_HL);
        break;
    case OP_ULT32:
    case OP_UGE32:
    case OP_UGT32:
    case OP_ULE32:
    case OP_LT32:
    case OP_GE32:
    case OP_GT32:
    case OP_LE32: {
        u8 swap_ops = (op == OP_UGT32 || op == OP_ULE32 || op == OP_GT32 || op == OP_LE32);
        u8 invert = (op == OP_UGE32 || op == OP_ULE32 || op == OP_GE32 || op == OP_LE32);
        u8 is_signed = (op >= OP_LT32 && op <= OP_GE32);
        if (swap_ops) {
            /* Swap top two 32-bit values */
            z80_pop_any(TY_I32);
            cb_emit8(Z80_EXX);
            z80_pop_any(TY_I32);
            cb_emit8(Z80_EXX);
            z80_push_any(TY_I32);
            cb_emit8(Z80_EXX);
            z80_push_any(TY_I32);
            cb_emit8(Z80_EXX);
        }
        z80_pop(Z80_POP_BC);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_POP_HL);
        cb_emit8(Z80_OR_A);
        cb_emit8(0xED);
        cb_emit8(0x42); /* SBC HL, BC */
        cb_emit8(0xE3); /* EX (SP), HL */
        cb_emit8(0xED);
        cb_emit8(0x52); /* SBC HL, DE */
        cb_emit8(Z80_POP_DE);
        if (is_signed) {
            /* Signed lhs < rhs iff (S ^ P/V) */
            u16 a0 = cb_cur_addr();
            u8 cc = invert ? 0xF2 : 0xFA;   /* JP P : JP M */
            cb_emit8_16(0xEA, (u16)(a0 + 11)); /* JP PE, ovf */
            cb_emit8_16(cc, (u16)(a0 + 14));   /* JP M/P, set_true */
            cb_emit8_16(Z80_LD_HL_NN, 0x0000); /* set_false */
            cb_emit8_8(Z80_JR, 0x06);          /* JR done */
            cb_emit8_16(cc, (u16)(a0 + 6));    /* ovf: JP M/P, set_false */
            cb_emit8_16(Z80_LD_HL_NN, 0xFFFF); /* set_true */
        } else {
            cb_emit8_8(invert ? Z80_JR_NC : Z80_JR_C, 0x05);
            cb_emit8_16(Z80_LD_HL_NN, 0x0000);
            cb_emit8_8(Z80_JR, 0x03);
            cb_emit8_16(Z80_LD_HL_NN, 0xFFFF);
        }
        z80_push(Z80_PUSH_HL);
        break;
    }
    case OP_MUL32:
    case OP_DIV32:
    case OP_MOD32:
    case OP_UDIV32:
    case OP_UMOD32:
    case OP_SHL32:
    case OP_RSHR32:
    case OP_ASHR32:
        ir4_die("Z80 i32 mul/div/shift handled by lib/i32.ir4");
    }
}

static int z80_local_mem(size_t slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)stack_bytes;
    if (!z80_is_static_slot.data[slot])
        return 0;
    return z80_data_mem(op, (u16)(z80_static_base + z80_local_off.data[slot] + off));
}

static size_t z80_emit_br_fwd(BranchCond cond) {
    u16 patch_pos;
    if (cond == BR_JZ) {
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_L);
        cb_emit8(Z80_JP_Z);
    } else {
        cb_emit8(Z80_JP);
    }
    patch_pos = cb_pc();
    cb_emit16(0x0000);
    return patch_pos;
}

static void z80_emit_br_back(BranchCond cond, size_t target_addr) {
    if (cond == BR_JZ) {
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_L);
        cb_emit8_16(Z80_JP_Z, target_addr);
    } else {
        cb_emit8_16(Z80_JP, target_addr);
    }
}

static int z80_cmp_br_emit(IrOp op, int has_imm, i32 imm, int is_fwd, size_t target_addr,
                           size_t *patch_offset) {
    u8 swap_ops;
    u8 jcc = 0;

    if (op < OP_EQ || op > OP_AGT)
        return 0;

    if (has_imm && imm == 0 && (op == OP_EQ || op == OP_NE || op == OP_AEQ || op == OP_ANE)) {
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_LD_A_H);
        cb_emit8(Z80_OR_L);
        jcc = (op == OP_EQ || op == OP_AEQ) ? 0xC2 : Z80_JP_Z; /* JP NZ : JP Z */
        if (is_fwd) {
            cb_emit8(jcc);
            *patch_offset = cb_pc();
            cb_emit16(0x0000);
        } else {
            cb_emit8_16(jcc, (u16)target_addr);
        }
        return 1;
    }

    swap_ops = (op == OP_GT || op == OP_LE || op == OP_UGT || op == OP_ULE || op == OP_AGT);
    if (has_imm) {
        if (swap_ops) {
            z80_pop(Z80_POP_DE);
            cb_emit8_16(Z80_LD_HL_NN, (u16)imm);
        } else {
            z80_pop(Z80_POP_HL);
            cb_emit8_16(Z80_LD_DE_NN, (u16)imm);
        }
    } else {
        if (swap_ops) {
            z80_pop(Z80_POP_HL);
            cb_emit8(Z80_POP_DE);
        } else {
            z80_pop(Z80_POP_DE);
            cb_emit8(Z80_POP_HL);
        }
    }

    cb_emit8(Z80_OR_A);
    z80_sbc_hl_de();

    switch (op) {
    case OP_EQ:
    case OP_AEQ:
        jcc = 0xC2; /* JP NZ */
        break;
    case OP_NE:
    case OP_ANE:
        jcc = Z80_JP_Z;
        break;
    case OP_ULT:
    case OP_UGT:
    case OP_ALT:
    case OP_AGT:
        jcc = 0xD2; /* JP NC */
        break;
    case OP_UGE:
    case OP_ULE:
        jcc = 0xDA; /* JP C */
        break;
    case OP_LT:
    case OP_GT:
    case OP_GE:
    case OP_LE: {
        u16 a0 = cb_cur_addr();
        u8 is_lt = (op == OP_LT || op == OP_GT);
        if (is_fwd) {
            u8 skip_noov = is_lt ? Z80_JP_M : Z80_JP_P;
            u8 skip_ov = is_lt ? Z80_JP_P : Z80_JP_M;
            cb_emit8_16(Z80_JP_PE, (u16)(a0 + 8));
            cb_emit8_16(skip_noov, (u16)(a0 + 14));
            cb_emit8_8(Z80_JR, 3);
            cb_emit8_16(skip_ov, (u16)(a0 + 14));
            cb_emit8(Z80_JP);
            *patch_offset = cb_pc();
            cb_emit16(0x0000);
        } else {
            u8 j_noov = is_lt ? Z80_JP_P : Z80_JP_M;
            u8 j_ov = is_lt ? Z80_JP_M : Z80_JP_P;
            cb_emit8_16(Z80_JP_PE, (u16)(a0 + 8));
            cb_emit8_16(j_noov, (u16)target_addr);
            cb_emit8_8(Z80_JR, 3);
            cb_emit8_16(j_ov, (u16)target_addr);
        }
        return 1;
    }
    default:
        return 0;
    }

    if (is_fwd) {
        cb_emit8(jcc);
        *patch_offset = cb_pc();
        cb_emit16(0x0000);
    } else {
        cb_emit8_16(jcc, (u16)target_addr);
    }
    return 1;
}

static int z80_cmp_br_fwd(IrOp op, int has_imm, i32 imm, size_t *patch_offset) {
    return z80_cmp_br_emit(op, has_imm, imm, 1, 0, patch_offset);
}

static int z80_cmp_br_back(IrOp op, int has_imm, i32 imm, size_t target_addr) {
    return z80_cmp_br_emit(op, has_imm, imm, 0, target_addr, NULL);
}

static size_t z80_emit_jtab(size_t count) {
    size_t tbl_off, k;
    u16 tbl_addr;
    z80_pop(Z80_POP_HL);
    cb_emit8(Z80_ADD_HL_HL);
    tbl_addr = (u16)(cb_cur_addr() + 9);
    cb_emit8_16(Z80_LD_DE_NN, tbl_addr);
    cb_emit8(Z80_ADD_HL_DE);
    cb_emit8(0x5E); /* LD E, (HL) */
    cb_emit8(Z80_INC_HL);
    cb_emit8(0x56); /* LD D, (HL) */
    cb_emit8(Z80_EX_DE_HL);
    cb_emit8(Z80_JP_HL);
    tbl_off = cb_pc();
    for (k = 0; k < count; k++)
        cb_emit16(0x0000);
    return tbl_off;
}

static void z80_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    (void)count;
    cb_patch16(patch_offset + idx * 2, (u16)target_addr);
}

static void z80_patch_fwd(size_t patch_offset) {
    z80_peep_barrier();
    cb_patch16(patch_offset, cb_cur_addr());
}

static int z80_add_imm(int is_addr, i32 imm) {
    i16 d = (i16)imm;
    (void)is_addr;
    z80_peep_barrier();
    if (d == 0)
        return 1;
    cb_emit8(Z80_POP_HL);
    if (d >= -3 && d <= 3) {
        int k;
        u8 op = (d > 0) ? 0x23 : 0x2B; /* INC HL / DEC HL */
        int n = (d > 0) ? (int)d : (int)(-d);
        for (k = 0; k < n; k++)
            cb_emit8(op);
    } else {
        cb_emit8_16(Z80_LD_DE_NN, (u16)d);
        cb_emit8(Z80_ADD_HL_DE);
    }
    cb_emit8(Z80_PUSH_HL);
    return 1;
}

static int z80_shift_imm(IrOp op, u8 count) {
    u8 k;
    z80_peep_barrier();
    if (count == 0)
        return 1;
    if (op == OP_SHL32 && count == 1) {
        z80_pop(Z80_POP_HL);
        cb_emit8(Z80_POP_DE);
        cb_emit8(Z80_ADD_HL_HL);
        cb_emit8(0xCB);
        cb_emit8(0x13); /* RL E */
        cb_emit8(0xCB);
        cb_emit8(0x12); /* RL D */
        cb_emit8(Z80_PUSH_DE);
        z80_push(Z80_PUSH_HL);
        return 1;
    }
    if (op != OP_LSHIFT && op != OP_RSHIFT && op != OP_ARSHIFT)
        return 0;
    if (count >= 8) {
        if ((u8)(count - 8) > 4)
            return 0;
        z80_pop(Z80_POP_HL);
        if (op == OP_LSHIFT) {
            cb_emit8(0x65);         /* LD H, L */
            cb_emit8_8(0x2E, 0x00); /* LD L, 0 */
        } else if (op == OP_RSHIFT) {
            cb_emit8(0x6C);         /* LD L, H */
            cb_emit8_8(0x26, 0x00); /* LD H, 0 */
        } else {
            cb_emit8(0x6C); /* LD L, H */
            cb_emit8(Z80_LD_A_H);
            cb_emit8(Z80_RLA);
            cb_emit8(Z80_SBC_A_A);
            cb_emit8(Z80_LD_H_A);
        }
        count = (u8)(count - 8);
    } else {
        if (count > 4)
            return 0;
        z80_pop(Z80_POP_HL);
    }
    for (k = 0; k < count; k++) {
        if (op == OP_LSHIFT) {
            cb_emit8(Z80_ADD_HL_HL);
        } else if (op == OP_RSHIFT) {
            z80_srl_h();
            z80_rr_l();
        } else {
            z80_sra_h();
            z80_rr_l();
        }
    }
    z80_push(Z80_PUSH_HL);
    return 1;
}

static int z80_bit_imm(IrOp op, i32 imm) {
    u16 v = (u16)imm;
    u8 lo = (u8)(v & 0xFF);
    u8 hi = (u8)(v >> 8);
    if (op != OP_AND && op != OP_OR && op != OP_XOR)
        return 0;
    z80_pop(Z80_POP_HL);
    if (op == OP_AND) {
        if (lo == 0)
            cb_emit8_8(0x2E, 0x00); /* LD L, 0 */
        else if (lo != 0xFF) {
            cb_emit8(Z80_LD_A_L);
            cb_emit8_8(0xE6, lo); /* AND lo */
            cb_emit8(Z80_LD_L_A);
        }
        if (hi == 0)
            cb_emit8_8(Z80_LD_H_N, 0x00); /* LD H, 0 */
        else if (hi != 0xFF) {
            cb_emit8(Z80_LD_A_H);
            cb_emit8_8(0xE6, hi); /* AND hi */
            cb_emit8(Z80_LD_H_A);
        }
    } else if (op == OP_OR) {
        if (lo == 0xFF)
            cb_emit8_8(0x2E, 0xFF); /* LD L, 0xFF */
        else if (lo != 0) {
            cb_emit8(Z80_LD_A_L);
            cb_emit8_8(0xF6, lo); /* OR lo */
            cb_emit8(Z80_LD_L_A);
        }
        if (hi == 0xFF)
            cb_emit8_8(Z80_LD_H_N, 0xFF); /* LD H, 0xFF */
        else if (hi != 0) {
            cb_emit8(Z80_LD_A_H);
            cb_emit8_8(0xF6, hi); /* OR hi */
            cb_emit8(Z80_LD_H_A);
        }
    } else {
        if (lo != 0) {
            cb_emit8(Z80_LD_A_L);
            cb_emit8_8(0xEE, lo); /* XOR lo */
            cb_emit8(Z80_LD_L_A);
        }
        if (hi != 0) {
            cb_emit8(Z80_LD_A_H);
            cb_emit8_8(0xEE, hi); /* XOR hi */
            cb_emit8(Z80_LD_H_A);
        }
    }
    z80_push(Z80_PUSH_HL);
    return 1;
}

static void z80_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    cb_patch16(call_pc + 1, (u16)target_addr);
}

static size_t z80_push_fn(size_t target_addr) {
    size_t pc = cb_pc();
    cb_emit8_16(Z80_LD_HL_NN, (u16)target_addr);
    z80_push(Z80_PUSH_HL);
    return pc;
}

static void z80_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    (void)patch_addr;
    cb_patch16(patch_pc + 1, (u16)target_addr);
}

static void z80_icall(void) {
    z80_pop(Z80_POP_HL);
    if (rt_icall_addr == 0) {
        cb_emit8_8(Z80_JR, 1);
        rt_icall_addr = cb_cur_addr();
        cb_emit8(0xE9); /* JP (HL) */
    }
    z80_call(rt_icall_addr);
}

void be_z80_emit_zero_bss(void) {
    size_t bss_off;
    size_t bss_sz;

    (void)cb_alloc_data(0, 2);
    bss_off = cb_init_data_size();
    bss_sz = cb_data_size() - bss_off;
    if (bss_sz == 0) {
        return;
    }

    cb_emit8(Z80_LD_HL_NN);
    cb_add_data_reloc(cb_pc(), bss_off);
    cb_emit16(0x0000);
    cb_emit8_8(0x36, 0x00); /* LD (HL), 0 */
    if (bss_sz > 1) {
        cb_emit8(Z80_LD_DE_NN);
        cb_add_data_reloc(cb_pc(), bss_off + 1);
        cb_emit16(0x0000);
        cb_emit8(Z80_LD_BC_NN);
        cb_emit16((u16)(bss_sz - 1));
        cb_emit8(0xED);
        cb_emit8(0xB0); /* LDIR */
    }
}

const Backend be_z80 = {"z80",
                        2,
                        2,
                        6,
                        z80_cur_addr,
                        z80_fn_begin,
                        z80_fn_end,
                        z80_call,
                        z80_tail_call,
                        z80_call_extern,
                        z80_push_w,
                        z80_push_a,
                        z80_push_f,
                        z80_push_i32,
                        z80_push_data,
                        z80_local_op,
                        z80_local_add_imm,
                        z80_stack_op,
                        z80_op,
                        z80_add_imm,
                        NULL,
                        z80_shift_imm,
                        z80_bit_imm,
                        z80_mem_imm,
                        z80_data_mem,
                        z80_local_mem,
                        NULL,
                        z80_emit_br_fwd,
                        z80_emit_br_back,
                        z80_cmp_br_fwd,
                        z80_cmp_br_back,
                        z80_patch_fwd,
                        z80_emit_jtab,
                        z80_patch_jtab,
                        z80_patch_call,
                        z80_push_fn,
                        z80_patch_fn,
                        z80_icall,
                        NULL,
                        z80_peep_barrier};
