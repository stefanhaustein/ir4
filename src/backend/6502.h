/* be_6502.h - Shared 6502 opcode constants for 6502 backend & platform stubs */
#ifndef BE_6502_H
#define BE_6502_H

enum {
    /* Implied / Accumulator / Stack */
    M6502_PHP = 0x08,
    M6502_CLC = 0x18,
    M6502_PLP = 0x28,
    M6502_ROL_A = 0x2A,
    M6502_SEC = 0x38,
    M6502_PHA = 0x48,
    M6502_RTS = 0x60,
    M6502_PLA = 0x68,
    M6502_DEY = 0x88,
    M6502_TXA = 0x8A,
    M6502_TYA = 0x98,
    M6502_TXS = 0x9A,
    M6502_TAY = 0xA8,
    M6502_INY = 0xC8,
    M6502_DEX = 0xCA,
    M6502_CLD = 0xD8,
    M6502_INX = 0xE8,

    /* Immediate */
    M6502_ORA_IMM = 0x09,
    M6502_AND_IMM = 0x29,
    M6502_EOR_IMM = 0x49,
    M6502_ADC_IMM = 0x69,
    M6502_LDY_IMM = 0xA0,
    M6502_LDX_IMM = 0xA2,
    M6502_LDA_IMM = 0xA9,
    M6502_CMP_IMM = 0xC9,
    M6502_SBC_IMM = 0xE9,

    /* Zero Page */
    M6502_ORA_ZP = 0x05,
    M6502_ASL_ZP = 0x06,
    M6502_ROL_ZP = 0x26,
    M6502_STA_ZP = 0x85,
    M6502_STX_ZP = 0x86,
    M6502_LDY_ZP = 0xA4,
    M6502_LDA_ZP = 0xA5,
    M6502_LDX_ZP = 0xA6,
    M6502_SBC_ZP = 0xE5,
    M6502_INC_ZP = 0xE6,

    /* Absolute / Indexed */
    M6502_ADC_ABS = 0x6D,
    M6502_STA_ABS = 0x8D,
    M6502_STA_ABSX = 0x9D,
    M6502_LDA_ABS = 0xAD,
    M6502_LDA_ABSX = 0xBD,

    /* Indirect,Y */
    M6502_STA_INDY = 0x91,
    M6502_LDA_INDY = 0xB1,

    /* Branches & Jumps */
    M6502_BPL = 0x10,
    M6502_JSR = 0x20,
    M6502_BMI = 0x30,
    M6502_JMP = 0x4C,
    M6502_BVC = 0x50,
    M6502_BCC = 0x90,
    M6502_BCS = 0xB0,
    M6502_BNE = 0xD0,
    M6502_BEQ = 0xF0
};

void be_6502_set_ram_top(u16 top);
void be_6502_set_default_stack_zp(u8 base, u8 init_x);
u8 be_6502_stack_zp_init_x(void);

void be_6502_cbm_begin(size_t base, int is_x16);
void be_6502_cbm_end(int main_idx, int is_x16);
int be_6502_cbm_write(FILE *fp, size_t base);

#endif /* BE_6502_H */
