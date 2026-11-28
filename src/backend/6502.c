/* 6502.c - 6502 Direct Binary Machine-Code Backend for IR4th
 * Calling & Register Convention:
 *   - Data stack: 256-byte BSS buffer in normal RAM indexed by X (wraps in 0..255 via DEX/DEX)
 *   - Locals: 256-byte BSS software frame stack indexed by 1-byte BSS lfp (grows downward)
 *   - Scratch pointers: 4 Zero Page bytes at zp_scratch (default $FB..$FE; override via --zp)
 */
#include "backend/bc.h"
#include "codebuf.h"
#include "backend/6502.h"
#include "backend/6502_wozfp.h"
#include <math.h>

static u8 zp_scratch = 0xFB;
static int zp_explicit = 0;
static u8 stack_zp_base = 0;
static u8 stack_zp_init_x = 0;
static int stack_zp_explicit = 0;
static u16 stack_data_off = 0;
static u16 locals_data_off = 0;
static u16 lfp_data_off = 0;
static int data_inited = 0;
static u16 bfp_data_off = 0;
static int big_inited = 0;
static u16 m6502_ram_top = 0xE000;

void be_6502_set_ram_top(u16 top) { m6502_ram_top = top; }
static u16 mul16_addr = 0;
static u16 udivmod16_addr = 0;
static u16 sdivmod16_addr = 0;
static u16 icall_addr = 0;
static u16 wozfp_addr = 0;
static int m6502_compact = 2; /* 0 = native, 1 = all compact, 2 = auto */
static u16 vm_enter_addr = 0;
static u16 m6502_call_target_bank = 0;
static u16 m6502_bank_call_addr = 0;
static u16 m6502_kernal_call_addr = 0;
static u8 m6502_phys_bank_offset = 32;

void be_6502_set_compact(int compact) { m6502_compact = compact; }
int be_6502_get_compact(void) { return m6502_compact; }
void be_6502_set_call_bank(u16 bank) { m6502_call_target_bank = bank; }
void be_6502_set_trampoline(u16 bank_call_addr, u16 kernal_call_addr) {
    m6502_bank_call_addr = bank_call_addr;
    m6502_kernal_call_addr = kernal_call_addr;
}

static void ensure_6502_vm(void);
static void m6502_bc_local_op(LocalOp op, int slot, StackType type);
static void m6502_pop2(void);
static void m6502_push2(void);

void be_6502_set_zp(u8 zp) {
    zp_scratch = zp;
    zp_explicit = 1;
}

void be_6502_set_default_zp(u8 zp) {
    if (!zp_explicit) {
        zp_scratch = zp;
    }
}

void be_6502_set_stack_zp(u8 base, u8 init_x) {
    stack_zp_base = base;
    stack_zp_init_x = init_x;
    stack_zp_explicit = 1;
}

void be_6502_set_default_stack_zp(u8 base, u8 init_x) {
    if (!stack_zp_explicit) {
        stack_zp_base = base;
        stack_zp_init_x = init_x;
    }
}

u8 be_6502_stack_zp_base(void) { return stack_zp_base; }

u8 be_6502_stack_zp_init_x(void) { return stack_zp_init_x; }

static void ensure_data_alloc(void) {
    if (!data_inited) {
        if (stack_zp_base == 0) {
            stack_data_off = cb_alloc_data(256, 1);
        }
        locals_data_off = cb_alloc_data(256, 1);
        lfp_data_off = cb_alloc_data(1, 1);
        data_inited = 1;
    }
}

static void ensure_big_alloc(void) {
    if (!big_inited) {
        bfp_data_off = cb_alloc_data(2, 1);
        big_inited = 1;
    }
}

size_t be_6502_stack_data_off(void) {
    if (stack_zp_base != 0) {
        return (u16)stack_zp_base;
    }
    ensure_data_alloc();
    return stack_data_off;
}

static void emit_stack_op(u8 opcode, u8 disp) {
    if (stack_zp_base != 0) {
        /* Every abs,X stack opcode in be_6502 maps to zp,X via (opcode - 0x08). */
        cb_emit8_8((u8)(opcode - 0x08), (u8)(stack_zp_base + disp));
        return;
    }
    ensure_data_alloc();
    cb_emit8(opcode);
    cb_add_data_reloc(cb_pc(), (u16)(stack_data_off + disp));
    cb_emit16(0x0000);
}

static void emit_local_op(u8 opcode, u8 byte_off) {
    ensure_data_alloc();
    cb_emit8(opcode);
    cb_add_data_reloc(cb_pc(), (u16)(locals_data_off + byte_off));
    cb_emit16(0x0000);
}

/* --- 6502 Stack & Local Frame Helpers --- */
static void m6502_lda_local(u8 o) { emit_local_op(0xB9, o); } /* LDA abs,Y */
static void m6502_sta_local(u8 o) { emit_local_op(0x99, o); } /* STA abs,Y */
static void m6502_ldy_lfp(void) {
    ensure_data_alloc();
    cb_emit8(0xAC);
    cb_add_data_reloc(cb_pc(), lfp_data_off);
    cb_emit16(0x0000);
} /* LDY lfp */

static void m6502_lda_stk(u8 d) { emit_stack_op(M6502_LDA_ABSX, d); }
static void m6502_sta_stk(u8 d) { emit_stack_op(0x9D, d); }
static void m6502_ldy_stk(u8 d) { emit_stack_op(0xBC, d); }
static void m6502_adc_stk(u8 d) { emit_stack_op(0x7D, d); }
static void m6502_sbc_stk(u8 d) { emit_stack_op(0xFD, d); }
static void m6502_and_stk(u8 d) { emit_stack_op(0x3D, d); }
static void m6502_ora_stk(u8 d) { emit_stack_op(0x1D, d); }
static void m6502_eor_stk(u8 d) { emit_stack_op(0x5D, d); }
static void m6502_asl_stk(u8 d) { emit_stack_op(0x1E, d); }
static void m6502_lsr_stk(u8 d) { emit_stack_op(0x5E, d); }
static void m6502_ror_stk(u8 d) { emit_stack_op(0x7E, d); }

/* --- Backend Interface Implementation --- */
static size_t m6502_cur_addr(void) { return cb_cur_addr(); }

VEC_OF(U16Vec, u16);
VEC_OF(U8Vec, u8);
static U16Vec m6502_local_off;
static U8Vec m6502_local_is_big;
static u16 m6502_frame_bytes = 0;
static u16 m6502_big_bytes = 0;
static u8 m6502_static_frame = 0;
static u16 m6502_static_base = 0;

static void m6502_emit_local_store(u16 off) {
    if (m6502_static_frame) {
        cb_emit8(M6502_STA_ABS);
        cb_add_data_reloc(cb_pc(), (u16)(m6502_static_base + off));
        cb_emit16(0x0000);
    } else {
        m6502_sta_local((u8)off);
    }
}

static void m6502_fn_begin(const char *name, int param_cnt, const LocalVar *locals,
                           int total_locals, Bool is_recursive) {
    int i;
    (void)name;

    VEC_ENSURE(m6502_local_off, (size_t)total_locals);
    VEC_ENSURE(m6502_local_is_big, (size_t)total_locals);
    m6502_frame_bytes = 0;
    m6502_big_bytes = 0;
    for (i = 0; i < total_locals; i++) {
        u16 sz = (locals[i].size > 0)
                     ? locals[i].size
                     : (u16)((locals[i].type == TY_F || locals[i].type == TY_I32) ? 4 : 2);
        if (is_recursive && locals[i].size > 0) {
            m6502_local_is_big.data[i] = 1;
            m6502_local_off.data[i] = m6502_big_bytes;
            m6502_big_bytes += sz;
        } else {
            m6502_local_is_big.data[i] = 0;
            if (is_recursive && (u32)m6502_frame_bytes + sz > 255) {
                ir4_die("6502 local frame exceeds 255 bytes");
            }
            m6502_local_off.data[i] = m6502_frame_bytes;
            m6502_frame_bytes += sz;
        }
    }

    m6502_static_frame = (u8)(!is_recursive);
    m6502_static_base = 0;

    if (m6502_compact) {
        ensure_6502_vm();
        cb_emit8_16(M6502_JSR, vm_enter_addr);
        if (m6502_static_frame) {
            if (m6502_frame_bytes > 0)
                m6502_static_base = cb_alloc_data(m6502_frame_bytes, 1);
        } else {
            if (m6502_frame_bytes > 0) {
                ensure_data_alloc();
                cb_emit8_8(BC_ENTER, (u8)m6502_frame_bytes);
            }
            if (m6502_big_bytes > 0) {
                ensure_big_alloc();
                cb_emit8_16(BC_ENTER_BIG, m6502_big_bytes);
            }
        }
        for (i = param_cnt; i > 0; i--) {
            size_t p = i - 1;
            m6502_bc_local_op(LOCAL_SET, p, locals[p].type);
        }
        for (i = param_cnt; i < total_locals; i++) {
            if (locals[i].size > 0)
                continue;
            if (locals[i].type == TY_F || locals[i].type == TY_I32) {
                cb_emit8(BC_PUSH_0_32);
            } else {
                cb_emit8(BC_PUSH_IMM7_BASE);
            }
            m6502_bc_local_op(LOCAL_SET, i, locals[i].type);
        }
        return;
    }

    if (m6502_static_frame) {
        if (m6502_frame_bytes > 0) {
            m6502_static_base = cb_alloc_data(m6502_frame_bytes, 1);
        }
    } else {
        if (m6502_frame_bytes > 0) {
            ensure_data_alloc();
            /* Save caller's lfp and allocate m6502_frame_bytes downward */
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), lfp_data_off);
            cb_emit16(0x0000); /* LDA lfp */
            cb_emit8(M6502_PHA);
            cb_emit8(M6502_SEC);
            cb_emit8_8(M6502_SBC_IMM, (u8)m6502_frame_bytes);
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), lfp_data_off);
            cb_emit16(0x0000);   /* STA lfp */
            cb_emit8(M6502_TAY); /* TAY (Y = new lfp) */
        }
        if (m6502_big_bytes > 0) {
            ensure_big_alloc();
            /* Lazy-init bfp = m6502_ram_top if bfp+1 == 0 */
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
            cb_emit16(0x0000);
            cb_emit8_8(M6502_BNE, 10);
            cb_emit8_8(M6502_LDA_IMM, (u8)(m6502_ram_top & 0xFF));
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), bfp_data_off);
            cb_emit16(0x0000);
            cb_emit8_8(M6502_LDA_IMM, (u8)(m6502_ram_top >> 8));
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
            cb_emit16(0x0000);
            /* Save caller's bfp and allocate m6502_big_bytes downward */
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), bfp_data_off);
            cb_emit16(0x0000);
            cb_emit8(M6502_PHA);
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
            cb_emit16(0x0000);
            cb_emit8(M6502_PHA);
            cb_emit8(M6502_SEC);
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), bfp_data_off);
            cb_emit16(0x0000);
            cb_emit8_8(M6502_SBC_IMM, (u8)(m6502_big_bytes & 0xFF));
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), bfp_data_off);
            cb_emit16(0x0000);
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
            cb_emit16(0x0000);
            cb_emit8_8(M6502_SBC_IMM, (u8)(m6502_big_bytes >> 8));
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
            cb_emit16(0x0000);
        }
    }

    /* Pop incoming parameters from data stack into locals */
    for (i = param_cnt; i > 0; i--) {
        u8 idx = (u8)(i - 1);
        u16 off = m6502_local_off.data[idx];
        u8 sz = (locals[idx].type == TY_F || locals[idx].type == TY_I32) ? 4 : 2;
        u8 b;
        for (b = 0; b < sz; b++) {
            m6502_lda_stk(b);
            m6502_emit_local_store((u16)(off + b));
        }
        for (b = 0; b < sz; b++)
            cb_emit8(M6502_INX);
    }

    /* Zero-initialize scalar scratch locals after '|' */
    if (total_locals > param_cnt) {
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        for (i = param_cnt; i < total_locals; i++) {
            u16 off = m6502_local_off.data[i];
            u8 sz = (locals[i].type == TY_F || locals[i].type == TY_I32) ? 4 : 2;
            u8 b;
            if (locals[i].size > 0)
                continue;
            for (b = 0; b < sz; b++) {
                m6502_emit_local_store((u16)(off + b));
            }
        }
    }
}

static void m6502_fn_end(const FnSig *sig, const LocalVar *locals, int total_locals) {
    (void)sig;
    (void)locals;
    (void)total_locals;

    if (m6502_compact) {
        if (!m6502_static_frame) {
            if (m6502_big_bytes > 0)
                cb_emit8(BC_LEAVE_BIG);
            if (m6502_frame_bytes > 0)
                cb_emit8(BC_LEAVE);
        }
        cb_emit8(BC_RET);
        return;
    }

    if (!m6502_static_frame) {
        if (m6502_big_bytes > 0) {
            ensure_big_alloc();
            cb_emit8(M6502_PLA);
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
            cb_emit16(0x0000);
            cb_emit8(M6502_PLA);
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), bfp_data_off);
            cb_emit16(0x0000);
        }
        if (m6502_frame_bytes > 0) {
            ensure_data_alloc();
            cb_emit8(M6502_PLA);
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), lfp_data_off);
            cb_emit16(0x0000); /* STA lfp */
        }
    }

    cb_emit8(M6502_RTS);
}

static void m6502_call(size_t target_addr) {
    u16 tbank = m6502_call_target_bank;
    m6502_call_target_bank = 0;
    if (cb_is_banked() && tbank != 0 && tbank != cb_cur_bank()) {
        cb_emit8_16(M6502_JSR, m6502_bank_call_addr);
        cb_emit8((u8)(m6502_phys_bank_offset + tbank));
        cb_emit16((u16)target_addr);
        return;
    }
    cb_emit8_16(m6502_compact ? BC_CALL : M6502_JSR, (u16)target_addr);
}

static int m6502_tail_call(size_t target_addr, const FnSig *sig, const FnSig *callee,
                           const LocalVar *locals, int total_locals) {
    u16 tbank = m6502_call_target_bank;
    m6502_call_target_bank = 0;
    (void)sig;
    (void)callee;
    (void)locals;
    (void)total_locals;
    if (m6502_compact)
        return 0;
    if (!m6502_static_frame && (m6502_big_bytes > 0 || m6502_frame_bytes > 0))
        return 0;
    if (cb_is_banked() && tbank != 0 && tbank != cb_cur_bank())
        return 0;
    be_6502_peep_fence();
    cb_emit8_16(0x4C, (u16)target_addr); /* JMP abs */
    return 1;
}

static void m6502_call_extern(const FnSig *sig) {
    size_t i;
    u8 has_a = 0, has_x = 0, has_y = 0;
    if (m6502_compact) {
        u16 jmp_patch;
        u16 stub_addr;
        cb_emit8(BC_JMP);
        jmp_patch = cb_pc();
        cb_emit16(0x0000);
        stub_addr = cb_cur_addr();
        m6502_compact = 0;
        m6502_call_extern(sig);
        m6502_compact = 1;
        cb_emit8(M6502_RTS);
        cb_patch16(jmp_patch, cb_cur_addr());
        cb_emit8_16(BC_CALL, stub_addr);
        return;
    }
    if (!FN_FLAG(sig, FN_FL_HAS_ADDR))
        ir4_die("6502 extern requires a fixed address");
    if (sig->out_cnt > 1)
        ir4_die("6502 extern supports at most 1 return value");

    for (i = sig->in_cnt; i > 0; i--) {
        const char *r = (sig->ext && sig->ext->in_regs) ? sig->ext->in_regs[i - 1] : "";
        m6502_lda_stk(0);
        if (strcmp(r, "A") == 0) {
            cb_emit8_8(M6502_STA_ZP, zp_scratch);
            has_a = 1;
        } else if (strcmp(r, "X") == 0) {
            cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
            has_x = 1;
        } else if (strcmp(r, "Y") == 0) {
            cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 2));
            has_y = 1;
        } else {
            ir4_die("6502 extern only supports A, X, and Y registers");
        }
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
    }

    cb_emit8_8(M6502_STX_ZP, (u8)(zp_scratch + 3));
    if (has_a)
        cb_emit8_8(M6502_LDA_ZP, zp_scratch);
    if (has_x) {
        cb_emit8_8(M6502_LDX_ZP, (u8)(zp_scratch + 1));
    }
    if (has_y) {
        cb_emit8_8(M6502_LDY_ZP, (u8)(zp_scratch + 2));
    }

    if (cb_is_banked() && sig->addr >= 0xC000 && m6502_kernal_call_addr != 0) {
        cb_emit8_16(M6502_JSR, m6502_kernal_call_addr);
        cb_emit16(sig->addr);
    } else {
        cb_emit8_16(M6502_JSR, sig->addr);
    }

    if (sig->out_cnt == 1) {
        const char *r = sig->ext ? sig->ext->out_reg : "";
        if (strcmp(r, "X") == 0) {
            cb_emit8(M6502_TXA);
        } else if (strcmp(r, "Y") == 0) {
            cb_emit8(M6502_TYA);
        } else if (strcmp(r, "A") != 0) {
            ir4_die("6502 extern return only supports A, X, and Y registers");
        }
        cb_emit8_8(M6502_LDX_ZP, (u8)(zp_scratch + 3));
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        m6502_sta_stk(0);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(1);
    } else {
        cb_emit8_8(M6502_LDX_ZP, (u8)(zp_scratch + 3));
    }
}

static void m6502_push_w(i32 val) {
    u16 v = (u16)val;
    if (m6502_compact) {
        bc_emit_push_w(val, 2);
        return;
    }
    m6502_push2();
    cb_emit8_8(M6502_LDA_IMM, (u8)(v & 0xFF));
    m6502_sta_stk(0);
    cb_emit8_8(M6502_LDA_IMM, (u8)((v >> 8) & 0xFF));
    m6502_sta_stk(1);
}

static void m6502_push_a(u32 val) { m6502_push_w((i32)val); }

static void encode_wozfp(double val, u8 out[4]) {
    int exp2;
    double m;
    i32 mant;
    if (val == 0.0) {
        out[0] = 0;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        return;
    }
    m = frexp(val, &exp2); /* val = m * 2^exp2, 0.5 <= |m| < 1.0 */
    mant = (i32)(m * 8388608.0 + (m >= 0.0 ? 0.5 : -0.5));
    exp2 -= 1;
    /* Normalize 24-bit 2's complement mantissa so bits 23 and 22 differ */
    if (mant != 0) {
        while (((mant >> 23) & 1) == ((mant >> 22) & 1)) {
            mant <<= 1;
            exp2 -= 1;
        }
    }
    if (exp2 < -128 || exp2 > 127) {
        ir4_die("6502 wozfp float literal out of range");
    }
    out[0] = (u8)(exp2 + 0x80);
    out[1] = (u8)((mant >> 16) & 0xFF);
    out[2] = (u8)((mant >> 8) & 0xFF);
    out[3] = (u8)(mant & 0xFF);
}

static void m6502_push_f(double val) {
    u8 bytes[4];
    u8 b;
    encode_wozfp(val, bytes);
    if (m6502_compact) {
        if (val == 0.0) {
            cb_emit8(BC_PUSH_0_32);
            return;
        }
        cb_emit8(BC_PUSH_I32);
        for (b = 0; b < 4; b++)
            cb_emit8(bytes[b]);
        return;
    }
    for (b = 0; b < 4; b++)
        cb_emit8(M6502_DEX);
    for (b = 0; b < 4; b++) {
        cb_emit8_8(M6502_LDA_IMM, bytes[b]);
        m6502_sta_stk(b);
    }
}

static void m6502_push_i32(i32 val) {
    u8 b;
    u32 u = (u32)val;
    if (m6502_compact) {
        bc_emit_push_i32(val);
        return;
    }
    for (b = 0; b < 4; b++)
        cb_emit8(M6502_DEX);
    for (b = 0; b < 4; b++) {
        cb_emit8_8(M6502_LDA_IMM, (u8)((u >> (b * 8)) & 0xFF));
        m6502_sta_stk(b);
    }
}

static void m6502_push_data(size_t offset) {
    u16 lo_pos, hi_pos;
    if (m6502_compact) {
        cb_emit8(BC_PUSH_W);
        bc_emit_data_reloc(offset, 2);
        return;
    }
    m6502_push2();
    cb_emit8(M6502_LDA_IMM);
    lo_pos = cb_pc();
    cb_emit8(0x00); /* LDA #<addr */
    m6502_sta_stk(0);
    cb_emit8(M6502_LDA_IMM);
    hi_pos = cb_pc();
    cb_emit8(0x00); /* LDA #>addr */
    m6502_sta_stk(1);
    cb_add_data_reloc_split(lo_pos, hi_pos, offset);
}

static void m6502_local_get(size_t slot, StackType type, u16 stack_bytes) {
    u16 off = m6502_local_off.data[slot];
    u8 sz = (type == TY_F || type == TY_I32) ? 4 : 2;
    u8 b;
    (void)stack_bytes;
    if (sz == 2)
        m6502_push2();
    else {
        for (b = 0; b < sz; b++)
            cb_emit8(M6502_DEX);
    }
    if (m6502_static_frame) {
        for (b = 0; b < sz; b++) {
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(m6502_static_base + off + b));
            cb_emit16(0x0000);
            m6502_sta_stk(b);
        }
    } else {
        m6502_ldy_lfp();
        for (b = 0; b < sz; b++) {
            m6502_lda_local((u8)(off + b));
            m6502_sta_stk(b);
        }
    }
}

static void m6502_local_set(size_t slot, StackType type, u16 stack_bytes) {
    u16 off = m6502_local_off.data[slot];
    u8 sz = (type == TY_F || type == TY_I32) ? 4 : 2;
    u8 b;
    (void)stack_bytes;
    if (!m6502_static_frame)
        m6502_ldy_lfp();
    for (b = 0; b < sz; b++) {
        m6502_lda_stk(b);
        m6502_emit_local_store((u16)(off + b));
    }
    if (sz == 2)
        m6502_pop2();
    else {
        for (b = 0; b < sz; b++)
            cb_emit8(M6502_INX);
    }
}

static void m6502_local_addr(size_t slot) {
    u16 lo_pos, hi_pos;
    if (m6502_static_frame) {
        m6502_push_data((u16)(m6502_static_base + m6502_local_off.data[slot]));
        return;
    }
    if (m6502_local_is_big.data[slot]) {
        u16 off = m6502_local_off.data[slot];
        ensure_big_alloc();
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_CLC);
        cb_emit8(M6502_LDA_ABS);
        cb_add_data_reloc(cb_pc(), bfp_data_off);
        cb_emit16(0x0000);
        cb_emit8_8(M6502_ADC_IMM, (u8)(off & 0xFF));
        m6502_sta_stk(0);
        cb_emit8(M6502_LDA_ABS);
        cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
        cb_emit16(0x0000);
        cb_emit8_8(M6502_ADC_IMM, (u8)(off >> 8));
        m6502_sta_stk(1);
        return;
    }
    ensure_data_alloc();
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_CLC);
    cb_emit8(M6502_LDA_IMM);
    lo_pos = cb_pc();
    cb_emit8(0x00); /* LDA #<local */
    cb_emit8(M6502_ADC_ABS);
    cb_add_data_reloc(cb_pc(), lfp_data_off);
    cb_emit16(0x0000); /* ADC lfp */
    m6502_sta_stk(0);
    cb_emit8(M6502_LDA_IMM);
    hi_pos = cb_pc();
    cb_emit8(0x00); /* LDA #>local */
    cb_emit8_8(M6502_ADC_IMM, 0x00);
    m6502_sta_stk(1);
    cb_add_data_reloc_split(lo_pos, hi_pos, (u16)(locals_data_off + m6502_local_off.data[slot]));
}

static void m6502_local_add(size_t slot) {
    u16 off = m6502_local_off.data[slot];
    u8 b;
    if (m6502_static_frame) {
        cb_emit8(M6502_CLC);
        for (b = 0; b < 2; b++) {
            m6502_lda_stk(b);
            cb_emit8(M6502_ADC_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(m6502_static_base + off + b));
            cb_emit16(0x0000);
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(m6502_static_base + off + b));
            cb_emit16(0x0000);
        }
        m6502_pop2();
        return;
    }
    m6502_ldy_lfp();
    cb_emit8(M6502_CLC);
    for (b = 0; b < 2; b++) {
        m6502_lda_stk(b);
        cb_emit8(0x79); /* ADC abs,Y */
        cb_add_data_reloc(cb_pc(), (u16)(locals_data_off + off + b));
        cb_emit16(0x0000);
        m6502_emit_local_store((u16)(off + b));
    }
    m6502_pop2();
}

static int m6502_local_add_imm(int slot, StackType type, size_t stack_bytes, i32 imm) {
    u16 off = m6502_local_off.data[slot];
    u16 k = (u16)(i16)imm;
    (void)type;
    (void)stack_bytes;
    if (m6502_compact)
        return 0;
    if (k == 0)
        return 1;
    if (m6502_static_frame) {
        u16 addr = (u16)(m6502_static_base + off);
        if (k == 1) {
            cb_emit8(0xEE); /* INC abs */
            cb_add_data_reloc(cb_pc(), addr);
            cb_emit16(0x0000);
            cb_emit8_8(M6502_BNE, 3);
            cb_emit8(0xEE); /* INC abs+1 */
            cb_add_data_reloc(cb_pc(), (u16)(addr + 1));
            cb_emit16(0x0000);
            return 1;
        }
        if (k == 0xFFFF) {
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), addr);
            cb_emit16(0x0000);
            cb_emit8_8(M6502_BNE, 3);
            cb_emit8(0xCE); /* DEC abs+1 */
            cb_add_data_reloc(cb_pc(), (u16)(addr + 1));
            cb_emit16(0x0000);
            cb_emit8(0xCE); /* DEC abs */
            cb_add_data_reloc(cb_pc(), addr);
            cb_emit16(0x0000);
            return 1;
        }
        cb_emit8(M6502_CLC);
        cb_emit8(M6502_LDA_ABS);
        cb_add_data_reloc(cb_pc(), addr);
        cb_emit16(0x0000);
        cb_emit8_8(M6502_ADC_IMM, (u8)(k & 0xFF));
        cb_emit8(M6502_STA_ABS);
        cb_add_data_reloc(cb_pc(), addr);
        cb_emit16(0x0000);
        if ((k >> 8) == 0) {
            cb_emit8_8(M6502_BCC, 3);
            cb_emit8(0xEE); /* INC abs+1 */
            cb_add_data_reloc(cb_pc(), (u16)(addr + 1));
            cb_emit16(0x0000);
        } else {
            cb_emit8(M6502_LDA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(addr + 1));
            cb_emit16(0x0000);
            cb_emit8_8(M6502_ADC_IMM, (u8)(k >> 8));
            cb_emit8(M6502_STA_ABS);
            cb_add_data_reloc(cb_pc(), (u16)(addr + 1));
            cb_emit16(0x0000);
        }
        return 1;
    }
    m6502_ldy_lfp();
    cb_emit8(M6502_CLC);
    m6502_lda_local((u8)off);
    cb_emit8_8(M6502_ADC_IMM, (u8)(k & 0xFF));
    m6502_emit_local_store(off);
    m6502_lda_local((u8)(off + 1));
    cb_emit8_8(M6502_ADC_IMM, (u8)(k >> 8));
    m6502_emit_local_store((u16)(off + 1));
    return 1;
}

static void m6502_local_op(LocalOp op, int slot, StackType type, size_t stack_bytes) {
    if (m6502_compact) {
        m6502_bc_local_op(op, slot, type);
        return;
    }
    if (op == LOCAL_GET)
        m6502_local_get(slot, type, (u16)stack_bytes);
    else if (op == LOCAL_ADDR)
        m6502_local_addr(slot);
    else if (op == LOCAL_ADD)
        m6502_local_add(slot);
    else
        m6502_local_set(slot, type, (u16)stack_bytes);
}

static void m6502_stack_op(StackOp op, StackType t1, StackType t2) {
    u8 sz1 = (t1 == TY_F || t1 == TY_I32) ? 4 : 2;
    u8 sz2 = (t2 == TY_F || t2 == TY_I32) ? 4 : 2;
    u8 b;
    if (m6502_compact) {
        bc_emit_stack_op(op, t1, t2);
        return;
    }
    switch (op) {
    case OP_DUP:
        if (sz1 == 2)
            m6502_push2();
        else {
            for (b = 0; b < sz1; b++)
                cb_emit8(M6502_DEX);
        }
        for (b = 0; b < sz1; b++) {
            m6502_lda_stk((u8)(sz1 + b));
            m6502_sta_stk(b);
        }
        break;
    case OP_DROP:
        if (sz1 == 2)
            m6502_pop2();
        else {
            for (b = 0; b < sz1; b++)
                cb_emit8(M6502_INX);
        }
        break;
    case OP_SWAP:
        if (sz1 == sz2) {
            for (b = 0; b < sz1; b++) {
                m6502_ldy_stk(b);
                m6502_lda_stk((u8)(sz1 + b));
                m6502_sta_stk(b);
                cb_emit8(M6502_TYA);
                m6502_sta_stk((u8)(sz1 + b));
            }
        } else {
            /* Save TOS (sz1 <= 4 bytes) in zp_scratch, shift NOS down, store TOS */
            for (b = 0; b < sz1; b++) {
                m6502_lda_stk(b);
                cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + b));
            }
            for (b = 0; b < sz2; b++) {
                m6502_lda_stk((u8)(sz1 + b));
                m6502_sta_stk(b);
            }
            for (b = 0; b < sz1; b++) {
                cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + b));
                m6502_sta_stk((u8)(sz2 + b));
            }
        }
        break;
    default:
        ir4_die("unimplemented 6502 stack op");
    }
}

static size_t m6502_cmp_mat_pc = 0;
static size_t m6502_cmp_end_pc = 0;
static u8 m6502_cmp_true_op = 0;
static u8 m6502_cmp_pops = 0;
static size_t m6502_inx2_pc = 0;

void be_6502_peep_fence(void) {
    m6502_inx2_pc = 0;
    m6502_cmp_end_pc = 0;
}

static void m6502_pop2(void) {
    cb_emit8(M6502_INX);
    cb_emit8(M6502_INX);
    if (ir4_get_opt_level() && !m6502_compact)
        m6502_inx2_pc = cb_pc();
}

static void m6502_push2(void) {
    if (!m6502_compact && m6502_inx2_pc != 0 && cb_pc() == m6502_inx2_pc) {
        cb_rewind(2);
        m6502_inx2_pc = 0;
        return;
    }
    m6502_inx2_pc = 0;
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
}

static void m6502_int_cmp(IrOp op) {
    u8 lhs_off = 2;
    u8 rhs_off = 0;

    switch (op) {
    case OP_GT:
    case OP_LE:
    case OP_UGT:
    case OP_ULE:
    case OP_AGT:
        lhs_off = 0;
        rhs_off = 2;
        break;
    default:
        break;
    }

    switch (op) {
    case OP_EQ:
    case OP_NE:
    case OP_AEQ:
    case OP_ANE:
        m6502_lda_stk(2);
        m6502_eor_stk(0);
        cb_emit8_8(M6502_STA_ZP, zp_scratch);
        m6502_lda_stk(3);
        m6502_eor_stk(1);
        cb_emit8_8(M6502_ORA_ZP, zp_scratch);
        if (op == OP_EQ || op == OP_AEQ) {
            m6502_cmp_true_op = M6502_BEQ;
        } else {
            m6502_cmp_true_op = M6502_BNE;
        }
        m6502_cmp_mat_pc = cb_pc();
        cb_emit8_8(m6502_cmp_true_op, 0x04); /* Bxx true */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        m6502_cmp_pops = 2;
        m6502_cmp_end_pc = cb_pc();
        break;
    case OP_ULT:
    case OP_UGT:
    case OP_ALT:
    case OP_AGT:
    case OP_UGE:
    case OP_ULE:
        cb_emit8(M6502_SEC);
        m6502_lda_stk(lhs_off);
        m6502_sbc_stk(rhs_off);
        m6502_lda_stk((u8)(lhs_off + 1));
        m6502_sbc_stk((u8)(rhs_off + 1));
        if (op == OP_UGE || op == OP_ULE) {
            m6502_cmp_true_op = M6502_BCS;
        } else {
            m6502_cmp_true_op = M6502_BCC;
        }
        m6502_cmp_mat_pc = cb_pc();
        cb_emit8_8(m6502_cmp_true_op, 0x04); /* Bxx true */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        m6502_cmp_pops = 2;
        m6502_cmp_end_pc = cb_pc();
        break;
    case OP_LT:
    case OP_GT:
    case OP_GE:
    case OP_LE:
        cb_emit8(M6502_SEC);
        m6502_lda_stk(lhs_off);
        m6502_sbc_stk(rhs_off);
        m6502_lda_stk((u8)(lhs_off + 1));
        m6502_sbc_stk((u8)(rhs_off + 1));
        cb_emit8_8(M6502_BVC, 0x02);
        cb_emit8_8(M6502_EOR_IMM, 0x80);
        if (op == OP_GE || op == OP_LE) {
            m6502_cmp_true_op = M6502_BPL;
        } else {
            m6502_cmp_true_op = M6502_BMI;
        }
        m6502_cmp_mat_pc = cb_pc();
        cb_emit8_8(m6502_cmp_true_op, 0x04); /* Bxx true */
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        m6502_cmp_pops = 2;
        m6502_cmp_end_pc = cb_pc();
        break;
    default:
        ir4_die("unimplemented 6502 comparison op");
    }
}

static void m6502_mem_addr_setup_off(u8 y_off) {
    m6502_lda_stk(0);
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    m6502_lda_stk(1);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_emit8_8(M6502_LDY_IMM, y_off);
}

static void emit_neg_stk(u8 disp) {
    cb_emit8(M6502_SEC);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    m6502_sbc_stk(disp);
    m6502_sta_stk(disp);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    m6502_sbc_stk((u8)(disp + 1));
    m6502_sta_stk((u8)(disp + 1));
}

static void emit_neg_zp(u8 zp) {
    cb_emit8(M6502_SEC);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_8(M6502_SBC_ZP, zp);
    cb_emit8_8(M6502_STA_ZP, zp);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_8(M6502_SBC_ZP, (u8)(zp + 1));
    cb_emit8_8(M6502_STA_ZP, (u8)(zp + 1));
}

static void ensure_mul16(void) {
    u16 skip_patch, loop_pc, bcc_patch;
    if (mul16_addr)
        return;
    cb_emit8(M6502_JMP);
    skip_patch = cb_pc();
    cb_emit16(0x0000);
    mul16_addr = cb_cur_addr();

    /* prod (zp0/zp1) = 0, Y = 16 */
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_emit8_8(M6502_LDY_IMM, 16);

    loop_pc = cb_pc();
    m6502_lsr_stk(1);
    m6502_ror_stk(0);
    cb_emit8(M6502_BCC);
    bcc_patch = cb_pc();
    cb_emit8(0x00); /* BCC skip_add */
    cb_emit8(M6502_CLC);
    cb_emit8_8(M6502_LDA_ZP, zp_scratch);
    m6502_adc_stk(2);
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 1));
    m6502_adc_stk(3);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_patch8(bcc_patch, (u8)(cb_pc() - (bcc_patch + 1)));

    m6502_asl_stk(2);
    m6502_lda_stk(3);
    cb_emit8(M6502_ROL_A); /* ROL A (avoids $3E sim65 bug) */
    m6502_sta_stk(3);
    cb_emit8(M6502_DEY);
    cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));

    cb_emit8_8(M6502_LDA_ZP, zp_scratch);
    m6502_sta_stk(2);
    cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 1));
    m6502_sta_stk(3);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_RTS);

    cb_patch16(skip_patch, cb_cur_addr());
}

static void ensure_udivmod16(void) {
    u16 skip_patch, loop_pc, bcc_patch;
    if (udivmod16_addr)
        return;
    cb_emit8(M6502_JMP);
    skip_patch = cb_pc();
    cb_emit16(0x0000);
    udivmod16_addr = cb_cur_addr();

    /* rem (zp0/zp1) = 0, Y = 16; quotient in stack+2/3,X */
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_emit8_8(M6502_LDY_IMM, 16);

    loop_pc = cb_pc();
    m6502_asl_stk(2);
    m6502_lda_stk(3);
    cb_emit8(M6502_ROL_A);
    m6502_sta_stk(3);
    cb_emit8_8(M6502_ROL_ZP, zp_scratch);
    cb_emit8_8(M6502_ROL_ZP, (u8)(zp_scratch + 1));

    cb_emit8(M6502_SEC);
    cb_emit8_8(M6502_LDA_ZP, zp_scratch);
    m6502_sbc_stk(0);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 2));
    cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 1));
    m6502_sbc_stk(1);
    cb_emit8(M6502_BCC);
    bcc_patch = cb_pc();
    cb_emit8(0x00); /* BCC skip_sub */
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 2));
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    emit_stack_op(0xFE, 2); /* INC stack+2,X */
    cb_patch8(bcc_patch, (u8)(cb_pc() - (bcc_patch + 1)));

    cb_emit8(M6502_DEY);
    cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
    cb_emit8(M6502_RTS);

    cb_patch16(skip_patch, cb_cur_addr());
}

static void ensure_sdivmod16(void) {
    u16 skip_patch, p1, p2, p3, p4;
    if (sdivmod16_addr)
        return;
    ensure_udivmod16();
    cb_emit8(M6502_JMP);
    skip_patch = cb_pc();
    cb_emit16(0x0000);
    sdivmod16_addr = cb_cur_addr();

    /* Save dividend sign (zp3) and quotient sign (HW stack) */
    m6502_lda_stk(3);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 3));
    m6502_eor_stk(1);
    cb_emit8(M6502_PHA);

    /* If dividend < 0, negate stack+2/3,X */
    m6502_lda_stk(3);
    cb_emit8(M6502_BPL);
    p1 = cb_pc();
    cb_emit8(0x00);
    emit_neg_stk(2);
    cb_patch8(p1, (u8)(cb_pc() - (p1 + 1)));

    /* If divisor < 0, negate stack+0/1,X */
    m6502_lda_stk(1);
    cb_emit8(M6502_BPL);
    p2 = cb_pc();
    cb_emit8(0x00);
    emit_neg_stk(0);
    cb_patch8(p2, (u8)(cb_pc() - (p2 + 1)));

    cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 3));
    cb_emit8(M6502_PHA);
    cb_emit8_16(M6502_JSR, udivmod16_addr);

    /* Apply dividend sign to remainder (zp0/zp1) */
    cb_emit8(M6502_PLA);
    cb_emit8(M6502_BPL);
    p3 = cb_pc();
    cb_emit8(0x00);
    emit_neg_zp(zp_scratch);
    cb_patch8(p3, (u8)(cb_pc() - (p3 + 1)));

    /* Apply quotient sign to quotient (stack+2/3,X) */
    cb_emit8(M6502_PLA);
    cb_emit8(M6502_BPL);
    p4 = cb_pc();
    cb_emit8(0x00);
    emit_neg_stk(2);
    cb_patch8(p4, (u8)(cb_pc() - (p4 + 1)));

    cb_emit8(M6502_RTS);
    cb_patch16(skip_patch, cb_cur_addr());
}

static void ensure_rt_blob(u16 *addr, const u8 *code, u16 code_len, const u16 *relocs,
                           u16 reloc_cnt) {
    u16 jmp_skip;
    u16 base_pc;
    u16 i;
    if (*addr != 0)
        return;

    cb_emit8(M6502_JMP);
    jmp_skip = cb_pc();
    cb_emit16(0x0000); /* JMP skip */
    base_pc = cb_pc();
    *addr = cb_cur_addr();
    for (i = 0; i < code_len; i++) {
        cb_emit8(code[i]);
    }
    for (i = 0; i < reloc_cnt; i++) {
        u16 r = relocs[i];
        u16 orig = (u16)code[r] | ((u16)code[r + 1] << 8);
        cb_patch16((u16)(base_pc + r), (u16)(orig + *addr));
    }
    cb_patch16(jmp_skip, cb_cur_addr());
}

static void ensure_wozfp(void) {
    ensure_rt_blob(&wozfp_addr, be_6502_wozfp, (u16)sizeof(be_6502_wozfp), be_6502_wozfp_relocs,
                   (u16)BE_6502_WOZFP_RELOC_CNT);
}

static void wozfp_pop_to(u8 zp_base) {
    u8 b;
    for (b = 0; b < 4; b++) {
        m6502_lda_stk(b);
        cb_emit8_8(M6502_STA_ZP, (u8)(zp_base + b));
    }
    for (b = 0; b < 4; b++)
        cb_emit8(M6502_INX);
}

static void wozfp_push_from(u8 zp_base) {
    u8 b;
    for (b = 0; b < 4; b++)
        cb_emit8(M6502_DEX);
    for (b = 0; b < 4; b++) {
        cb_emit8_8(M6502_LDA_ZP, (u8)(zp_base + b));
        m6502_sta_stk(b);
    }
}

static void wozfp_call_keep_x(u16 entry_off) {
    cb_emit8_8(M6502_STX_ZP, zp_scratch);
    cb_emit8_16(M6502_JSR, (u16)(wozfp_addr + entry_off));
    cb_emit8_8(M6502_LDX_ZP, zp_scratch); /* LDX zp_scratch (preserves A) */
}

static void emit_data_abs(u8 opcode, u16 data_offset) {
    cb_emit8(opcode);
    cb_add_data_reloc(cb_pc(), data_offset);
    cb_emit16(0x0000);
}

static int m6502_data_mem(IrOp op, size_t data_offset) {
    if (m6502_compact)
        return 0;
    switch (op) {
    case OP_U8FETCH:
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        emit_data_abs(0xAD, data_offset); /* LDA abs */
        m6502_sta_stk(0);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(1);
        return 1;
    case OP_I8FETCH:
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        emit_data_abs(0xAD, data_offset); /* LDA abs */
        m6502_sta_stk(0);
        cb_emit8_8(M6502_BPL, 0x04);
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        cb_emit8_8(M6502_BMI, 0x02);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(1);
        return 1;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        emit_data_abs(0xAD, data_offset); /* LDA abs */
        m6502_sta_stk(0);
        emit_data_abs(0xAD, (u16)(data_offset + 1)); /* LDA abs+1 */
        m6502_sta_stk(1);
        return 1;
    case OP_I8STORE:
        m6502_lda_stk(0);
        emit_data_abs(0x8D, data_offset); /* STA abs */
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        return 1;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
        m6502_lda_stk(0);
        emit_data_abs(0x8D, data_offset); /* STA abs */
        m6502_lda_stk(1);
        emit_data_abs(0x8D, (u16)(data_offset + 1)); /* STA abs+1 */
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        return 1;
    case OP_I32FETCH: {
        u8 b;
        for (b = 0; b < 4; b++)
            cb_emit8(M6502_DEX);
        for (b = 0; b < 4; b++) {
            emit_data_abs(0xAD, (u16)(data_offset + b));
            m6502_sta_stk(b);
        }
        return 1;
    }
    case OP_I32STORE: {
        u8 b;
        for (b = 0; b < 4; b++) {
            m6502_lda_stk(b);
            emit_data_abs(0x8D, (u16)(data_offset + b));
        }
        for (b = 0; b < 4; b++)
            cb_emit8(M6502_INX);
        return 1;
    }
    default:
        return 0;
    }
}

static int m6502_bit_imm(IrOp op, i32 imm) {
    u16 v = (u16)(i16)imm;
    u8 lo = (u8)(v & 0xFF);
    u8 hi = (u8)(v >> 8);
    if (m6502_compact)
        return 0;
    if (op == OP_AND) {
        if (lo == 0) {
            cb_emit8_8(M6502_LDA_IMM, 0);
            m6502_sta_stk(0);
        } else if (lo != 0xFF) {
            m6502_lda_stk(0);
            cb_emit8_8(M6502_AND_IMM, lo);
            m6502_sta_stk(0);
        }
        if (hi == 0) {
            cb_emit8_8(M6502_LDA_IMM, 0);
            m6502_sta_stk(1);
        } else if (hi != 0xFF) {
            m6502_lda_stk(1);
            cb_emit8_8(M6502_AND_IMM, hi);
            m6502_sta_stk(1);
        }
        return 1;
    }
    if (op == OP_OR) {
        if (lo == 0xFF) {
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
            m6502_sta_stk(0);
        } else if (lo != 0) {
            m6502_lda_stk(0);
            cb_emit8_8(M6502_ORA_IMM, lo);
            m6502_sta_stk(0);
        }
        if (hi == 0xFF) {
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
            m6502_sta_stk(1);
        } else if (hi != 0) {
            m6502_lda_stk(1);
            cb_emit8_8(M6502_ORA_IMM, hi);
            m6502_sta_stk(1);
        }
        return 1;
    }
    if (op == OP_XOR) {
        if (lo != 0) {
            m6502_lda_stk(0);
            cb_emit8_8(M6502_EOR_IMM, lo);
            m6502_sta_stk(0);
        }
        if (hi != 0) {
            m6502_lda_stk(1);
            cb_emit8_8(M6502_EOR_IMM, hi);
            m6502_sta_stk(1);
        }
        return 1;
    }
    return 0;
}

static int m6502_mem_imm(IrOp op, i32 off) {
    i32 sz;
    int swapped = (op >= OP_SI8STORE && op <= OP_SFSTORE);
    u8 val_off;
    if (m6502_compact)
        return 0;
    switch (op) {
    case OP_U8FETCH:
    case OP_I8FETCH:
    case OP_I8STORE:
    case OP_SI8STORE:
        sz = 1;
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SI16STORE:
        sz = 2;
        break;
    case OP_FFETCH:
    case OP_FSTORE:
    case OP_SFSTORE:
    case OP_I32FETCH:
    case OP_I32STORE:
        sz = 4;
        break;
    default:
        return 0;
    }
    if (off < 0 || off > 256 - sz)
        return 0;

    if (swapped) {
        u8 aoff = (u8)((sz == 4) ? 4 : 2);
        val_off = 0;
        m6502_lda_stk(aoff);
        cb_emit8_8(M6502_STA_ZP, zp_scratch);
        m6502_lda_stk((u8)(aoff + 1));
        cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
        cb_emit8_8(M6502_LDY_IMM, (u8)off);
    } else {
        val_off = 2;
        m6502_mem_addr_setup_off((u8)off);
    }
    switch (op) {
    case OP_U8FETCH:
        cb_emit8_8(M6502_LDA_INDY, zp_scratch);
        m6502_sta_stk(0);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(1);
        break;
    case OP_I8FETCH:
        cb_emit8_8(M6502_LDA_INDY, zp_scratch);
        m6502_sta_stk(0);
        cb_emit8_8(M6502_BPL, 0x04);
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        cb_emit8_8(M6502_BMI, 0x02);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(1);
        break;
    case OP_I8STORE:
    case OP_SI8STORE:
        m6502_lda_stk(val_off);
        cb_emit8_8(M6502_STA_INDY, zp_scratch);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        break;
    case OP_FETCH:
    case OP_AFETCH:
    case OP_I16FETCH:
    case OP_U16FETCH:
        cb_emit8_8(M6502_LDA_INDY, zp_scratch);
        m6502_sta_stk(0);
        cb_emit8(M6502_INY);
        cb_emit8_8(M6502_LDA_INDY, zp_scratch);
        m6502_sta_stk(1);
        break;
    case OP_STORE:
    case OP_ASTORE:
    case OP_I16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_SI16STORE:
        m6502_lda_stk(val_off);
        cb_emit8_8(M6502_STA_INDY, zp_scratch);
        cb_emit8(M6502_INY);
        m6502_lda_stk((u8)(val_off + 1));
        cb_emit8_8(M6502_STA_INDY, zp_scratch);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        break;
    case OP_FFETCH:
    case OP_I32FETCH: {
        u8 b;
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        for (b = 0; b < 4; b++) {
            cb_emit8_8(M6502_LDA_INDY, zp_scratch);
            m6502_sta_stk(b);
            if (b < 3)
                cb_emit8(M6502_INY);
        }
        break;
    }
    case OP_FSTORE:
    case OP_SFSTORE:
    case OP_I32STORE: {
        u8 b;
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(val_off + b));
            cb_emit8_8(M6502_STA_INDY, zp_scratch);
            if (b < 3)
                cb_emit8(M6502_INY);
        }
        for (b = 0; b < 6; b++)
            cb_emit8(M6502_INX);
        break;
    }
    default:
        break;
    }
    return 1;
}

static void m6502_op(IrOp op) {
    if (m6502_compact) {
        bc_emit_op(op, 2);
        return;
    }
    switch (op) {
    case OP_INVERT:
        m6502_lda_stk(0);
        cb_emit8_8(M6502_EOR_IMM, 0xFF);
        m6502_sta_stk(0);
        m6502_lda_stk(1);
        cb_emit8_8(M6502_EOR_IMM, 0xFF);
        m6502_sta_stk(1);
        break;
    case OP_ADD:
    case OP_AADD:
        cb_emit8(M6502_CLC);
        m6502_lda_stk(2);
        m6502_adc_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_adc_stk(1);
        m6502_sta_stk(3);
        m6502_pop2();
        break;
    case OP_SUB:
    case OP_ASUB:
    case OP_ADIFF:
        cb_emit8(M6502_SEC);
        m6502_lda_stk(2);
        m6502_sbc_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_sbc_stk(1);
        m6502_sta_stk(3);
        m6502_pop2();
        break;
    case OP_AND:
        m6502_lda_stk(2);
        m6502_and_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_and_stk(1);
        m6502_sta_stk(3);
        m6502_pop2();
        break;
    case OP_OR:
        m6502_lda_stk(2);
        m6502_ora_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_ora_stk(1);
        m6502_sta_stk(3);
        m6502_pop2();
        break;
    case OP_XOR:
        m6502_lda_stk(2);
        m6502_eor_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(3);
        m6502_eor_stk(1);
        m6502_sta_stk(3);
        m6502_pop2();
        break;
    case OP_LSHIFT:
    case OP_RSHIFT:
    case OP_ARSHIFT:
        m6502_ldy_stk(0);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8_8(0xC0, 0x00); /* CPY #0 */
        if (op == OP_LSHIFT) {
            /* Avoid ROL abs,X ($3E) due to a PC-advance bug in sim65 V2.18 */
            u16 skip_br;
            u16 loop_pc;
            cb_emit8(M6502_BEQ);
            skip_br = cb_pc();
            cb_emit8(0x00);
            loop_pc = cb_pc();
            m6502_asl_stk(0);
            m6502_lda_stk(1);
            cb_emit8(M6502_ROL_A);
            m6502_sta_stk(1);
            cb_emit8(M6502_DEY);
            cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
            cb_patch8(skip_br, (u8)(cb_pc() - (skip_br + 1)));
        } else if (op == OP_RSHIFT) {
            u16 skip_br;
            u16 loop_pc;
            cb_emit8(M6502_BEQ);
            skip_br = cb_pc();
            cb_emit8(0x00);
            loop_pc = cb_pc();
            m6502_lsr_stk(1);
            m6502_ror_stk(0);
            cb_emit8(M6502_DEY);
            cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
            cb_patch8(skip_br, (u8)(cb_pc() - (skip_br + 1)));
        } else {
            u16 skip_br;
            u16 loop_pc;
            cb_emit8(M6502_BEQ);
            skip_br = cb_pc();
            cb_emit8(0x00);
            loop_pc = cb_pc();
            m6502_lda_stk(1);
            cb_emit8(0x0A); /* ASL A */
            m6502_ror_stk(1);
            m6502_ror_stk(0);
            cb_emit8(M6502_DEY);
            cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
            cb_patch8(skip_br, (u8)(cb_pc() - (skip_br + 1)));
        }
        break;
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
        m6502_int_cmp(op);
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
    case OP_SI8STORE:
    case OP_SI16STORE:
    case OP_SSTORE:
    case OP_SASTORE:
    case OP_I32FETCH:
    case OP_I32STORE:
        m6502_mem_imm(op, 0);
        break;
    case OP_BCOPY: {
        u16 skip_pages, page_loop, byte_loop, skip_tail, tail_loop;
        m6502_lda_stk(2);
        cb_emit8_8(M6502_STA_ZP, zp_scratch);
        m6502_lda_stk(3);
        cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
        m6502_lda_stk(4);
        cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 2));
        m6502_lda_stk(5);
        cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 3));
        m6502_lda_stk(0);
        cb_emit8(0x48); /* PHA len_lo */
        m6502_lda_stk(1);
        cb_emit8(0x48); /* PHA len_hi */
        for (skip_pages = 0; skip_pages < 6; skip_pages++)
            cb_emit8(M6502_INX);
        cb_emit8_8(M6502_LDY_IMM, 0x00);
        cb_emit8(0x68); /* PLA len_hi */
        cb_emit8(M6502_BEQ);
        skip_pages = cb_pc();
        cb_emit8(0x00);
        page_loop = cb_pc();
        cb_emit8(0x48); /* PHA */
        byte_loop = cb_pc();
        cb_emit8_8(0xB1, zp_scratch);           /* LDA (zp0),Y */
        cb_emit8_8(0x91, (u8)(zp_scratch + 2)); /* STA (zp2),Y */
        cb_emit8(M6502_INY);
        cb_emit8_8(M6502_BNE, (u8)(byte_loop - (cb_pc() + 2)));
        cb_emit8_8(0xE6, (u8)(zp_scratch + 1)); /* INC zp1 */
        cb_emit8_8(0xE6, (u8)(zp_scratch + 3)); /* INC zp3 */
        cb_emit8(0x68);                         /* PLA */
        cb_emit8(M6502_SEC);
        cb_emit8_8(0xE9, 0x01); /* SBC #1 */
        cb_emit8_8(M6502_BNE, (u8)(page_loop - (cb_pc() + 2)));
        cb_patch8(skip_pages, (u8)(cb_pc() - (skip_pages + 1)));
        cb_emit8(0x68); /* PLA len_lo */
        cb_emit8(M6502_BEQ);
        skip_tail = cb_pc();
        cb_emit8(0x00);
        cb_emit8(0xA8); /* TAY */
        tail_loop = cb_pc();
        cb_emit8(M6502_DEY);
        cb_emit8_8(0xB1, zp_scratch);           /* LDA (zp0),Y */
        cb_emit8_8(0x91, (u8)(zp_scratch + 2)); /* STA (zp2),Y */
        cb_emit8(0x98);                         /* TYA */
        cb_emit8_8(M6502_BNE, (u8)(tail_loop - (cb_pc() + 2)));
        cb_patch8(skip_tail, (u8)(cb_pc() - (skip_tail + 1)));
        break;
    }
    case OP_INV32: {
        u8 b;
        for (b = 0; b < 4; b++) {
            m6502_lda_stk(b);
            cb_emit8_8(M6502_EOR_IMM, 0xFF);
            m6502_sta_stk(b);
        }
        break;
    }
    case OP_ADD32: {
        u8 b;
        cb_emit8(M6502_CLC);
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(4 + b));
            m6502_adc_stk(b);
            m6502_sta_stk((u8)(4 + b));
        }
        for (b = 0; b < 4; b++)
            cb_emit8(M6502_INX);
        break;
    }
    case OP_SUB32: {
        u8 b;
        cb_emit8(M6502_SEC);
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(4 + b));
            m6502_sbc_stk(b);
            m6502_sta_stk((u8)(4 + b));
        }
        for (b = 0; b < 4; b++)
            cb_emit8(M6502_INX);
        break;
    }
    case OP_AND32:
    case OP_OR32:
    case OP_XOR32: {
        u8 b;
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(4 + b));
            if (op == OP_AND32)
                m6502_and_stk(b);
            else if (op == OP_OR32)
                m6502_ora_stk(b);
            else
                m6502_eor_stk(b);
            m6502_sta_stk((u8)(4 + b));
        }
        for (b = 0; b < 4; b++)
            cb_emit8(M6502_INX);
        break;
    }
    case OP_W2I32:
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        m6502_lda_stk(2);
        m6502_sta_stk(0);
        m6502_lda_stk(3);
        m6502_sta_stk(1);
        cb_emit8_8(M6502_BPL, 0x04);
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        cb_emit8_8(M6502_BMI, 0x02);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        break;
    case OP_U2I32:
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        m6502_lda_stk(2);
        m6502_sta_stk(0);
        m6502_lda_stk(3);
        m6502_sta_stk(1);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_sta_stk(2);
        m6502_sta_stk(3);
        break;
    case OP_I322W:
        m6502_lda_stk(0);
        m6502_sta_stk(2);
        m6502_lda_stk(1);
        m6502_sta_stk(3);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        break;
    case OP_EQ32:
    case OP_NE32: {
        u8 b;
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_STA_ZP, zp_scratch);
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(4 + b));
            m6502_eor_stk(b);
            cb_emit8_8(M6502_ORA_ZP, zp_scratch);
            cb_emit8_8(M6502_STA_ZP, zp_scratch);
        }
        for (b = 0; b < 6; b++)
            cb_emit8(M6502_INX);
        cb_emit8_8(M6502_LDA_ZP, zp_scratch);
        cb_emit8_8(op == OP_EQ32 ? M6502_BEQ : M6502_BNE, 0x04);
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_BEQ, 0x02);
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        m6502_sta_stk(0);
        m6502_sta_stk(1);
        break;
    }
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
        u8 lhs_off = swap_ops ? 0 : 4;
        u8 rhs_off = swap_ops ? 4 : 0;
        u8 b;
        cb_emit8(M6502_SEC);
        for (b = 0; b < 4; b++) {
            m6502_lda_stk((u8)(lhs_off + b));
            m6502_sbc_stk((u8)(rhs_off + b));
        }
        for (b = 0; b < 6; b++)
            cb_emit8(M6502_INX);
        if (is_signed) {
            cb_emit8_8(M6502_BVC, 0x02);
            cb_emit8_8(M6502_EOR_IMM, 0x80);
            cb_emit8_8(0x09, 0x00); /* ORA #$00: refresh N from A after INX */
            cb_emit8_8(invert ? M6502_BPL : M6502_BMI, 0x04);
        } else {
            cb_emit8_8(invert ? M6502_BCS : M6502_BCC, 0x04);
        }
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        cb_emit8_8(M6502_BEQ, 0x02);
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        m6502_sta_stk(0);
        m6502_sta_stk(1);
        break;
    }
    case OP_SFSTORE:
        m6502_mem_imm(op, 0);
        break;
    case OP_SEXT:
        cb_emit8_8(M6502_LDA_IMM, 0x00);
        m6502_ldy_stk(0);
        cb_emit8_8(M6502_BPL, 0x02);
        cb_emit8_8(M6502_LDA_IMM, 0xFF);
        m6502_sta_stk(1);
        break;
    case OP_W2A:
    case OP_A2W:
        /* Zero-cost no-op on 6502 */
        break;
    case OP_MUL:
        ensure_mul16();
        cb_emit8_16(M6502_JSR, mul16_addr);
        break;
    case OP_UDIV:
        ensure_udivmod16();
        cb_emit8_16(M6502_JSR, udivmod16_addr);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        break;
    case OP_UMOD:
        ensure_udivmod16();
        cb_emit8_16(M6502_JSR, udivmod16_addr);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8_8(M6502_LDA_ZP, zp_scratch);
        m6502_sta_stk(0);
        cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 1));
        m6502_sta_stk(1);
        break;
    case OP_DIV:
        ensure_sdivmod16();
        cb_emit8_16(M6502_JSR, sdivmod16_addr);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        break;
    case OP_MOD:
        ensure_sdivmod16();
        cb_emit8_16(M6502_JSR, sdivmod16_addr);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8_8(M6502_LDA_ZP, zp_scratch);
        m6502_sta_stk(0);
        cb_emit8_8(M6502_LDA_ZP, (u8)(zp_scratch + 1));
        m6502_sta_stk(1);
        break;
    case OP_FADD:
    case OP_FSUB:
    case OP_FMUL:
    case OP_FDIV: {
        u16 entry = (op == OP_FADD)   ? RT_FP_ADD
                    : (op == OP_FSUB) ? RT_FP_SUB
                    : (op == OP_FMUL) ? RT_FP_MUL
                                      : RT_FP_DIV;
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1); /* rhs -> X1/M1 */
        wozfp_pop_to(WOZFP_ZP_X2); /* lhs -> X2/M2 */
        wozfp_call_keep_x(entry);
        wozfp_push_from(WOZFP_ZP_X1);
        break;
    }
    case OP_FNEG:
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1);
        wozfp_call_keep_x(RT_FP_NEG);
        wozfp_push_from(WOZFP_ZP_X1);
        break;
    case OP_FEQ:
    case OP_FNE:
    case OP_FLT:
    case OP_FGT:
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1); /* rhs -> X1/M1 */
        wozfp_pop_to(WOZFP_ZP_X2); /* lhs -> X2/M2 */
        wozfp_call_keep_x(RT_FP_CMP);
        /* A = $FF if lhs<rhs, 0 if lhs==rhs, 1 if lhs>rhs */
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        if (op == OP_FEQ) {
            cb_emit8_8(M6502_CMP_IMM, 0x00);
            cb_emit8_8(M6502_BEQ, 0x04); /* BEQ true */
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
        } else if (op == OP_FNE) {
            cb_emit8_8(M6502_CMP_IMM, 0x00);
            cb_emit8_8(M6502_BNE, 0x04); /* BNE true */
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
        } else if (op == OP_FLT) {
            cb_emit8_8(M6502_CMP_IMM, 0xFF);
            cb_emit8_8(M6502_BEQ, 0x04); /* BEQ true */
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
        } else { /* OP_FGT */
            cb_emit8_8(M6502_CMP_IMM, 0x01);
            cb_emit8_8(M6502_BEQ, 0x04); /* BEQ true */
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
        }
        m6502_sta_stk(0);
        m6502_sta_stk(1);
        break;
    case OP_FFETCH:
    case OP_FSTORE:
        m6502_mem_imm(op, 0);
        break;
    case OP_W2F:
        ensure_wozfp();
        /* Pop 16-bit signed (lo, hi) -> M1+1 (lo), M1 (hi) */
        m6502_lda_stk(0);
        cb_emit8_8(M6502_STA_ZP, (u8)(WOZFP_ZP_M1 + 1));
        m6502_lda_stk(1);
        cb_emit8_8(M6502_STA_ZP, WOZFP_ZP_M1);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        wozfp_call_keep_x(RT_FP_ITOF);
        wozfp_push_from(WOZFP_ZP_X1);
        break;
    case OP_F2W: {
        u16 skip_neg1;
        u16 skip_neg2;
        ensure_wozfp();
        wozfp_pop_to(WOZFP_ZP_X1);
        /* Truncate toward zero: if M1 < 0, negate before FIX and negate result */
        cb_emit8_8(M6502_LDA_ZP, WOZFP_ZP_M1);
        cb_emit8(M6502_PHA);
        cb_emit8(M6502_BPL);
        skip_neg1 = cb_pc();
        cb_emit8(0x00); /* BPL skip_neg1 */
        wozfp_call_keep_x(RT_FP_NEG);
        cb_patch8(skip_neg1, (u8)(cb_pc() - (skip_neg1 + 1)));
        wozfp_call_keep_x(RT_FP_FTOI);
        cb_emit8(M6502_DEX);
        cb_emit8(M6502_DEX);
        cb_emit8_8(M6502_LDA_ZP, (u8)(WOZFP_ZP_M1 + 1));
        m6502_sta_stk(0);
        cb_emit8_8(M6502_LDA_ZP, WOZFP_ZP_M1);
        m6502_sta_stk(1);
        cb_emit8(M6502_PLA);
        cb_emit8(M6502_BPL);
        skip_neg2 = cb_pc();
        cb_emit8(0x00); /* BPL skip_neg2 */
        emit_neg_stk(0);
        cb_patch8(skip_neg2, (u8)(cb_pc() - (skip_neg2 + 1)));
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
        ir4_die("6502 i32 mul/div/shift handled by lib/i32.ir4");
    }
}

static int m6502_local_mem(int slot, i32 off, IrOp op, size_t stack_bytes) {
    (void)stack_bytes;
    if (m6502_compact || !m6502_static_frame)
        return 0;
    return m6502_data_mem(op, (u16)(m6502_static_base + m6502_local_off.data[slot] + off));
}

static size_t m6502_emit_br_fwd(BranchCond cond) {
    u16 patch_pos;
    if (m6502_compact)
        return bc_emit_br_fwd_op(cond, 2);
    if (ir4_get_opt_level() && cond == BR_JZ && m6502_cmp_end_pc != 0 && cb_pc() == m6502_cmp_end_pc) {
        u8 n_inx = (u8)(m6502_cmp_pops * 2);
        u8 k;
        cb_rewind(cb_pc() - m6502_cmp_mat_pc);
        m6502_cmp_end_pc = 0;
        cb_emit8_8(m6502_cmp_true_op, (u8)(n_inx + 3));
        for (k = 0; k < n_inx; k++)
            cb_emit8(M6502_INX);
        cb_emit8(M6502_JMP);
        patch_pos = cb_pc();
        cb_emit16(0x0000);
        for (k = 0; k < n_inx; k++)
            cb_emit8(M6502_INX);
        return patch_pos;
    }
    m6502_cmp_end_pc = 0;
    if (cond == BR_JZ) {
        m6502_lda_stk(0);
        m6502_ora_stk(1);
        cb_emit8(M6502_PHP);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_PLP);
        cb_emit8_8(M6502_BNE, 0x03);
    }
    cb_emit8(M6502_JMP);
    patch_pos = cb_pc();
    cb_emit16(0x0000);
    return patch_pos;
}

static void m6502_emit_br_back(BranchCond cond, size_t target_addr) {
    if (m6502_compact) {
        bc_emit_br_back_op(cond, target_addr, 2);
        return;
    }
    if (ir4_get_opt_level() && cond == BR_JZ && m6502_cmp_end_pc != 0 && cb_pc() == m6502_cmp_end_pc) {
        u8 n_inx = (u8)(m6502_cmp_pops * 2);
        u8 k;
        cb_rewind(cb_pc() - m6502_cmp_mat_pc);
        m6502_cmp_end_pc = 0;
        cb_emit8_8(m6502_cmp_true_op, (u8)(n_inx + 3));
        for (k = 0; k < n_inx; k++)
            cb_emit8(M6502_INX);
        cb_emit8_16(M6502_JMP, target_addr);
        for (k = 0; k < n_inx; k++)
            cb_emit8(M6502_INX);
        return;
    }
    m6502_cmp_end_pc = 0;
    if (cond == BR_JZ) {
        m6502_lda_stk(0);
        m6502_ora_stk(1);
        cb_emit8(M6502_PHP);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_INX);
        cb_emit8(M6502_PLP);
        cb_emit8_8(M6502_BNE, 0x03);
    }
    cb_emit8_16(M6502_JMP, target_addr);
}

static size_t m6502_emit_jtab(size_t count) {
    size_t lo_pc, hi_pc, tbl_off, k;
    u16 tbl_addr;
    if (m6502_compact)
        return bc_emit_jtab_op(count, 2);
    if (count > 256)
        ir4_die("'jtab' supports at most 256 entries on 6502");
    be_6502_peep_fence();
    m6502_lda_stk(0);
    cb_emit8(M6502_TAY);
    m6502_pop2();
    cb_emit8(0xB9); /* LDA lo_tbl,Y */
    lo_pc = cb_pc();
    cb_emit16(0);
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    cb_emit8(0xB9); /* LDA hi_tbl,Y */
    hi_pc = cb_pc();
    cb_emit16(0);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_emit8_16(0x6C, (u16)zp_scratch); /* JMP (zp_scratch) */
    tbl_addr = cb_cur_addr();
    cb_patch16(lo_pc, tbl_addr);
    cb_patch16(hi_pc, (u16)(tbl_addr + count));
    tbl_off = cb_pc();
    for (k = 0; k < count * 2; k++)
        cb_emit8(0);
    return tbl_off;
}

static void m6502_patch_jtab(size_t patch_offset, size_t idx, size_t count, size_t target_addr) {
    if (m6502_compact) {
        bc_patch_jtab_op(patch_offset, idx, target_addr, 2);
        return;
    }
    cb_patch8(patch_offset + idx, (u8)(target_addr & 0xFF));
    cb_patch8(patch_offset + count + idx, (u8)(target_addr >> 8));
}

static void m6502_patch_fwd(size_t patch_offset) {
    m6502_cmp_end_pc = 0;
    cb_patch16(patch_offset, (u16)cb_cur_addr());
}

static int m6502_add_imm(int is_addr, i32 imm) {
    u16 d = (u16)(i16)imm;
    (void)is_addr;
    if (m6502_compact)
        return bc_emit_add_imm((i16)d);
    if (d == 0)
        return 1;
    cb_emit8(M6502_CLC);
    m6502_lda_stk(0);
    cb_emit8_8(M6502_ADC_IMM, (u8)(d & 0xFF));
    m6502_sta_stk(0);
    m6502_lda_stk(1);
    cb_emit8_8(M6502_ADC_IMM, (u8)(d >> 8));
    m6502_sta_stk(1);
    return 1;
}

static int m6502_cmp_imm(IrOp op, i32 imm) {
    u16 rhs = (u16)(i16)imm;
    u8 lo = (u8)(rhs & 0xFF);
    u8 hi = (u8)(rhs >> 8);
    if (m6502_compact)
        return 0;
    switch (op) {
    case OP_EQ:
    case OP_NE:
        if (rhs == 0) {
            m6502_lda_stk(0);
            m6502_ora_stk(1);
        } else {
            m6502_lda_stk(0);
            cb_emit8_8(M6502_EOR_IMM, lo);
            cb_emit8_8(M6502_STA_ZP, zp_scratch);
            m6502_lda_stk(1);
            cb_emit8_8(M6502_EOR_IMM, hi);
            cb_emit8_8(M6502_ORA_ZP, zp_scratch);
        }
        m6502_cmp_true_op = (op == OP_EQ) ? M6502_BEQ : M6502_BNE;
        break;
    case OP_ULT:
    case OP_UGE:
        if (rhs == 0)
            return 0;
        cb_emit8(M6502_SEC);
        m6502_lda_stk(0);
        cb_emit8_8(M6502_SBC_IMM, lo);
        m6502_lda_stk(1);
        cb_emit8_8(M6502_SBC_IMM, hi);
        m6502_cmp_true_op = (op == OP_UGE) ? M6502_BCS : M6502_BCC;
        break;
    case OP_UGT:
    case OP_ULE:
        cb_emit8(M6502_SEC);
        cb_emit8_8(M6502_LDA_IMM, lo);
        m6502_sbc_stk(0);
        cb_emit8_8(M6502_LDA_IMM, hi);
        m6502_sbc_stk(1);
        m6502_cmp_true_op = (op == OP_ULE) ? M6502_BCS : M6502_BCC;
        break;
    case OP_LT:
    case OP_GE:
        if (rhs == 0) {
            m6502_lda_stk(1);
        } else {
            cb_emit8(M6502_SEC);
            m6502_lda_stk(0);
            cb_emit8_8(M6502_SBC_IMM, lo);
            m6502_lda_stk(1);
            cb_emit8_8(M6502_SBC_IMM, hi);
            cb_emit8_8(M6502_BVC, 0x02);
            cb_emit8_8(M6502_EOR_IMM, 0x80);
        }
        m6502_cmp_true_op = (op == OP_GE) ? M6502_BPL : M6502_BMI;
        break;
    case OP_GT:
    case OP_LE:
        cb_emit8(M6502_SEC);
        cb_emit8_8(M6502_LDA_IMM, lo);
        m6502_sbc_stk(0);
        cb_emit8_8(M6502_LDA_IMM, hi);
        m6502_sbc_stk(1);
        cb_emit8_8(M6502_BVC, 0x02);
        cb_emit8_8(M6502_EOR_IMM, 0x80);
        m6502_cmp_true_op = (op == OP_LE) ? M6502_BPL : M6502_BMI;
        break;
    default:
        return 0;
    }
    m6502_cmp_mat_pc = cb_pc();
    cb_emit8_8(m6502_cmp_true_op, 0x04); /* Bxx true */
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    cb_emit8_8(M6502_BEQ, 0x02); /* BEQ store */
    cb_emit8_8(M6502_LDA_IMM, 0xFF);
    m6502_sta_stk(0);
    m6502_sta_stk(1);
    m6502_cmp_pops = 1;
    m6502_cmp_end_pc = cb_pc();
    return 1;
}

static int m6502_shift_imm(IrOp op, u8 count) {
    u8 k;
    if (m6502_compact)
        return 0;
    if (count == 0)
        return 1;
    if (op != OP_LSHIFT && op != OP_RSHIFT && op != OP_ARSHIFT)
        return 0;
    if (count >= 8 && count <= 15) {
        u8 rem = (u8)(count - 8);
        if (op == OP_LSHIFT) {
            m6502_lda_stk(0);
            for (k = 0; k < rem; k++)
                cb_emit8(0x0A); /* ASL A */
            m6502_sta_stk(1);
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            m6502_sta_stk(0);
        } else if (op == OP_RSHIFT) {
            m6502_lda_stk(1);
            for (k = 0; k < rem; k++)
                cb_emit8(0x4A); /* LSR A */
            m6502_sta_stk(0);
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            m6502_sta_stk(1);
        } else {
            m6502_lda_stk(1);
            for (k = 0; k < rem; k++) {
                cb_emit8_8(M6502_CMP_IMM, 0x80);
                cb_emit8(0x6A); /* ROR A */
            }
            m6502_sta_stk(0);
            cb_emit8_8(M6502_BPL, 0x04);
            cb_emit8_8(M6502_LDA_IMM, 0xFF);
            cb_emit8_8(M6502_BMI, 0x02);
            cb_emit8_8(M6502_LDA_IMM, 0x00);
            m6502_sta_stk(1);
        }
        return 1;
    }
    if (count > 5)
        return 0;
    for (k = 0; k < count; k++) {
        if (op == OP_LSHIFT) {
            m6502_asl_stk(0);
            m6502_lda_stk(1);
            cb_emit8(M6502_ROL_A);
            m6502_sta_stk(1);
        } else if (op == OP_RSHIFT) {
            m6502_lsr_stk(1);
            m6502_ror_stk(0);
        } else {
            m6502_lda_stk(1);
            cb_emit8(0x0A); /* ASL A */
            m6502_ror_stk(1);
            m6502_ror_stk(0);
        }
    }
    return 1;
}

static void m6502_patch_call(size_t call_pc, size_t call_addr, size_t target_addr) {
    (void)call_addr;
    if (cb_is_banked() && m6502_bank_call_addr != 0) {
        const u8 *d = cb_data();
        if (d[call_pc] == M6502_JSR) {
            u16 jmp_t = (u16)d[call_pc + 1] | ((u16)d[call_pc + 2] << 8);
            if (jmp_t == m6502_bank_call_addr) {
                cb_patch16(call_pc + 4, (u16)target_addr);
                return;
            }
        }
    }
    cb_patch16(call_pc + 1, (u16)target_addr);
}

static size_t m6502_push_fn(size_t target_addr) {
    size_t pc;
    if (m6502_compact) {
        cb_emit8(BC_PUSH_W);
        pc = cb_pc();
        cb_emit16((u16)target_addr);
        return pc;
    }
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    pc = cb_pc();
    cb_emit8_8(M6502_LDA_IMM, (u8)(target_addr & 0xFF));
    m6502_sta_stk(0);
    cb_emit8_8(M6502_LDA_IMM, (u8)((target_addr >> 8) & 0xFF));
    m6502_sta_stk(1);
    return pc;
}

static void m6502_patch_fn(size_t patch_pc, size_t patch_addr, size_t target_addr) {
    u16 sta_len = (stack_zp_base != 0) ? 2 : 3;
    (void)patch_addr;
    if (m6502_compact) {
        bc_patch_fn_op(patch_pc, target_addr, 2);
        return;
    }
    cb_patch8((u16)(patch_pc + 1), (u8)(target_addr & 0xFF));
    cb_patch8((u16)(patch_pc + 2 + sta_len + 1), (u8)((target_addr >> 8) & 0xFF));
}

static void m6502_icall(void) {
    if (m6502_compact) {
        cb_emit8(BC_ICALL);
        return;
    }
    m6502_lda_stk(0);
    cb_emit8_8(M6502_STA_ZP, zp_scratch);
    m6502_lda_stk(1);
    cb_emit8_8(M6502_STA_ZP, (u8)(zp_scratch + 1));
    cb_emit8(M6502_INX);
    cb_emit8(M6502_INX);
    if (icall_addr == 0) {
        u16 skip_jmp;
        cb_emit8(M6502_JMP);
        skip_jmp = cb_pc();
        cb_emit16(0x0000);
        icall_addr = cb_cur_addr();
        cb_emit8_16(0x6C, (u16)zp_scratch); /* JMP (zp_scratch) */
        cb_patch16(skip_jmp, cb_cur_addr());
    }
    cb_emit8_16(M6502_JSR, icall_addr);
}

void be_6502_ensure_helpers(int need_mul, int need_div, int need_icall, int need_fp) {
    if (need_mul)
        ensure_mul16();
    if (need_div) {
        ensure_udivmod16();
        ensure_sdivmod16();
    }
    if (need_icall && icall_addr == 0) {
        u16 skip_jmp;
        cb_emit8(M6502_JMP);
        skip_jmp = cb_pc();
        cb_emit16(0x0000);
        icall_addr = cb_cur_addr();
        cb_emit8_16(0x6C, (u16)zp_scratch);
        cb_patch16(skip_jmp, cb_cur_addr());
    }
    if (need_fp)
        ensure_wozfp();
}

static void m6502_bc_local_op(LocalOp op, int slot, StackType type) {
    u8 sz = (type == TY_F || type == TY_I32) ? 4 : 2;
    u16 off = m6502_local_off.data[slot];
    if (op == LOCAL_ADD) {
        m6502_bc_local_op(LOCAL_GET, slot, type);
        cb_emit8(BC_ADD);
        m6502_bc_local_op(LOCAL_SET, slot, type);
        return;
    }
    if (m6502_static_frame) {
        u16 data_off = (u16)(m6502_static_base + off);
        if (op == LOCAL_ADDR)
            cb_emit8(BC_PUSH_W);
        else if (op == LOCAL_GET)
            cb_emit8(sz == 4 ? BC_STATIC_GET32 : BC_STATIC_GET);
        else
            cb_emit8(sz == 4 ? BC_STATIC_SET32 : BC_STATIC_SET);
        cb_add_data_reloc(cb_pc(), data_off);
        cb_emit16(0x0000);
    } else if (m6502_local_is_big.data[slot]) {
        cb_emit8_16(BC_LOCAL_ADDR_BIG, off);
    } else {
        if (op == LOCAL_ADDR)
            cb_emit8_8(BC_LOCAL_ADDR, (u8)off);
        else if (op == LOCAL_GET)
            cb_emit8_8(sz == 4 ? BC_LOCAL_GET32 : BC_LOCAL_GET, (u8)off);
        else
            cb_emit8_8(sz == 4 ? BC_LOCAL_SET32 : BC_LOCAL_SET, (u8)off);
    }
}

static void emit_vm_read_ip_a(u8 ip_lo, u8 ip_hi) {
    cb_emit8_8(M6502_LDY_IMM, 0x00);
    cb_emit8_8(M6502_LDA_INDY, ip_lo);
    cb_emit8_8(M6502_INC_ZP, ip_lo);
    cb_emit8_8(M6502_BNE, 0x02);
    cb_emit8_8(M6502_INC_ZP, ip_hi);
}

static void emit_vm_save_ip(u8 ip_lo, u8 ip_hi) {
    cb_emit8_8(M6502_LDA_ZP, ip_lo);
    cb_emit8(M6502_PHA);
    cb_emit8_8(M6502_LDA_ZP, ip_hi);
    cb_emit8(M6502_PHA);
}

static void ensure_6502_vm(void) {
    u8 ip_lo = zp_scratch;
    u8 ip_hi = (u8)(zp_scratch + 1);
    u8 tmp_lo = (u8)(zp_scratch + 2);
    u8 tmp_hi = (u8)(zp_scratch + 3);
    u16 skip_vm;
    u16 vm_next_addr;
    u16 vm_restore_ip_addr;
    u16 vm_call_tmp_addr;
    u16 tbl_lo_pc, tbl_hi_pc, vm_tbl_pc, vm_tbl_addr, bmi_imm7_pc;
    u16 handlers[BC_I322W + 1];
    size_t i;

    if (vm_enter_addr != 0)
        return;

    m6502_compact = 0;
    ensure_data_alloc();
    for (i = 0; i <= BC_I322W; i++)
        handlers[i] = 0;

    cb_emit8(M6502_JMP);
    skip_vm = cb_pc();
    cb_emit16(0x0000);

    /* vm_enter: pop return addr (fn+2), increment by 1 to point at bytecode */
    vm_enter_addr = cb_cur_addr();
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, ip_lo);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, ip_hi);
    cb_emit8_8(M6502_INC_ZP, ip_lo);
    cb_emit8_8(M6502_BNE, 0x02);
    cb_emit8_8(M6502_INC_ZP, ip_hi);

    /* vm_next: fetch opcode, dispatch via vm_tbl (0x80..0xFF push 0..127) */
    vm_next_addr = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_ORA_IMM, 0x00);
    cb_emit8(M6502_BMI);
    bmi_imm7_pc = cb_pc();
    cb_emit8(0x00);
    cb_emit8(0x0A); /* ASL A */
    cb_emit8(M6502_TAY);
    cb_emit8(0xB9); /* LDA vm_tbl,Y */
    tbl_lo_pc = cb_pc();
    cb_emit16(0x0000);
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    cb_emit8(0xB9); /* LDA vm_tbl+1,Y */
    tbl_hi_pc = cb_pc();
    cb_emit16(0x0000);
    cb_emit8_8(M6502_STA_ZP, tmp_hi);
    cb_emit8_16(0x6C, (u16)tmp_lo); /* JMP (tmp_lo) */

    /* vm_restore_ip */
    vm_restore_ip_addr = cb_cur_addr();
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, ip_hi);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, ip_lo);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* vm_call_tmp */
    vm_call_tmp_addr = cb_cur_addr();
    cb_emit8_16(0x6C, (u16)tmp_lo);

    /* push_imm7: A has 0x80..0xFF -> push (A & 0x7F) */
    cb_patch8(bmi_imm7_pc, (u8)(cb_pc() - (bmi_imm7_pc + 1)));
    cb_emit8_8(M6502_AND_IMM, 0x7F);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    m6502_sta_stk(0);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    m6502_sta_stk(1);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* Jump table */
    vm_tbl_pc = cb_pc();
    vm_tbl_addr = cb_cur_addr();
    for (i = 0; i <= BC_I322W; i++)
        cb_emit16(0x0000);
    cb_patch16(tbl_lo_pc, vm_tbl_addr);
    cb_patch16(tbl_hi_pc, (u16)(vm_tbl_addr + 1));

    /* BC_RET */
    handlers[BC_RET] = cb_cur_addr();
    cb_emit8(M6502_RTS);

    /* BC_ENTER (u8 frame_bytes) */
    handlers[BC_ENTER] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), lfp_data_off);
    cb_emit16(0x0000);
    cb_emit8(M6502_PHA);
    cb_emit8(M6502_SEC);
    cb_emit8_8(M6502_SBC_ZP, tmp_lo);
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), lfp_data_off);
    cb_emit16(0x0000);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_LEAVE */
    handlers[BC_LEAVE] = cb_cur_addr();
    cb_emit8(M6502_PLA);
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), lfp_data_off);
    cb_emit16(0x0000);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_ENTER_BIG (u16 big_bytes) */
    handlers[BC_ENTER_BIG] = cb_cur_addr();
    ensure_big_alloc();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_hi);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8_8(M6502_BNE, 10);
    cb_emit8_8(M6502_LDA_IMM, (u8)(m6502_ram_top & 0xFF));
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), bfp_data_off);
    cb_emit16(0x0000);
    cb_emit8_8(M6502_LDA_IMM, (u8)(m6502_ram_top >> 8));
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), bfp_data_off);
    cb_emit16(0x0000);
    cb_emit8(M6502_PHA);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8(M6502_PHA);
    cb_emit8(M6502_SEC);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), bfp_data_off);
    cb_emit16(0x0000);
    cb_emit8_8(M6502_SBC_ZP, tmp_lo);
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), bfp_data_off);
    cb_emit16(0x0000);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8_8(M6502_SBC_ZP, tmp_hi);
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_LEAVE_BIG */
    handlers[BC_LEAVE_BIG] = cb_cur_addr();
    cb_emit8(M6502_PLA);
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8(M6502_PLA);
    cb_emit8(M6502_STA_ABS);
    cb_add_data_reloc(cb_pc(), bfp_data_off);
    cb_emit16(0x0000);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_LOCAL_ADDR_BIG (u16 off) */
    handlers[BC_LOCAL_ADDR_BIG] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_hi);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_CLC);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), bfp_data_off);
    cb_emit16(0x0000);
    cb_emit8_8(0x65, tmp_lo); /* ADC tmp_lo */
    m6502_sta_stk(0);
    cb_emit8(M6502_LDA_ABS);
    cb_add_data_reloc(cb_pc(), (u16)(bfp_data_off + 1));
    cb_emit16(0x0000);
    cb_emit8_8(0x65, tmp_hi); /* ADC tmp_hi */
    m6502_sta_stk(1);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_PUSH_0_32 */
    handlers[BC_PUSH_0_32] = cb_cur_addr();
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    m6502_sta_stk(0);
    m6502_sta_stk(1);
    m6502_sta_stk(2);
    m6502_sta_stk(3);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_PUSH_I8 */
    handlers[BC_PUSH_I8] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    m6502_sta_stk(0);
    cb_emit8_8(M6502_ORA_IMM, 0x00); /* set N from A after DEX */
    cb_emit8_8(M6502_BPL, 0x04);
    cb_emit8_8(M6502_LDA_IMM, 0xFF);
    cb_emit8_8(M6502_BMI, 0x02);
    cb_emit8_8(M6502_LDA_IMM, 0x00);
    m6502_sta_stk(1);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_PUSH_W */
    handlers[BC_PUSH_W] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    m6502_sta_stk(0);
    emit_vm_read_ip_a(ip_lo, ip_hi);
    m6502_sta_stk(1);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_PUSH_I32 (also handles floats) */
    handlers[BC_PUSH_I32] = cb_cur_addr();
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    {
        u8 b;
        for (b = 0; b < 4; b++) {
            emit_vm_read_ip_a(ip_lo, ip_hi);
            m6502_sta_stk(b);
        }
    }
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_INC */
    handlers[BC_INC] = cb_cur_addr();
    emit_stack_op(0xFE, 0); /* INC stk+0,X (or zp,X) */
    cb_emit8_8(M6502_BNE, (stack_zp_base != 0) ? 2 : 3);
    emit_stack_op(0xFE, 1); /* INC stk+1,X */
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_DEC */
    handlers[BC_DEC] = cb_cur_addr();
    m6502_lda_stk(0);
    cb_emit8_8(M6502_BNE, (stack_zp_base != 0) ? 2 : 3);
    emit_stack_op(0xDE, 1); /* DEC stk+1,X */
    emit_stack_op(0xDE, 0); /* DEC stk+0,X */
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_ADD_IMM8 */
    handlers[BC_ADD_IMM8] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_LDY_IMM, 0x00);
    cb_emit8_8(M6502_ORA_IMM, 0x00);
    cb_emit8_8(M6502_BPL, 0x02);
    cb_emit8_8(M6502_LDY_IMM, 0xFF);
    cb_emit8(M6502_CLC);
    emit_stack_op(0x7D, 0); /* ADC stk+0,X */
    m6502_sta_stk(0);
    cb_emit8(M6502_TYA);
    emit_stack_op(0x7D, 1); /* ADC stk+1,X */
    m6502_sta_stk(1);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_STATIC_GET / GET32 / SET / SET32 */
    {
        int is_set, sz4;
        for (is_set = 0; is_set < 2; is_set++) {
            for (sz4 = 0; sz4 < 2; sz4++) {
                BcOp bc = is_set ? (sz4 ? BC_STATIC_SET32 : BC_STATIC_SET)
                                 : (sz4 ? BC_STATIC_GET32 : BC_STATIC_GET);
                u8 n = sz4 ? 4 : 2;
                u8 b;
                handlers[bc] = cb_cur_addr();
                emit_vm_read_ip_a(ip_lo, ip_hi);
                cb_emit8_8(M6502_STA_ZP, tmp_lo);
                emit_vm_read_ip_a(ip_lo, ip_hi);
                cb_emit8_8(M6502_STA_ZP, tmp_hi);
                if (!is_set) {
                    for (b = 0; b < n; b++)
                        cb_emit8(M6502_DEX);
                    for (b = 0; b < n; b++) {
                        cb_emit8_8(M6502_LDY_IMM, b);
                        cb_emit8_8(M6502_LDA_INDY, tmp_lo);
                        m6502_sta_stk(b);
                    }
                } else {
                    for (b = 0; b < n; b++) {
                        m6502_lda_stk(b);
                        cb_emit8_8(M6502_LDY_IMM, b);
                        cb_emit8_8(M6502_STA_INDY, tmp_lo);
                    }
                    for (b = 0; b < n; b++)
                        cb_emit8(M6502_INX);
                }
                cb_emit8_16(M6502_JMP, vm_next_addr);
            }
        }
    }

    /* BC_LOCAL_GET / GET32 / SET / SET32 */
    {
        int is_set, sz4;
        for (is_set = 0; is_set < 2; is_set++) {
            for (sz4 = 0; sz4 < 2; sz4++) {
                BcOp bc = is_set ? (sz4 ? BC_LOCAL_SET32 : BC_LOCAL_SET)
                                 : (sz4 ? BC_LOCAL_GET32 : BC_LOCAL_GET);
                u8 n = sz4 ? 4 : 2;
                u8 b;
                handlers[bc] = cb_cur_addr();
                emit_vm_read_ip_a(ip_lo, ip_hi);
                cb_emit8(M6502_CLC);
                cb_emit8(M6502_ADC_ABS);
                cb_add_data_reloc(cb_pc(), lfp_data_off);
                cb_emit16(0x0000);
                cb_emit8(M6502_TAY);
                if (!is_set) {
                    for (b = 0; b < n; b++)
                        cb_emit8(M6502_DEX);
                    for (b = 0; b < n; b++) {
                        m6502_lda_local(b);
                        m6502_sta_stk(b);
                    }
                } else {
                    for (b = 0; b < n; b++) {
                        m6502_lda_stk(b);
                        m6502_sta_local(b);
                    }
                    for (b = 0; b < n; b++)
                        cb_emit8(M6502_INX);
                }
                cb_emit8_16(M6502_JMP, vm_next_addr);
            }
        }
    }

    /* BC_LOCAL_ADDR (u8 off) */
    handlers[BC_LOCAL_ADDR] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8(M6502_CLC);
    cb_emit8(M6502_ADC_ABS);
    cb_add_data_reloc(cb_pc(), lfp_data_off);
    cb_emit16(0x0000);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_DEX);
    cb_emit8(M6502_CLC);
    cb_emit8(M6502_ADC_IMM);
    {
        u16 lo_pos = cb_pc();
        u16 hi_pos;
        cb_emit8(0x00);
        m6502_sta_stk(0);
        cb_emit8(M6502_LDA_IMM);
        hi_pos = cb_pc();
        cb_emit8(0x00);
        cb_emit8_8(M6502_ADC_IMM, 0x00);
        m6502_sta_stk(1);
        cb_add_data_reloc_split(lo_pos, hi_pos, locals_data_off);
    }
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_JMP (u16 target) */
    handlers[BC_JMP] = cb_cur_addr();
    cb_emit8_8(M6502_LDY_IMM, 0x00);
    cb_emit8_8(M6502_LDA_INDY, ip_lo);
    cb_emit8(M6502_PHA);
    cb_emit8(M6502_INY);
    cb_emit8_8(M6502_LDA_INDY, ip_lo);
    cb_emit8_8(M6502_STA_ZP, ip_hi);
    cb_emit8(M6502_PLA);
    cb_emit8_8(M6502_STA_ZP, ip_lo);
    cb_emit8_16(M6502_JMP, vm_next_addr);

    /* BC_JZ (u16 target) */
    handlers[BC_JZ] = cb_cur_addr();
    m6502_lda_stk(0);
    m6502_ora_stk(1);
    cb_emit8(M6502_PHP);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_PLP);
    cb_emit8_8(M6502_BEQ, (u8)(handlers[BC_JMP] ? 0 : 0));
    {
        u16 beq_pc = (u16)(cb_pc() - 1);
        u16 skip_len;
        /* Not zero: ip += 2 */
        cb_emit8(M6502_CLC);
        cb_emit8_8(M6502_LDA_ZP, ip_lo);
        cb_emit8_8(M6502_ADC_IMM, 0x02);
        cb_emit8_8(M6502_STA_ZP, ip_lo);
        cb_emit8_8(M6502_BCC, 0x02);
        cb_emit8_8(M6502_INC_ZP, ip_hi);
        cb_emit8_16(M6502_JMP, vm_next_addr);
        skip_len = (u16)(cb_pc() - (beq_pc + 1));
        cb_patch8(beq_pc, (u8)skip_len);
        cb_emit8_16(M6502_JMP, handlers[BC_JMP]);
    }

    /* BC_JTAB (u16[] targets) */
    handlers[BC_JTAB] = cb_cur_addr();
    m6502_lda_stk(0);
    cb_emit8(0x0A); /* ASL A */
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    m6502_lda_stk(1);
    cb_emit8(0x2A); /* ROL A */
    cb_emit8_8(M6502_STA_ZP, tmp_hi);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_CLC);
    cb_emit8_8(M6502_LDA_ZP, ip_lo);
    cb_emit8_8(0x65, tmp_lo); /* ADC tmp_lo */
    cb_emit8_8(M6502_STA_ZP, ip_lo);
    cb_emit8_8(M6502_LDA_ZP, ip_hi);
    cb_emit8_8(0x65, tmp_hi); /* ADC tmp_hi */
    cb_emit8_8(M6502_STA_ZP, ip_hi);
    cb_emit8_16(M6502_JMP, handlers[BC_JMP]);

    /* BC_CALL (u16 target) */
    handlers[BC_CALL] = cb_cur_addr();
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    emit_vm_read_ip_a(ip_lo, ip_hi);
    cb_emit8_8(M6502_STA_ZP, tmp_hi);
    emit_vm_save_ip(ip_lo, ip_hi);
    cb_emit8_16(M6502_JSR, vm_call_tmp_addr);
    cb_emit8_16(M6502_JMP, vm_restore_ip_addr);

    /* BC_ICALL */
    handlers[BC_ICALL] = cb_cur_addr();
    m6502_lda_stk(0);
    cb_emit8_8(M6502_STA_ZP, tmp_lo);
    m6502_lda_stk(1);
    cb_emit8_8(M6502_STA_ZP, tmp_hi);
    cb_emit8(M6502_INX);
    cb_emit8(M6502_INX);
    emit_vm_save_ip(ip_lo, ip_hi);
    cb_emit8_16(M6502_JSR, vm_call_tmp_addr);
    cb_emit8_16(M6502_JMP, vm_restore_ip_addr);

    /* Stack ops */
#define VM_STK(bc, op, t1, t2, save)                                                               \
    do {                                                                                           \
        handlers[bc] = cb_cur_addr();                                                              \
        if (save)                                                                                  \
            emit_vm_save_ip(ip_lo, ip_hi);                                                         \
        m6502_stack_op(op, t1, t2);                                                                \
        cb_emit8_16(M6502_JMP, (save) ? vm_restore_ip_addr : vm_next_addr);                           \
    } while (0)

    VM_STK(BC_DUP, OP_DUP, TY_W, TY_NONE, 0);
    VM_STK(BC_DUP32, OP_DUP, TY_I32, TY_NONE, 0);
    VM_STK(BC_DROP, OP_DROP, TY_W, TY_NONE, 0);
    VM_STK(BC_DROP32, OP_DROP, TY_I32, TY_NONE, 0);
    VM_STK(BC_SWAP, OP_SWAP, TY_W, TY_W, 0);
    VM_STK(BC_SWAP_W32, OP_SWAP, TY_I32, TY_W, 1);
    VM_STK(BC_SWAP_32W, OP_SWAP, TY_W, TY_I32, 1);
    VM_STK(BC_SWAP32, OP_SWAP, TY_I32, TY_I32, 0);
#undef VM_STK

    /* ALU / cmp / mem / conv ops */
#define VM_OP(bc, ir, save)                                                                        \
    do {                                                                                           \
        handlers[bc] = cb_cur_addr();                                                              \
        if (save)                                                                                  \
            emit_vm_save_ip(ip_lo, ip_hi);                                                         \
        m6502_op(ir);                                                                              \
        cb_emit8_16(M6502_JMP, (save) ? vm_restore_ip_addr : vm_next_addr);                           \
    } while (0)

    VM_OP(BC_ADD, OP_ADD, 0);
    VM_OP(BC_SUB, OP_SUB, 0);
    VM_OP(BC_MUL, OP_MUL, 1);
    VM_OP(BC_DIV, OP_DIV, 1);
    VM_OP(BC_MOD, OP_MOD, 1);
    VM_OP(BC_UDIV, OP_UDIV, 1);
    VM_OP(BC_UMOD, OP_UMOD, 1);
    VM_OP(BC_AND, OP_AND, 0);
    VM_OP(BC_OR, OP_OR, 0);
    VM_OP(BC_XOR, OP_XOR, 0);
    VM_OP(BC_INVERT, OP_INVERT, 0);
    VM_OP(BC_LSHIFT, OP_LSHIFT, 0);
    VM_OP(BC_ARSHIFT, OP_ARSHIFT, 0);
    VM_OP(BC_RSHIFT, OP_RSHIFT, 0);

    VM_OP(BC_FADD, OP_FADD, 1);
    VM_OP(BC_FSUB, OP_FSUB, 1);
    VM_OP(BC_FMUL, OP_FMUL, 1);
    VM_OP(BC_FDIV, OP_FDIV, 1);
    VM_OP(BC_FNEG, OP_FNEG, 1);

    VM_OP(BC_ADD32, OP_ADD32, 0);
    VM_OP(BC_SUB32, OP_SUB32, 0);
    VM_OP(BC_AND32, OP_AND32, 0);
    VM_OP(BC_OR32, OP_OR32, 0);
    VM_OP(BC_XOR32, OP_XOR32, 0);
    VM_OP(BC_INV32, OP_INV32, 0);

    VM_OP(BC_EQ, OP_EQ, 1);
    VM_OP(BC_NE, OP_NE, 1);
    VM_OP(BC_LT, OP_LT, 0);
    VM_OP(BC_GT, OP_GT, 0);
    VM_OP(BC_LE, OP_LE, 0);
    VM_OP(BC_GE, OP_GE, 0);
    VM_OP(BC_ULT, OP_ULT, 0);
    VM_OP(BC_UGT, OP_UGT, 0);
    VM_OP(BC_ULE, OP_ULE, 0);
    VM_OP(BC_UGE, OP_UGE, 0);

    VM_OP(BC_FEQ, OP_FEQ, 1);
    VM_OP(BC_FNE, OP_FNE, 1);
    VM_OP(BC_FLT, OP_FLT, 1);
    VM_OP(BC_FGT, OP_FGT, 1);

    VM_OP(BC_EQ32, OP_EQ32, 1);
    VM_OP(BC_NE32, OP_NE32, 1);
    VM_OP(BC_LT32, OP_LT32, 0);
    VM_OP(BC_GT32, OP_GT32, 0);
    VM_OP(BC_LE32, OP_LE32, 0);
    VM_OP(BC_GE32, OP_GE32, 0);
    VM_OP(BC_ULT32, OP_ULT32, 0);
    VM_OP(BC_UGT32, OP_UGT32, 0);
    VM_OP(BC_ULE32, OP_ULE32, 0);
    VM_OP(BC_UGE32, OP_UGE32, 0);

    VM_OP(BC_I8FETCH, OP_I8FETCH, 1);
    VM_OP(BC_U8FETCH, OP_U8FETCH, 1);
    VM_OP(BC_I8STORE, OP_I8STORE, 1);
    VM_OP(BC_FETCH, OP_FETCH, 1);
    VM_OP(BC_STORE, OP_STORE, 1);
    VM_OP(BC_FETCH32, OP_I32FETCH, 1);
    VM_OP(BC_STORE32, OP_I32STORE, 1);
    VM_OP(BC_BCOPY, OP_BCOPY, 1);

    VM_OP(BC_SEXT, OP_SEXT, 0);
    VM_OP(BC_W2F, OP_W2F, 1);
    VM_OP(BC_F2W, OP_F2W, 1);
    VM_OP(BC_W2I32, OP_W2I32, 0);
    VM_OP(BC_U2I32, OP_U2I32, 0);
    VM_OP(BC_I322W, OP_I322W, 0);
#undef VM_OP

    for (i = 0; i <= BC_I322W; i++) {
        cb_patch16((u16)(vm_tbl_pc + i * 2), handlers[i] ? handlers[i] : handlers[BC_RET]);
    }
    cb_patch16(skip_vm, cb_cur_addr());
    m6502_compact = 1;
}

void be_6502_emit_zero_bss(u8 zp_lo, u8 zp_hi) {
    size_t bss_off;
    size_t bss_sz;
    u16 lo_off, hi_off, loop_pc;

    (void)cb_alloc_data(0, 2);
    bss_off = cb_init_data_size();
    bss_sz = cb_data_size() - bss_off;
    if (bss_sz == 0) {
        return;
    }

    cb_emit8(M6502_LDA_IMM);
    lo_off = (u16)cb_pc();
    cb_emit8(0x00);
    cb_emit8_8(M6502_STA_ZP, zp_lo);
    cb_emit8(M6502_LDA_IMM);
    hi_off = (u16)cb_pc();
    cb_emit8(0x00);
    cb_emit8_8(M6502_STA_ZP, zp_hi);
    cb_add_data_reloc_split(lo_off, hi_off, bss_off);

    cb_emit8_8(M6502_LDA_IMM, 0x00);
    if ((bss_sz >> 8) > 0) {
        cb_emit8_8(M6502_LDX_IMM, (u8)(bss_sz >> 8));
        cb_emit8_8(M6502_LDY_IMM, 0x00);
        loop_pc = (u16)cb_pc();
        cb_emit8_8(M6502_STA_INDY, zp_lo);
        cb_emit8(M6502_INY);
        cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
        cb_emit8_8(M6502_INC_ZP, zp_hi);
        cb_emit8(M6502_DEX);
        cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
    }
    if ((bss_sz & 0xFF) > 0) {
        cb_emit8_8(M6502_LDY_IMM, 0x00);
        loop_pc = (u16)cb_pc();
        cb_emit8_8(M6502_STA_INDY, zp_lo);
        cb_emit8(M6502_INY);
        cb_emit8_8(0xC0, (u8)(bss_sz & 0xFF)); /* CPY #imm */
        cb_emit8_8(M6502_BNE, (u8)(loop_pc - (cb_pc() + 2)));
    }
}

const Backend be_6502 = {"6502",
                         2,
                         2,
                         4,
                         m6502_cur_addr,
                         m6502_fn_begin,
                         m6502_fn_end,
                         m6502_call,
                         m6502_tail_call,
                         m6502_call_extern,
                         m6502_push_w,
                         m6502_push_a,
                         m6502_push_f,
                         m6502_push_i32,
                         m6502_push_data,
                         m6502_local_op,
                         m6502_local_add_imm,
                         m6502_stack_op,
                         m6502_op,
                         m6502_add_imm,
                         m6502_cmp_imm,
                         m6502_shift_imm,
                         m6502_bit_imm,
                         m6502_mem_imm,
                         m6502_data_mem,
                         m6502_local_mem,
                         NULL,
                         m6502_emit_br_fwd,
                         m6502_emit_br_back,
                         NULL,
                         NULL,
                         m6502_patch_fwd,
                         m6502_emit_jtab,
                         m6502_patch_jtab,
                         m6502_patch_call,
                         m6502_push_fn,
                         m6502_patch_fn,
                         m6502_icall,
                         be_6502_stack_data_off,
                         be_6502_peep_fence};
