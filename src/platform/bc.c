/* bc.c - Portable C16-bit Bytecode VM Runner for IR4th */
#include "backend/bc.h"
#include "codebuf.h"
#include "frontend.h"
#include "plat_web_vm.h"

VEC_OF(U8Vec, u8);

#define VM_MEM_SIZE 65536
#define VM_STACK_TOP 0xFF00
#define VM_FRAME_TOP 0xE000
#define VM_CALL_MAX 1024

static u16 rd16(const u8 *m, u16 addr) { return (u16)m[addr] | ((u16)m[(u16)(addr + 1)] << 8); }

static void wr16(u8 *m, u16 addr, u16 val) {
    m[addr] = (u8)(val & 0xFF);
    m[(u16)(addr + 1)] = (u8)((val >> 8) & 0xFF);
}

static u32 rd32(const u8 *m, u16 addr) {
    return (u32)rd16(m, addr) | ((u32)rd16(m, (u16)(addr + 2)) << 16);
}

static void wr32(u8 *m, u16 addr, u32 val) {
    wr16(m, addr, (u16)(val & 0xFFFF));
    wr16(m, (u16)(addr + 2), (u16)(val >> 16));
}

static float rd_f(const u8 *m, u16 addr) {
    u32 bits = rd32(m, addr);
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void wr_f(u8 *m, u16 addr, float f) {
    u32 bits;
    memcpy(&bits, &f, 4);
    wr32(m, addr, bits);
}

static void bc_run_vm(size_t main_idx) {
    const FuncSig *sig = ir4_func_sig(main_idx);
    u8 *mem;
    u16 call_stack[VM_CALL_MAX];
    size_t csp = 0;
    u16 sp = VM_STACK_TOP;
    u16 fp = VM_FRAME_TOP;
    u16 pc;
    u16 base;
    size_t code_sz = cb_pc();
    size_t init_sz = cb_init_data_size();
    u16 data_addr;

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--run requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    mem = (u8 *)calloc(1, VM_MEM_SIZE);
    if (!mem) {
        perror("calloc vm mem");
        exit(1);
    }

    base = (u16)(cb_cur_addr() - code_sz);
    if (code_sz > 0)
        memcpy(mem + base, cb_data(), code_sz);
    data_addr = (u16)(base + code_sz);
    if (init_sz > 0)
        memcpy(mem + data_addr, cb_init_data(), init_sz);

    pc = (u16)sig->addr;
    call_stack[csp++] = 0xFFFF; /* sentinel return address */

    for (;;) {
        u8 op = mem[pc++];
        if (op & BC_PUSH_IMM7_BASE) {
            sp -= 2;
            wr16(mem, sp, (u16)(op & 0x7F));
            continue;
        }
        switch ((BcOp)op) {
        case BC_RET:
            if (csp == 0) {
                fprintf(stderr, "VM call stack underflow\n");
                free(mem);
                exit(1);
            }
            pc = call_stack[--csp];
            if (pc == 0xFFFF) {
                i16 res = 0;
                if (sig->out_types.len == 1)
                    res = (i16)rd16(mem, sp);
                free(mem);
                printf("VM main() returned: %d\n", (int)res);
                return;
            }
            break;

        case BC_ENTER: {
            u16 sz = rd16(mem, pc);
            pc += 2;
            fp -= sz;
            break;
        }
        case BC_LEAVE: {
            u16 sz = rd16(mem, pc);
            pc += 2;
            fp += sz;
            break;
        }
        case BC_ENTER_BIG:
        case BC_LEAVE_BIG:
        case BC_LOCAL_ADDR_BIG:
            break;

        case BC_PUSH_0_32:
            sp -= 4;
            wr32(mem, sp, 0);
            break;
        case BC_PUSH_I8: {
            i16 v = (i8)mem[pc++];
            sp -= 2;
            wr16(mem, sp, (u16)v);
            break;
        }
        case BC_PUSH_W:
        case BC_PUSH_DATA:
        case BC_PUSH_FN: {
            u16 v = rd16(mem, pc);
            pc += 2;
            sp -= 2;
            wr16(mem, sp, v);
            break;
        }
        case BC_PUSH_I32:
        case BC_PUSH_F: {
            u32 v = rd32(mem, pc);
            pc += 4;
            sp -= 4;
            wr32(mem, sp, v);
            break;
        }

        case BC_INC:
            wr16(mem, sp, (u16)(rd16(mem, sp) + 1));
            break;
        case BC_DEC:
            wr16(mem, sp, (u16)(rd16(mem, sp) - 1));
            break;
        case BC_ADD_IMM8: {
            i16 d = (i8)mem[pc++];
            wr16(mem, sp, (u16)(rd16(mem, sp) + (u16)d));
            break;
        }

        case BC_STATIC_GET: {
            u16 addr = rd16(mem, pc);
            pc += 2;
            sp -= 2;
            wr16(mem, sp, rd16(mem, addr));
            break;
        }
        case BC_STATIC_GET32: {
            u16 addr = rd16(mem, pc);
            pc += 2;
            sp -= 4;
            wr32(mem, sp, rd32(mem, addr));
            break;
        }
        case BC_STATIC_SET: {
            u16 addr = rd16(mem, pc);
            pc += 2;
            wr16(mem, addr, rd16(mem, sp));
            sp += 2;
            break;
        }
        case BC_STATIC_SET32: {
            u16 addr = rd16(mem, pc);
            pc += 2;
            wr32(mem, addr, rd32(mem, sp));
            sp += 4;
            break;
        }
        case BC_LOCAL_GET: {
            u16 off = rd16(mem, pc);
            pc += 2;
            sp -= 2;
            wr16(mem, sp, rd16(mem, (u16)(fp + off)));
            break;
        }
        case BC_LOCAL_GET32: {
            u16 off = rd16(mem, pc);
            pc += 2;
            sp -= 4;
            wr32(mem, sp, rd32(mem, (u16)(fp + off)));
            break;
        }
        case BC_LOCAL_SET: {
            u16 off = rd16(mem, pc);
            pc += 2;
            wr16(mem, (u16)(fp + off), rd16(mem, sp));
            sp += 2;
            break;
        }
        case BC_LOCAL_SET32: {
            u16 off = rd16(mem, pc);
            pc += 2;
            wr32(mem, (u16)(fp + off), rd32(mem, sp));
            sp += 4;
            break;
        }
        case BC_LOCAL_ADDR: {
            u16 off = rd16(mem, pc);
            pc += 2;
            sp -= 2;
            wr16(mem, sp, (u16)(fp + off));
            break;
        }

        case BC_DUP: {
            u16 v = rd16(mem, sp);
            sp -= 2;
            wr16(mem, sp, v);
            break;
        }
        case BC_DUP32: {
            u32 v = rd32(mem, sp);
            sp -= 4;
            wr32(mem, sp, v);
            break;
        }
        case BC_DROP:
            sp += 2;
            break;
        case BC_DROP32:
            sp += 4;
            break;
        case BC_SWAP: {
            u16 a = rd16(mem, sp);
            u16 b = rd16(mem, (u16)(sp + 2));
            wr16(mem, sp, b);
            wr16(mem, (u16)(sp + 2), a);
            break;
        }
        case BC_SWAP_W32: {
            u32 t = rd32(mem, sp);
            u16 n = rd16(mem, (u16)(sp + 4));
            wr16(mem, sp, n);
            wr32(mem, (u16)(sp + 2), t);
            break;
        }
        case BC_SWAP_32W: {
            u16 t = rd16(mem, sp);
            u32 n = rd32(mem, (u16)(sp + 2));
            wr32(mem, sp, n);
            wr16(mem, (u16)(sp + 4), t);
            break;
        }
        case BC_SWAP32: {
            u32 a = rd32(mem, sp);
            u32 b = rd32(mem, (u16)(sp + 4));
            wr32(mem, sp, b);
            wr32(mem, (u16)(sp + 4), a);
            break;
        }

        case BC_JMP:
            pc = rd16(mem, pc);
            break;
        case BC_JTAB: {
            u16 idx = rd16(mem, sp);
            sp = (u16)(sp + 2);
            pc = rd16(mem, (u16)(pc + idx * 2));
            break;
        }
        case BC_JZ: {
            u16 target = rd16(mem, pc);
            u16 cond = rd16(mem, sp);
            pc += 2;
            sp += 2;
            if (cond == 0)
                pc = target;
            break;
        }
        case BC_CALL: {
            u16 target = rd16(mem, pc);
            pc += 2;
            if (csp >= VM_CALL_MAX) {
                fprintf(stderr, "VM call stack overflow\n");
                free(mem);
                exit(1);
            }
            call_stack[csp++] = pc;
            pc = target;
            break;
        }
        case BC_ICALL: {
            u16 target = rd16(mem, sp);
            sp += 2;
            if (csp >= VM_CALL_MAX) {
                fprintf(stderr, "VM call stack overflow\n");
                free(mem);
                exit(1);
            }
            call_stack[csp++] = pc;
            pc = target;
            break;
        }
        case BC_CALL_EXTERN: {
            u16 id = rd16(mem, pc);
            const char *name = bc_extern_name(id);
            pc += 2;
            if (strcmp(name, "putchar") == 0) {
                int c = (int)(i16)rd16(mem, sp);
                int r = putchar(c);
                wr16(mem, sp, (u16)r);
            } else if (strcmp(name, "getchar") == 0) {
                int r = getchar();
                sp -= 2;
                wr16(mem, sp, (u16)r);
            } else {
                fprintf(stderr, "Unsupported extern in VM: %s\n", name);
                free(mem);
                exit(1);
            }
            break;
        }

        case BC_ADD: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) + b));
            break;
        }
        case BC_SUB: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) - b));
            break;
        }
        case BC_MUL: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)((i16)rd16(mem, sp) * b));
            break;
        }
        case BC_DIV: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)((i16)rd16(mem, sp) / b));
            break;
        }
        case BC_MOD: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)((i16)rd16(mem, sp) % b));
            break;
        }
        case BC_UDIV: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) / b));
            break;
        }
        case BC_UMOD: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) % b));
            break;
        }
        case BC_AND: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) & b));
            break;
        }
        case BC_OR: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) | b));
            break;
        }
        case BC_XOR: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)(rd16(mem, sp) ^ b));
            break;
        }
        case BC_INVERT:
            wr16(mem, sp, (u16)(~rd16(mem, sp)));
            break;
        case BC_LSHIFT: {
            u16 b = rd16(mem, sp);
            u16 a;
            sp += 2;
            a = rd16(mem, sp);
            wr16(mem, sp, b >= 16 ? 0 : (u16)(a << b));
            break;
        }
        case BC_ARSHIFT: {
            u16 b = rd16(mem, sp);
            i16 a;
            sp += 2;
            a = (i16)rd16(mem, sp);
            wr16(mem, sp, b >= 16 ? (a < 0 ? 0xFFFF : 0) : (u16)(a >> b));
            break;
        }
        case BC_RSHIFT: {
            u16 b = rd16(mem, sp);
            u16 a;
            sp += 2;
            a = rd16(mem, sp);
            wr16(mem, sp, b >= 16 ? 0 : (u16)(a >> b));
            break;
        }

        case BC_FADD: {
            float b = rd_f(mem, sp);
            sp += 4;
            wr_f(mem, sp, rd_f(mem, sp) + b);
            break;
        }
        case BC_FSUB: {
            float b = rd_f(mem, sp);
            sp += 4;
            wr_f(mem, sp, rd_f(mem, sp) - b);
            break;
        }
        case BC_FMUL: {
            float b = rd_f(mem, sp);
            sp += 4;
            wr_f(mem, sp, rd_f(mem, sp) * b);
            break;
        }
        case BC_FDIV: {
            float b = rd_f(mem, sp);
            sp += 4;
            wr_f(mem, sp, rd_f(mem, sp) / b);
            break;
        }
        case BC_FNEG:
            wr_f(mem, sp, -rd_f(mem, sp));
            break;

        case BC_ADD32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) + b);
            break;
        }
        case BC_SUB32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) - b);
            break;
        }
        case BC_MUL32: {
            i32 b = (i32)rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, (u32)((i32)rd32(mem, sp) * b));
            break;
        }
        case BC_DIV32: {
            i32 b = (i32)rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, (u32)((i32)rd32(mem, sp) / b));
            break;
        }
        case BC_MOD32: {
            i32 b = (i32)rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, (u32)((i32)rd32(mem, sp) % b));
            break;
        }
        case BC_UDIV32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) / b);
            break;
        }
        case BC_UMOD32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) % b);
            break;
        }
        case BC_AND32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) & b);
            break;
        }
        case BC_OR32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) | b);
            break;
        }
        case BC_XOR32: {
            u32 b = rd32(mem, sp);
            sp += 4;
            wr32(mem, sp, rd32(mem, sp) ^ b);
            break;
        }
        case BC_INV32:
            wr32(mem, sp, ~rd32(mem, sp));
            break;
        case BC_SHL32: {
            u16 b = rd16(mem, sp);
            u32 a;
            sp += 2;
            a = rd32(mem, sp);
            wr32(mem, sp, b >= 32 ? 0 : (a << b));
            break;
        }
        case BC_ASHR32: {
            u16 b = rd16(mem, sp);
            i32 a;
            sp += 2;
            a = (i32)rd32(mem, sp);
            wr32(mem, sp, b >= 32 ? (a < 0 ? 0xFFFFFFFFUL : 0) : (u32)(a >> b));
            break;
        }
        case BC_RSHR32: {
            u16 b = rd16(mem, sp);
            u32 a;
            sp += 2;
            a = rd32(mem, sp);
            wr32(mem, sp, b >= 32 ? 0 : (a >> b));
            break;
        }

        case BC_EQ: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, rd16(mem, sp) == b ? 0xFFFF : 0);
            break;
        }
        case BC_NE: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, rd16(mem, sp) != b ? 0xFFFF : 0);
            break;
        }
        case BC_LT: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (i16)rd16(mem, sp) < b ? 0xFFFF : 0);
            break;
        }
        case BC_GT: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (i16)rd16(mem, sp) > b ? 0xFFFF : 0);
            break;
        }
        case BC_LE: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (i16)rd16(mem, sp) <= b ? 0xFFFF : 0);
            break;
        }
        case BC_GE: {
            i16 b = (i16)rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, (i16)rd16(mem, sp) >= b ? 0xFFFF : 0);
            break;
        }
        case BC_ULT: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, rd16(mem, sp) < b ? 0xFFFF : 0);
            break;
        }
        case BC_UGT: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, rd16(mem, sp) > b ? 0xFFFF : 0);
            break;
        }
        case BC_ULE: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, rd16(mem, sp) <= b ? 0xFFFF : 0);
            break;
        }
        case BC_UGE: {
            u16 b = rd16(mem, sp);
            sp += 2;
            wr16(mem, sp, rd16(mem, sp) >= b ? 0xFFFF : 0);
            break;
        }

        case BC_FEQ: {
            float b = rd_f(mem, sp);
            float a = rd_f(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a == b ? 0xFFFF : 0);
            break;
        }
        case BC_FNE: {
            float b = rd_f(mem, sp);
            float a = rd_f(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a != b ? 0xFFFF : 0);
            break;
        }
        case BC_FLT: {
            float b = rd_f(mem, sp);
            float a = rd_f(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a < b ? 0xFFFF : 0);
            break;
        }
        case BC_FGT: {
            float b = rd_f(mem, sp);
            float a = rd_f(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a > b ? 0xFFFF : 0);
            break;
        }

        case BC_EQ32: {
            u32 b = rd32(mem, sp);
            u32 a = rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a == b ? 0xFFFF : 0);
            break;
        }
        case BC_NE32: {
            u32 b = rd32(mem, sp);
            u32 a = rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a != b ? 0xFFFF : 0);
            break;
        }
        case BC_LT32: {
            i32 b = (i32)rd32(mem, sp);
            i32 a = (i32)rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a < b ? 0xFFFF : 0);
            break;
        }
        case BC_GT32: {
            i32 b = (i32)rd32(mem, sp);
            i32 a = (i32)rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a > b ? 0xFFFF : 0);
            break;
        }
        case BC_LE32: {
            i32 b = (i32)rd32(mem, sp);
            i32 a = (i32)rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a <= b ? 0xFFFF : 0);
            break;
        }
        case BC_GE32: {
            i32 b = (i32)rd32(mem, sp);
            i32 a = (i32)rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a >= b ? 0xFFFF : 0);
            break;
        }
        case BC_ULT32: {
            u32 b = rd32(mem, sp);
            u32 a = rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a < b ? 0xFFFF : 0);
            break;
        }
        case BC_UGT32: {
            u32 b = rd32(mem, sp);
            u32 a = rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a > b ? 0xFFFF : 0);
            break;
        }
        case BC_ULE32: {
            u32 b = rd32(mem, sp);
            u32 a = rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a <= b ? 0xFFFF : 0);
            break;
        }
        case BC_UGE32: {
            u32 b = rd32(mem, sp);
            u32 a = rd32(mem, (u16)(sp + 4));
            sp += 6;
            wr16(mem, sp, a >= b ? 0xFFFF : 0);
            break;
        }

        case BC_I8FETCH: {
            u16 addr = rd16(mem, sp);
            wr16(mem, sp, (u16)(i16)(i8)mem[addr]);
            break;
        }
        case BC_U8FETCH: {
            u16 addr = rd16(mem, sp);
            wr16(mem, sp, (u16)mem[addr]);
            break;
        }
        case BC_I8STORE: {
            u16 addr = rd16(mem, sp);
            u16 val = rd16(mem, (u16)(sp + 2));
            mem[addr] = (u8)(val & 0xFF);
            sp += 4;
            break;
        }
        case BC_FETCH:
        case BC_U16FETCH:
        case BC_I16FETCH: {
            u16 addr = rd16(mem, sp);
            wr16(mem, sp, rd16(mem, addr));
            break;
        }
        case BC_STORE:
        case BC_STORE16: {
            u16 addr = rd16(mem, sp);
            u16 val = rd16(mem, (u16)(sp + 2));
            wr16(mem, addr, val);
            sp += 4;
            break;
        }
        case BC_FETCH32: {
            u16 addr = rd16(mem, sp);
            sp -= 2;
            wr32(mem, sp, rd32(mem, addr));
            break;
        }
        case BC_STORE32: {
            u16 addr = rd16(mem, sp);
            u32 val = rd32(mem, (u16)(sp + 2));
            wr32(mem, addr, val);
            sp += 6;
            break;
        }
        case BC_BCOPY: {
            u16 len = rd16(mem, sp);
            u16 src = rd16(mem, (u16)(sp + 2));
            u16 dst = rd16(mem, (u16)(sp + 4));
            if (len > 0)
                memmove(mem + dst, mem + src, len);
            sp += 6;
            break;
        }

        case BC_SEXT:
            wr16(mem, sp, (u16)(i16)(i8)(rd16(mem, sp) & 0xFF));
            break;
        case BC_W2F: {
            float f = (float)(i16)rd16(mem, sp);
            sp -= 2;
            wr_f(mem, sp, f);
            break;
        }
        case BC_F2W: {
            i16 v = (i16)rd_f(mem, sp);
            sp += 2;
            wr16(mem, sp, (u16)v);
            break;
        }
        case BC_W2I32: {
            i32 v = (i32)(i16)rd16(mem, sp);
            sp -= 2;
            wr32(mem, sp, (u32)v);
            break;
        }
        case BC_U2I32: {
            u32 v = (u32)rd16(mem, sp);
            sp -= 2;
            wr32(mem, sp, v);
            break;
        }
        case BC_I322W: {
            u16 v = (u16)(rd32(mem, sp) & 0xFFFF);
            sp += 2;
            wr16(mem, sp, v);
            break;
        }
        }
    }
}

static void wr32_mem(U8Vec *v, u32 val) {
    u8 b[4];
    cb_put_le32(b, val);
    {
        size_t i;
        for (i = 0; i < 4; i++)
            VEC_PUSH(*v, b[i]);
    }
}

static Bool bc32_build(U8Vec *out, size_t base) {
    int main_idx = ir4_find_func("main");
    u32 entry = (main_idx >= 0) ? (u32)ir4_func_addr((size_t)main_idx) : (u32)base;
    u32 code_sz = (u32)cb_pc();
    u32 init_sz = (u32)cb_init_data_size();
    u32 total_data = (u32)cb_data_size();
    u32 bss_sz = (total_data > init_sz) ? (total_data - init_sz) : 0;
    u16 ext_cnt = bc_extern_count();
    u16 i;
    const u8 *code = cb_data();
    const u8 *init_d = cb_init_data();

    VEC_PUSH(*out, 'I'); VEC_PUSH(*out, 'R'); VEC_PUSH(*out, '4'); VEC_PUSH(*out, 'B');
    wr32_mem(out, (u32)base);
    wr32_mem(out, entry);
    wr32_mem(out, code_sz);
    wr32_mem(out, init_sz);
    wr32_mem(out, bss_sz);
    wr32_mem(out, (u32)ext_cnt);
    for (i = 0; i < ext_cnt; i++) {
        const char *name = bc_extern_name(i);
        size_t j, len = strlen(name) + 1;
        for (j = 0; j < len; j++)
            VEC_PUSH(*out, (u8)name[j]);
    }
    {
        u32 k;
        for (k = 0; k < code_sz; k++)
            VEC_PUSH(*out, code[k]);
        for (k = 0; k < init_sz; k++)
            VEC_PUSH(*out, init_d[k]);
    }
    return true;
}

static int bc32_write(FILE *fp, size_t base) {
    U8Vec bin = {NULL, 0, 0};
    int ok;
    bc32_build(&bin, base);
    ok = (bin.len == 0 || fwrite(bin.data, 1, bin.len, fp) == bin.len) && ferror(fp) == 0;
    free(bin.data);
    return ok;
}

static void write_base64(FILE *fp, const u8 *d, size_t len) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i;
    for (i = 0; i < len; i += 3) {
        u32 v = ((u32)d[i] << 16) |
                ((i + 1 < len ? (u32)d[i + 1] : 0) << 8) |
                (i + 2 < len ? (u32)d[i + 2] : 0);
        fputc(b64[(v >> 18) & 63], fp);
        fputc(b64[(v >> 12) & 63], fp);
        fputc((i + 1 < len) ? b64[(v >> 6) & 63] : '=', fp);
        fputc((i + 2 < len) ? b64[v & 63] : '=', fp);
    }
}

static int web_write(FILE *fp, size_t base) {
    U8Vec bin = {NULL, 0, 0};
    bc32_build(&bin, base);
    fprintf(fp,
            "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"><title>IR4th</title>\n"
            "<style>body{margin:0;background:#111;display:flex;justify-content:center;"
            "align-items:center;min-height:100vh}</style>\n</head><body>\n"
            "<ir4-screen width=\"320\" height=\"240\" scale=\"2\" data-bin=\"");
    write_base64(fp, bin.data, bin.len);
    fprintf(fp, "\"></ir4-screen>\n<script>\n");
    fwrite(plat_web_vm_js, 1, sizeof(plat_web_vm_js), fp);
    fprintf(fp, "\n</script>\n</body></html>\n");
    free(bin.data);
    return ferror(fp) == 0;
}

static int web_js_write(FILE *fp, size_t base) {
    U8Vec bin = {NULL, 0, 0};
    bc32_build(&bin, base);
    fprintf(fp,
            "(function () {\n"
            "  var s = document.currentScript;\n"
            "  var el = s && s.previousElementSibling;\n"
            "  if (!el || el.tagName !== \"IR4-SCREEN\") el = document.querySelector(\"ir4-screen\");\n"
            "  if (el) el.setAttribute(\"data-bin\", \"");
    write_base64(fp, bin.data, bin.len);
    fprintf(fp, "\");\n})();\n");
    free(bin.data);
    return ferror(fp) == 0;
}

const Platform plat_bc_run = {"--run", &be_bc, 0x0100, NULL, true, NULL, NULL, NULL, bc_run_vm, "bin"};
const Platform plat_bc32 = {"--bc32", &be_bc32, 0x0100, "bc", true, NULL, NULL, bc32_write, NULL, "bin"};
const Platform plat_web = {"--web", &be_bc32, 0x0100, "web", true, NULL, NULL, web_write, NULL, "html"};
const Platform plat_web_js = {"--web-js", &be_bc32, 0x0100, "web", true, NULL, NULL, web_js_write, NULL, "js"};
