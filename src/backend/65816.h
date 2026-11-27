/* 65816.h - W65C816 opcode constants and backend helpers */
#ifndef BE_65816_H
#define BE_65816_H

#include "ir4.h"

enum {
    /* Processor mode & status */
    M816_CLC = 0x18,
    M816_SEC = 0x38,
    M816_CLD = 0xD8,
    M816_XCE = 0xFB,
    M816_REP = 0xC2,
    M816_SEP = 0xE2,
    M816_XBA = 0xEB,

    /* Stack & transfer */
    M816_PHA = 0x48,
    M816_PLA = 0x68,
    M816_PHX = 0xDA,
    M816_PLX = 0xFA,
    M816_PHY = 0x5A,
    M816_PLY = 0x7A,
    M816_PHB = 0x8B,
    M816_PLB = 0xAB,
    M816_PHK = 0x4B,
    M816_PEA = 0xF4,
    M816_TAX = 0xAA,
    M816_TXA = 0x8A,
    M816_TAY = 0xA8,
    M816_TYA = 0x98,
    M816_TXS = 0x9A,
    M816_TSX = 0xBA,
    M816_TCD = 0x5B,
    M816_TDC = 0x7B,
    M816_INX = 0xE8,
    M816_DEX = 0xCA,
    M816_INY = 0xC8,
    M816_DEY = 0x88,

    /* Immediate (16-bit when M=0/X=0, 8-bit when M=1/X=1) */
    M816_LDA_IMM = 0xA9,
    M816_LDX_IMM = 0xA2,
    M816_LDY_IMM = 0xA0,
    M816_ADC_IMM = 0x69,
    M816_SBC_IMM = 0xE9,
    M816_AND_IMM = 0x29,
    M816_ORA_IMM = 0x09,
    M816_EOR_IMM = 0x49,
    M816_CMP_IMM = 0xC9,
    M816_CPX_IMM = 0xE0,
    M816_CPY_IMM = 0xC0,

    /* Direct Page */
    M816_LDA_DP = 0xA5,
    M816_STA_DP = 0x85,
    M816_LDX_DP = 0xA6,
    M816_STX_DP = 0x86,
    M816_LDY_DP = 0xA4,
    M816_STY_DP = 0x84,
    M816_ADC_DP = 0x65,
    M816_SBC_DP = 0xE5,
    M816_AND_DP = 0x25,
    M816_ORA_DP = 0x05,
    M816_EOR_DP = 0x45,
    M816_CMP_DP = 0xC5,
    M816_INC_DP = 0xE6,
    M816_DEC_DP = 0xC6,
    M816_ASL_DP = 0x06,
    M816_LSR_DP = 0x46,
    M816_ROL_DP = 0x26,
    M816_ROR_DP = 0x66,
    M816_STZ_DP = 0x64,

    /* Direct Page Indirect Long ([dp], [dp],Y) */
    M816_LDA_IDL = 0xA7,
    M816_STA_IDL = 0x87,
    M816_LDA_IDLY = 0xB7,
    M816_STA_IDLY = 0x97,

    /* Absolute / Indexed in Data Bank (DBR) */
    M816_LDA_ABS = 0xAD,
    M816_STA_ABS = 0x8D,
    M816_LDA_ABSX = 0xBD,
    M816_STA_ABSX = 0x9D,
    M816_ADC_ABSX = 0x7D,
    M816_SBC_ABSX = 0xFD,
    M816_AND_ABSX = 0x3D,
    M816_ORA_ABSX = 0x1D,
    M816_EOR_ABSX = 0x5D,
    M816_CMP_ABSX = 0xDD,
    M816_STZ_ABS = 0x9C,
    M816_STZ_ABSX = 0x9E,

    /* 24-bit Long addressing */
    M816_LDA_LONG = 0xAF,
    M816_STA_LONG = 0x8F,
    M816_LDA_LONGX = 0xBF,
    M816_STA_LONGX = 0x9F,

    /* Accumulator shifts & inc/dec */
    M816_INC_A = 0x1A,
    M816_DEC_A = 0x3A,
    M816_ASL_A = 0x0A,
    M816_LSR_A = 0x4A,
    M816_ROL_A = 0x2A,
    M816_ROR_A = 0x6A,

    /* Branches, jumps, calls */
    M816_BRA = 0x80,
    M816_BRL = 0x82,
    M816_BPL = 0x10,
    M816_BMI = 0x30,
    M816_BVC = 0x50,
    M816_BVS = 0x70,
    M816_BCC = 0x90,
    M816_BCS = 0xB0,
    M816_BNE = 0xD0,
    M816_BEQ = 0xF0,
    M816_JMP = 0x4C,
    M816_JML = 0x5C,
    M816_JMP_IND = 0x6C,
    M816_JML_IND = 0xDC,
    M816_JSR = 0x20,
    M816_JSL = 0x22,
    M816_RTS = 0x60,
    M816_RTL = 0x6B,
    M816_STP = 0xDB
};

#define M816_STACK_BASE 0x0200U
#define M816_STACK_INIT_X 0x0E00U

#define M816_DP_PTR0 0x00U  /* 4 bytes (3-byte ptr + 16-bit high access) */
#define M816_DP_PTR1 0x04U  /* 4 bytes */
#define M816_DP_TMP0 0x08U  /* 4 bytes */
#define M816_DP_TMP1 0x0CU  /* 4 bytes */
#define M816_DP_LFP  0x10U  /* 4 bytes */
#define M816_DP_TMP2 0x14U  /* 4 bytes */
#define M816_DP_RET  0x18U  /* 4 bytes */

void be_65816_reset(void);
void be_65816_invalidate_from(size_t addr);
void be_65816_relax_reset(void);
int be_65816_need_relax(void);
size_t be_65816_rstack_off(void);
void be_65816_emit_zero_bss(void);

#endif /* BE_65816_H */
