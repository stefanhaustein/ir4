/* sim65816.c - Standalone W65C816 simulator platform (--sim65816 --run) */
#include "backend/65816.h"
#include "codebuf.h"
#include "frontend.h"

#define SIM816_PUTC_ADDR 0x00FFF0UL
#define SIM816_MEM_SIZE  (2UL * 1024UL * 1024UL)

static size_t sim816_base = 0x010000UL;

static void sim816_begin(size_t base) {
    sim816_base = base;
    be_65816_reset();
    /* Reserve JML entry_addr at base; patched in sim816_end() */
    cb_emit8(M816_JML);
    cb_emit24(0);
}

static void sim816_emit_dp_data_addr(u8 dp, size_t data_off) {
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc(cb_pc(), data_off);
    cb_emit16(0);
    cb_emit8_8(M816_STA_DP, dp);
    cb_emit8_8(M816_SEP, 0x20);
    cb_emit8(M816_LDA_IMM);
    cb_add_data_reloc_bank(cb_pc(), data_off);
    cb_emit8(0);
    cb_emit8_8(M816_STA_DP, (u8)(dp + 2));
    cb_emit8_8(M816_REP, 0x20);
}

static void sim816_end(int main_idx) {
    const FnSig *sig = ir4_fn_sig(main_idx);
    size_t entry_addr = cb_cur_addr();
    size_t putchar_addr, print_u16_addr;
    size_t putc_call1 = 0, putc_call2 = 0, print_call = 0;

    if (sig->in_cnt != 0 || sig->out_cnt > 1) {
        fprintf(stderr, "-t sim65816 requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    cb_patch24(1, (u32)entry_addr);

    /* Switch to native mode (CLC; XCE), 16-bit A/X/Y (REP #$30), CLD */
    cb_emit8(M816_CLC);
    cb_emit8(M816_XCE);
    cb_emit8_8(M816_REP, 0x30);
    cb_emit8(M816_CLD);

    /* Hardware stack S = $1FFF */
    cb_emit8(M816_LDX_IMM);
    cb_emit16(0x1FFF);
    cb_emit8(M816_TXS);

    /* D = $0000, DBR = $00 */
    cb_emit8(M816_LDA_IMM);
    cb_emit16(0x0000);
    cb_emit8(M816_TCD);
    cb_emit8_8(M816_SEP, 0x20);
    cb_emit8_8(M816_LDA_IMM, 0x00);
    cb_emit8(M816_PHA);
    cb_emit8(M816_PLB);
    cb_emit8_8(M816_REP, 0x20);

    /* Zero BSS and init DP_LFP */
    be_65816_emit_zero_bss();
    sim816_emit_dp_data_addr(M816_DP_LFP, be_65816_rstack_off());

    /* Data stack X = M816_STACK_INIT_X */
    cb_emit8(M816_LDX_IMM);
    cb_emit16(M816_STACK_INIT_X);

    /* JSL main */
    cb_emit8(M816_JSL);
    cb_emit24((u32)sig->addr);

    if (sig->out_cnt == 1) {
        size_t pos_br;
        /* Pop 16-bit signed return value from $0200,X */
        cb_emit8(M816_LDA_ABSX);
        cb_emit16(M816_STACK_BASE);
        cb_emit8(M816_INX);
        cb_emit8(M816_INX);
        cb_emit8(M816_CMP_IMM);
        cb_emit16(0);
        pos_br = cb_pc();
        cb_emit8_8(M816_BPL, 0);
        cb_emit8(M816_PHA);
        cb_emit8(M816_LDA_IMM);
        cb_emit16('-');
        cb_emit8(M816_JSL);
        putc_call1 = cb_pc();
        cb_emit24(0);
        cb_emit8(M816_PLA);
        cb_emit8(M816_EOR_IMM);
        cb_emit16(0xFFFF);
        cb_emit8(M816_INC_A);
        cb_patch8(pos_br + 1, (u8)(cb_pc() - (pos_br + 2)));

        cb_emit8(M816_JSL);
        print_call = cb_pc();
        cb_emit24(0);

        cb_emit8(M816_LDA_IMM);
        cb_emit16('\n');
        cb_emit8(M816_JSL);
        putc_call2 = cb_pc();
        cb_emit24(0);
    }

    cb_emit8(M816_STP);

    /* putchar: writes low 8 bits of A to $00FFF0 */
    putchar_addr = cb_cur_addr();
    cb_emit8_8(M816_SEP, 0x20);
    cb_emit8(M816_STA_LONG);
    cb_emit24((u32)SIM816_PUTC_ADDR);
    cb_emit8_8(M816_REP, 0x20);
    cb_emit8(M816_RTL);

    /* print_u16: prints unsigned 16-bit A in decimal */
    {
        size_t nz_br, div_lp, skip_sub, pr_lp;
        print_u16_addr = cb_cur_addr();
        cb_emit8(M816_CMP_IMM);
        cb_emit16(0);
        nz_br = cb_pc();
        cb_emit8_8(M816_BNE, 0);
        cb_emit8(M816_LDA_IMM);
        cb_emit16('0');
        cb_emit8(M816_JSL);
        cb_emit24((u32)putchar_addr);
        cb_emit8(M816_RTL);
        cb_patch8(nz_br + 1, (u8)(cb_pc() - (nz_br + 2)));

        /* Push digits on hardware stack (count in Y) */
        cb_emit8(M816_LDY_IMM);
        cb_emit16(0);
        div_lp = cb_cur_addr();
        cb_emit8_8(M816_STA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_STZ_DP, M816_DP_TMP2);
        /* Divide DP_TMP0 by 10 using X? Preserve X! Use DP_PTR1 as bit counter */
        cb_emit8(M816_LDA_IMM);
        cb_emit16(16);
        cb_emit8_8(M816_STA_DP, M816_DP_PTR1);
        {
            size_t bit_lp = cb_cur_addr();
            cb_emit8_8(M816_ASL_DP, M816_DP_TMP0);
            cb_emit8_8(M816_ROL_DP, M816_DP_TMP2);
            cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
            cb_emit8(M816_SEC);
            cb_emit8(M816_SBC_IMM);
            cb_emit16(10);
            skip_sub = cb_pc();
            cb_emit8_8(M816_BCC, 0);
            cb_emit8_8(M816_STA_DP, M816_DP_TMP2);
            cb_emit8_8(M816_INC_DP, M816_DP_TMP0);
            cb_patch8(skip_sub + 1, (u8)(cb_pc() - (skip_sub + 2)));
            cb_emit8_8(M816_DEC_DP, M816_DP_PTR1);
            cb_emit8_8(M816_BNE, (u8)((i8)((i32)bit_lp - (i32)(cb_cur_addr() + 2))));
        }
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP2);
        cb_emit8(M816_CLC);
        cb_emit8(M816_ADC_IMM);
        cb_emit16('0');
        cb_emit8(M816_PHA);
        cb_emit8(M816_INY);
        cb_emit8_8(M816_LDA_DP, M816_DP_TMP0);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)div_lp - (i32)(cb_cur_addr() + 2))));

        pr_lp = cb_cur_addr();
        cb_emit8(M816_PLA);
        cb_emit8(M816_JSL);
        cb_emit24((u32)putchar_addr);
        cb_emit8(M816_DEY);
        cb_emit8_8(M816_BNE, (u8)((i8)((i32)pr_lp - (i32)(cb_cur_addr() + 2))));
        cb_emit8(M816_RTL);
    }

    if (putc_call1)
        cb_patch24(putc_call1, (u32)putchar_addr);
    if (putc_call2)
        cb_patch24(putc_call2, (u32)putchar_addr);
    if (print_call)
        cb_patch24(print_call, (u32)print_u16_addr);
}

static int sim816_write(FILE *fp, size_t base) {
    (void)base;
    return cb_write_raw(fp, 1);
}

/* ---- Minimal W65C816 CPU simulator for headless verification ---- */

typedef struct {
    u16 a, x, y, s, d, pc;
    u8 pbr, dbr, p, e;
    u8 *mem;
    size_t mem_sz;
    int halted;
    u32 trace_pc[256];
    u16 trace_s[256];
    u8 trace_op[256];
    u8 trace_idx;
} Cpu816;

#define F_C 0x01
#define F_Z 0x02
#define F_I 0x04
#define F_D 0x08
#define F_X 0x10
#define F_M 0x20
#define F_V 0x40
#define F_N 0x80

static u8 cpu_r8(Cpu816 *c, u32 addr) {
    addr &= 0xFFFFFFUL;
    if (addr >= c->mem_sz)
        return 0;
    return c->mem[addr];
}

static void cpu_w8(Cpu816 *c, u32 addr, u8 v) {
    addr &= 0xFFFFFFUL;
    if (addr == SIM816_PUTC_ADDR) {
        putchar(v);
        return;
    }
    if (addr < c->mem_sz)
        c->mem[addr] = v;
}

static u16 cpu_r16(Cpu816 *c, u32 addr) {
    return (u16)cpu_r8(c, addr) | ((u16)cpu_r8(c, addr + 1) << 8);
}

static void cpu_w16(Cpu816 *c, u32 addr, u16 v) {
    cpu_w8(c, addr, (u8)(v & 0xFF));
    cpu_w8(c, addr + 1, (u8)(v >> 8));
}

static u32 cpu_r24(Cpu816 *c, u32 addr) {
    return (u32)cpu_r8(c, addr) | ((u32)cpu_r8(c, addr + 1) << 8) |
           ((u32)cpu_r8(c, addr + 2) << 16);
}

static u8 fetch8(Cpu816 *c) {
    u8 v = cpu_r8(c, ((u32)c->pbr << 16) | c->pc);
    c->pc++;
    return v;
}

static u16 fetch16(Cpu816 *c) {
    u16 lo = fetch8(c);
    return lo | ((u16)fetch8(c) << 8);
}

static u32 fetch24(Cpu816 *c) {
    u32 lo = fetch16(c);
    return lo | ((u32)fetch8(c) << 16);
}

static void push8(Cpu816 *c, u8 v) { cpu_w8(c, c->s--, v); }
static u8 pop8(Cpu816 *c) { return cpu_r8(c, ++c->s); }
static void push16(Cpu816 *c, u16 v) {
    push8(c, (u8)(v >> 8));
    push8(c, (u8)(v & 0xFF));
}
static u16 pop16(Cpu816 *c) {
    u16 lo = pop8(c);
    return lo | ((u16)pop8(c) << 8);
}

static void set_nz8(Cpu816 *c, u8 v) {
    c->p = (u8)((c->p & ~(F_N | F_Z)) | (v == 0 ? F_Z : 0) | (v & 0x80 ? F_N : 0));
}

static void set_nz16(Cpu816 *c, u16 v) {
    c->p = (u8)((c->p & ~(F_N | F_Z)) | (v == 0 ? F_Z : 0) | (v & 0x8000 ? F_N : 0));
}

static void set_flag(Cpu816 *c, u8 f, int cond) {
    if (cond)
        c->p |= f;
    else
        c->p &= (u8)~f;
}

static void do_adc(Cpu816 *c, u32 addr) {
    if (c->p & F_M) {
        u8 v = cpu_r8(c, addr);
        u16 a = c->a & 0xFF;
        u16 res = a + v + (c->p & F_C ? 1 : 0);
        set_flag(c, F_C, res > 0xFF);
        set_flag(c, F_V, (~(a ^ v) & (a ^ res) & 0x80) != 0);
        c->a = (c->a & 0xFF00) | (res & 0xFF);
        set_nz8(c, (u8)res);
    } else {
        u16 v = cpu_r16(c, addr);
        u32 res = (u32)c->a + v + (c->p & F_C ? 1 : 0);
        set_flag(c, F_C, res > 0xFFFF);
        set_flag(c, F_V, (~(c->a ^ v) & (c->a ^ (u16)res) & 0x8000) != 0);
        c->a = (u16)res;
        set_nz16(c, c->a);
    }
}

static void do_sbc(Cpu816 *c, u32 addr) {
    if (c->p & F_M) {
        u8 v = (u8)~cpu_r8(c, addr);
        u16 a = c->a & 0xFF;
        u16 res = a + v + (c->p & F_C ? 1 : 0);
        set_flag(c, F_C, res > 0xFF);
        set_flag(c, F_V, (~(a ^ v) & (a ^ res) & 0x80) != 0);
        c->a = (c->a & 0xFF00) | (res & 0xFF);
        set_nz8(c, (u8)res);
    } else {
        u16 v = (u16)~cpu_r16(c, addr);
        u32 res = (u32)c->a + v + (c->p & F_C ? 1 : 0);
        set_flag(c, F_C, res > 0xFFFF);
        set_flag(c, F_V, (~(c->a ^ v) & (c->a ^ (u16)res) & 0x8000) != 0);
        c->a = (u16)res;
        set_nz16(c, c->a);
    }
}

static void do_cmp(Cpu816 *c, u16 lhs, u32 addr, int is8) {
    if (is8) {
        u8 rhs = cpu_r8(c, addr);
        u8 l = (u8)(lhs & 0xFF);
        set_flag(c, F_C, l >= rhs);
        set_nz8(c, (u8)(l - rhs));
    } else {
        u16 rhs = cpu_r16(c, addr);
        set_flag(c, F_C, lhs >= rhs);
        set_nz16(c, (u16)(lhs - rhs));
    }
}

static void do_branch(Cpu816 *c, int cond) {
    i8 rel = (i8)fetch8(c);
    if (cond)
        c->pc = (u16)(c->pc + rel);
}

static u32 addr_dp(Cpu816 *c) { return (u32)((c->d + fetch8(c)) & 0xFFFF); }
static u32 addr_idl(Cpu816 *c) { return cpu_r24(c, addr_dp(c)); }
static u32 addr_idly(Cpu816 *c) { return (cpu_r24(c, addr_dp(c)) + c->y) & 0xFFFFFFUL; }
static u32 addr_abs(Cpu816 *c) { return ((u32)c->dbr << 16) | fetch16(c); }
static u32 addr_absx(Cpu816 *c) { return (((u32)c->dbr << 16) + fetch16(c) + c->x) & 0xFFFFFFUL; }
static u32 addr_absy(Cpu816 *c) { return (((u32)c->dbr << 16) + fetch16(c) + c->y) & 0xFFFFFFUL; }
static u32 addr_long(Cpu816 *c) { return fetch24(c); }
static u32 addr_longx(Cpu816 *c) { return (fetch24(c) + c->x) & 0xFFFFFFUL; }

static void sim816_step(Cpu816 *c) {
    u32 cur = ((u32)c->pbr << 16) | c->pc;
    u8 op = fetch8(c);
    int m8 = (c->p & F_M) != 0;
    int x8 = (c->p & F_X) != 0;
    u32 ea;
    c->trace_pc[c->trace_idx] = cur;
    c->trace_s[c->trace_idx] = c->s;
    c->trace_op[c->trace_idx] = op;
    c->trace_idx++;
    switch (op) {
    case 0x18: c->p &= (u8)~F_C; break; /* CLC */
    case 0x38: c->p |= F_C; break;       /* SEC */
    case 0xD8: c->p &= (u8)~F_D; break; /* CLD */
    case 0xFB: { /* XCE */
        u8 old_c = c->p & F_C;
        set_flag(c, F_C, c->e);
        c->e = old_c ? 1 : 0;
        if (c->e) { c->p |= F_M | F_X; c->x &= 0xFF; c->y &= 0xFF; }
        break;
    }
    case 0xC2: c->p &= (u8)~fetch8(c); break; /* REP */
    case 0xE2: {
        c->p |= fetch8(c);
        if (c->p & F_X) { c->x &= 0xFF; c->y &= 0xFF; }
        break;
    }
    case 0xEB: /* XBA */
        c->a = (u16)((c->a >> 8) | (c->a << 8));
        set_nz8(c, (u8)(c->a & 0xFF));
        break;
    case 0x48: if (m8) push8(c, (u8)c->a); else push16(c, c->a); break; /* PHA */
    case 0x68: if (m8) { c->a = (c->a & 0xFF00) | pop8(c); set_nz8(c, (u8)c->a); } else { c->a = pop16(c); set_nz16(c, c->a); } break; /* PLA */
    case 0xDA: if (x8) push8(c, (u8)c->x); else push16(c, c->x); break; /* PHX */
    case 0xFA: if (x8) { c->x = pop8(c); set_nz8(c, (u8)c->x); } else { c->x = pop16(c); set_nz16(c, c->x); } break; /* PLX */
    case 0x5A: if (x8) push8(c, (u8)c->y); else push16(c, c->y); break; /* PHY */
    case 0x7A: if (x8) { c->y = pop8(c); set_nz8(c, (u8)c->y); } else { c->y = pop16(c); set_nz16(c, c->y); } break; /* PLY */
    case 0x8B: push8(c, c->dbr); break; /* PHB */
    case 0xAB: c->dbr = pop8(c); set_nz8(c, c->dbr); break; /* PLB */
    case 0x4B: push8(c, c->pbr); break; /* PHK */
    case 0xF4: push16(c, fetch16(c)); break; /* PEA */
    case 0xAA: c->x = x8 ? (c->a & 0xFF) : c->a; if (x8) set_nz8(c, (u8)c->x); else set_nz16(c, c->x); break; /* TAX */
    case 0x8A: if (m8) { c->a = (c->a & 0xFF00) | (c->x & 0xFF); set_nz8(c, (u8)c->a); } else { c->a = c->x; set_nz16(c, c->a); } break; /* TXA */
    case 0xA8: c->y = x8 ? (c->a & 0xFF) : c->a; if (x8) set_nz8(c, (u8)c->y); else set_nz16(c, c->y); break; /* TAY */
    case 0x98: if (m8) { c->a = (c->a & 0xFF00) | (c->y & 0xFF); set_nz8(c, (u8)c->a); } else { c->a = c->y; set_nz16(c, c->a); } break; /* TYA */
    case 0x9A: c->s = c->x; break; /* TXS */
    case 0xBA: c->x = x8 ? (c->s & 0xFF) : c->s; if (x8) set_nz8(c, (u8)c->x); else set_nz16(c, c->x); break; /* TSX */
    case 0x5B: c->d = c->a; set_nz16(c, c->d); break; /* TCD */
    case 0x7B: c->a = c->d; set_nz16(c, c->a); break; /* TDC */
    case 0xE8: c->x = x8 ? ((c->x + 1) & 0xFF) : (u16)(c->x + 1); if (x8) set_nz8(c, (u8)c->x); else set_nz16(c, c->x); break;
    case 0xCA: c->x = x8 ? ((c->x - 1) & 0xFF) : (u16)(c->x - 1); if (x8) set_nz8(c, (u8)c->x); else set_nz16(c, c->x); break;
    case 0xC8: c->y = x8 ? ((c->y + 1) & 0xFF) : (u16)(c->y + 1); if (x8) set_nz8(c, (u8)c->y); else set_nz16(c, c->y); break;
    case 0x88: c->y = x8 ? ((c->y - 1) & 0xFF) : (u16)(c->y - 1); if (x8) set_nz8(c, (u8)c->y); else set_nz16(c, c->y); break;

    /* LDA */
    case 0xA9: if (m8) { c->a = (c->a & 0xFF00) | fetch8(c); set_nz8(c, (u8)c->a); } else { c->a = fetch16(c); set_nz16(c, c->a); } break;
    case 0xA5: ea = addr_dp(c); goto do_lda;
    case 0xA7: ea = addr_idl(c); goto do_lda;
    case 0xB7: ea = addr_idly(c); goto do_lda;
    case 0xAD: ea = addr_abs(c); goto do_lda;
    case 0xBD: ea = addr_absx(c); goto do_lda;
    case 0xB9: ea = addr_absy(c); goto do_lda;
    case 0xAF: ea = addr_long(c); goto do_lda;
    case 0xBF: ea = addr_longx(c);
    do_lda:
        if (m8) { c->a = (c->a & 0xFF00) | cpu_r8(c, ea); set_nz8(c, (u8)c->a); }
        else { c->a = cpu_r16(c, ea); set_nz16(c, c->a); }
        break;

    /* LDX / LDY */
    case 0xA2: if (x8) { c->x = fetch8(c); set_nz8(c, (u8)c->x); } else { c->x = fetch16(c); set_nz16(c, c->x); } break;
    case 0xA0: if (x8) { c->y = fetch8(c); set_nz8(c, (u8)c->y); } else { c->y = fetch16(c); set_nz16(c, c->y); } break;
    case 0xA6: ea = addr_dp(c); if (x8) { c->x = cpu_r8(c, ea); set_nz8(c, (u8)c->x); } else { c->x = cpu_r16(c, ea); set_nz16(c, c->x); } break;
    case 0xA4: ea = addr_dp(c); if (x8) { c->y = cpu_r8(c, ea); set_nz8(c, (u8)c->y); } else { c->y = cpu_r16(c, ea); set_nz16(c, c->y); } break;

    /* STA */
    case 0x85: ea = addr_dp(c); goto do_sta;
    case 0x87: ea = addr_idl(c); goto do_sta;
    case 0x97: ea = addr_idly(c); goto do_sta;
    case 0x8D: ea = addr_abs(c); goto do_sta;
    case 0x9D: ea = addr_absx(c); goto do_sta;
    case 0x8F: ea = addr_long(c); goto do_sta;
    case 0x9F: ea = addr_longx(c);
    do_sta:
        if (m8) cpu_w8(c, ea, (u8)c->a); else cpu_w16(c, ea, c->a);
        break;

    /* STX / STY / STZ */
    case 0x86: ea = addr_dp(c); if (x8) cpu_w8(c, ea, (u8)c->x); else cpu_w16(c, ea, c->x); break;
    case 0x84: ea = addr_dp(c); if (x8) cpu_w8(c, ea, (u8)c->y); else cpu_w16(c, ea, c->y); break;
    case 0x64: ea = addr_dp(c); if (m8) cpu_w8(c, ea, 0); else cpu_w16(c, ea, 0); break;
    case 0x9C: ea = addr_abs(c); if (m8) cpu_w8(c, ea, 0); else cpu_w16(c, ea, 0); break;
    case 0x9E: ea = addr_absx(c); if (m8) cpu_w8(c, ea, 0); else cpu_w16(c, ea, 0); break;

    /* ADC / SBC */
    case 0x69: ea = ((u32)c->pbr << 16) | c->pc; c->pc += m8 ? 1 : 2; do_adc(c, ea); break;
    case 0x65: do_adc(c, addr_dp(c)); break;
    case 0x67: do_adc(c, addr_idl(c)); break;
    case 0x77: do_adc(c, addr_idly(c)); break;
    case 0x7D: do_adc(c, addr_absx(c)); break;
    case 0x6F: do_adc(c, addr_long(c)); break;
    case 0xE9: ea = ((u32)c->pbr << 16) | c->pc; c->pc += m8 ? 1 : 2; do_sbc(c, ea); break;
    case 0xE5: do_sbc(c, addr_dp(c)); break;
    case 0xE7: do_sbc(c, addr_idl(c)); break;
    case 0xF7: do_sbc(c, addr_idly(c)); break;
    case 0xFD: do_sbc(c, addr_absx(c)); break;

    /* AND / ORA / EOR */
    case 0x29: ea = ((u32)c->pbr << 16) | c->pc; c->pc += m8 ? 1 : 2; goto do_and;
    case 0x25: ea = addr_dp(c); goto do_and;
    case 0x3D: ea = addr_absx(c);
    do_and:
        if (m8) { c->a = (c->a & 0xFF00) | ((c->a & cpu_r8(c, ea)) & 0xFF); set_nz8(c, (u8)c->a); }
        else { c->a &= cpu_r16(c, ea); set_nz16(c, c->a); }
        break;
    case 0x09: ea = ((u32)c->pbr << 16) | c->pc; c->pc += m8 ? 1 : 2; goto do_ora;
    case 0x05: ea = addr_dp(c); goto do_ora;
    case 0x1D: ea = addr_absx(c);
    do_ora:
        if (m8) { c->a = (c->a & 0xFF00) | ((c->a | cpu_r8(c, ea)) & 0xFF); set_nz8(c, (u8)c->a); }
        else { c->a |= cpu_r16(c, ea); set_nz16(c, c->a); }
        break;
    case 0x49: ea = ((u32)c->pbr << 16) | c->pc; c->pc += m8 ? 1 : 2; goto do_eor;
    case 0x45: ea = addr_dp(c); goto do_eor;
    case 0x5D: ea = addr_absx(c);
    do_eor:
        if (m8) { c->a = (c->a & 0xFF00) | ((c->a ^ cpu_r8(c, ea)) & 0xFF); set_nz8(c, (u8)c->a); }
        else { c->a ^= cpu_r16(c, ea); set_nz16(c, c->a); }
        break;

    /* CMP / CPX / CPY */
    case 0xC9: ea = ((u32)c->pbr << 16) | c->pc; c->pc += m8 ? 1 : 2; do_cmp(c, c->a, ea, m8); break;
    case 0xC5: do_cmp(c, c->a, addr_dp(c), m8); break;
    case 0xC7: do_cmp(c, c->a, addr_idl(c), m8); break;
    case 0xD7: do_cmp(c, c->a, addr_idly(c), m8); break;
    case 0xDD: do_cmp(c, c->a, addr_absx(c), m8); break;
    case 0xE0: ea = ((u32)c->pbr << 16) | c->pc; c->pc += x8 ? 1 : 2; do_cmp(c, c->x, ea, x8); break;
    case 0xE4: do_cmp(c, c->x, addr_dp(c), x8); break;
    case 0xC0: ea = ((u32)c->pbr << 16) | c->pc; c->pc += x8 ? 1 : 2; do_cmp(c, c->y, ea, x8); break;
    case 0xC4: do_cmp(c, c->y, addr_dp(c), x8); break;

    /* INC / DEC / Shifts */
    case 0x1A: if (m8) { u8 v = (u8)(c->a + 1); c->a = (c->a & 0xFF00) | v; set_nz8(c, v); } else { c->a++; set_nz16(c, c->a); } break;
    case 0x3A: if (m8) { u8 v = (u8)(c->a - 1); c->a = (c->a & 0xFF00) | v; set_nz8(c, v); } else { c->a--; set_nz16(c, c->a); } break;
    case 0xE6: ea = addr_dp(c); if (m8) { u8 v = (u8)(cpu_r8(c, ea) + 1); cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = (u16)(cpu_r16(c, ea) + 1); cpu_w16(c, ea, v); set_nz16(c, v); } break;
    case 0xC6: ea = addr_dp(c); if (m8) { u8 v = (u8)(cpu_r8(c, ea) - 1); cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = (u16)(cpu_r16(c, ea) - 1); cpu_w16(c, ea, v); set_nz16(c, v); } break;
    case 0x0A:
        if (m8) { u8 v = (u8)c->a; set_flag(c, F_C, v & 0x80); v <<= 1; c->a = (c->a & 0xFF00) | v; set_nz8(c, v); }
        else { set_flag(c, F_C, c->a & 0x8000); c->a <<= 1; set_nz16(c, c->a); }
        break;
    case 0x4A:
        if (m8) { u8 v = (u8)c->a; set_flag(c, F_C, v & 1); v >>= 1; c->a = (c->a & 0xFF00) | v; set_nz8(c, v); }
        else { set_flag(c, F_C, c->a & 1); c->a >>= 1; set_nz16(c, c->a); }
        break;
    case 0x2A: {
        u8 oc = (c->p & F_C) ? 1 : 0;
        if (m8) { u8 v = (u8)c->a; set_flag(c, F_C, v & 0x80); v = (u8)((v << 1) | oc); c->a = (c->a & 0xFF00) | v; set_nz8(c, v); }
        else { set_flag(c, F_C, c->a & 0x8000); c->a = (u16)((c->a << 1) | oc); set_nz16(c, c->a); }
        break;
    }
    case 0x6A: {
        u8 oc = (c->p & F_C) ? 1 : 0;
        if (m8) { u8 v = (u8)c->a; set_flag(c, F_C, v & 1); v = (u8)((v >> 1) | (oc << 7)); c->a = (c->a & 0xFF00) | v; set_nz8(c, v); }
        else { set_flag(c, F_C, c->a & 1); c->a = (u16)((c->a >> 1) | (oc << 15)); set_nz16(c, c->a); }
        break;
    }
    case 0x06: ea = addr_dp(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 0x80); v <<= 1; cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 0x8000); v <<= 1; cpu_w16(c, ea, v); set_nz16(c, v); } break;
    case 0x46: ea = addr_dp(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 1); v >>= 1; cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 1); v >>= 1; cpu_w16(c, ea, v); set_nz16(c, v); } break;
    case 0x26: { u8 oc = (c->p & F_C) ? 1 : 0; ea = addr_dp(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 0x80); v = (u8)((v << 1) | oc); cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 0x8000); v = (u16)((v << 1) | oc); cpu_w16(c, ea, v); set_nz16(c, v); } break; }
    case 0x66: { u8 oc = (c->p & F_C) ? 1 : 0; ea = addr_dp(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 1); v = (u8)((v >> 1) | (oc << 7)); cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 1); v = (u16)((v >> 1) | (oc << 15)); cpu_w16(c, ea, v); set_nz16(c, v); } break; }
    case 0x1E: ea = addr_absx(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 0x80); v <<= 1; cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 0x8000); v <<= 1; cpu_w16(c, ea, v); set_nz16(c, v); } break;
    case 0x5E: ea = addr_absx(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 1); v >>= 1; cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 1); v >>= 1; cpu_w16(c, ea, v); set_nz16(c, v); } break;
    case 0x3E: { u8 oc = (c->p & F_C) ? 1 : 0; ea = addr_absx(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 0x80); v = (u8)((v << 1) | oc); cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 0x8000); v = (u16)((v << 1) | oc); cpu_w16(c, ea, v); set_nz16(c, v); } break; }
    case 0x7E: { u8 oc = (c->p & F_C) ? 1 : 0; ea = addr_absx(c); if (m8) { u8 v = cpu_r8(c, ea); set_flag(c, F_C, v & 1); v = (u8)((v >> 1) | (oc << 7)); cpu_w8(c, ea, v); set_nz8(c, v); } else { u16 v = cpu_r16(c, ea); set_flag(c, F_C, v & 1); v = (u16)((v >> 1) | (oc << 15)); cpu_w16(c, ea, v); set_nz16(c, v); } break; }

    /* Branches & jumps */
    case 0x80: do_branch(c, 1); break;
    case 0x82: { i16 rel = (i16)fetch16(c); c->pc = (u16)(c->pc + rel); break; }
    case 0x10: do_branch(c, !(c->p & F_N)); break;
    case 0x30: do_branch(c, c->p & F_N); break;
    case 0x50: do_branch(c, !(c->p & F_V)); break;
    case 0x70: do_branch(c, c->p & F_V); break;
    case 0x90: do_branch(c, !(c->p & F_C)); break;
    case 0xB0: do_branch(c, c->p & F_C); break;
    case 0xD0: do_branch(c, !(c->p & F_Z)); break;
    case 0xF0: do_branch(c, c->p & F_Z); break;
    case 0x4C: c->pc = fetch16(c); break;
    case 0x5C: { u32 a = fetch24(c); c->pc = (u16)(a & 0xFFFF); c->pbr = (u8)(a >> 16); break; }
    case 0xDC: { u16 dp = fetch16(c); u32 a = cpu_r24(c, dp); c->pc = (u16)(a & 0xFFFF); c->pbr = (u8)(a >> 16); break; }
    case 0x20: { u16 t = fetch16(c); push16(c, (u16)(c->pc - 1)); c->pc = t; break; }
    case 0x60: c->pc = (u16)(pop16(c) + 1); break;
    case 0x22: {
        u32 t = fetch24(c);
        push8(c, c->pbr);
        push16(c, (u16)(c->pc - 1));
        c->pc = (u16)(t & 0xFFFF);
        c->pbr = (u8)(t >> 16);
        break;
    }
    case 0x6B: {
        u16 lo = pop16(c);
        c->pbr = pop8(c);
        c->pc = (u16)(lo + 1);
        break;
    }
    case 0xDB: c->halted = 1; break;
    default: {
        int ti;
        fprintf(stderr, "sim65816: unimplemented opcode 0x%02X at %02X:%04X (S=%04X X=%04X A=%04X P=%02X)\n",
                op, c->pbr, (u16)(c->pc - 1), c->s, c->x, c->a, c->p);
        for (ti = 0; ti < 256; ti++) {
            u8 k = (u8)(c->trace_idx + ti);
            fprintf(stderr, "  %02X:%04X op=%02X S=%04X\n",
                    (u8)(c->trace_pc[k] >> 16), (u16)(c->trace_pc[k] & 0xFFFF),
                    c->trace_op[k], c->trace_s[k]);
        }
        exit(1);
    }
    }
}

static void sim816_run(int main_idx) {
    Cpu816 cpu;
    const u8 *code = cb_data();
    const u8 *idata = cb_init_data();
    size_t code_sz = cb_pc();
    size_t data_sz = cb_init_data_size();
    u32 data_base = (u32)(sim816_base + code_sz);
    unsigned long steps = 0;
    int trace = getenv("SIM816_TRACE") != NULL;

    (void)main_idx;
    memset(&cpu, 0, sizeof(cpu));
    cpu.mem_sz = SIM816_MEM_SIZE;
    cpu.mem = (u8 *)calloc(1, SIM816_MEM_SIZE);
    if (!cpu.mem)
        ir4_die("sim65816: out of memory");

    if (sim816_base + code_sz + cb_data_size() > SIM816_MEM_SIZE)
        ir4_die("sim65816: program exceeds 512KB simulator RAM");

    memcpy(cpu.mem + sim816_base, code, code_sz);
    if (data_sz > 0)
        memcpy(cpu.mem + data_base, idata, data_sz);

    cpu.e = 1;
    cpu.p = F_M | F_X | F_I;
    cpu.s = 0x01FF;
    cpu.pbr = (u8)(sim816_base >> 16);
    cpu.pc = (u16)(sim816_base & 0xFFFF);

    while (!cpu.halted) {
        if (trace) {
            fprintf(stderr, "%02X:%04X op=%02X A=%04X X=%04X Y=%04X S=%04X P=%02X\n",
                    cpu.pbr, cpu.pc, cpu_r8(&cpu, ((u32)cpu.pbr << 16) | cpu.pc),
                    cpu.a, cpu.x, cpu.y, cpu.s, cpu.p);
        }
        sim816_step(&cpu);
        if (++steps > 500000000UL) {
            int ti;
            fprintf(stderr, "sim65816: cycle limit exceeded at %02X:%04X (S=%04X X=%04X A=%04X P=%02X)\n",
                    cpu.pbr, cpu.pc, cpu.s, cpu.x, cpu.a, cpu.p);
            for (ti = 0; ti < 16; ti++) {
                u8 k = (u8)((cpu.trace_idx + ti) & 15);
                fprintf(stderr, "  %02X:%04X op=%02X\n",
                        (u8)(cpu.trace_pc[k] >> 16), (u16)(cpu.trace_pc[k] & 0xFFFF), cpu.trace_op[k]);
            }
            free(cpu.mem);
            exit(1);
        }
    }
    free(cpu.mem);
}

const Platform plat_65816_sim = {"--sim65816", &be_65816, 0x010000UL, "sim65816", true,
                                 sim816_begin, sim816_end, sim816_write, sim816_run, "bin"};
