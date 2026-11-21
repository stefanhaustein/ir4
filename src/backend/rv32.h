/* rv32.h - Shared RV32IM instruction encoders (Strict C89)
 *
 * Exposes R/I/S/B/U/J-type RV32IM instruction encoders implemented in
 * backend/rv32.c for reuse by platform/elf.c (-t virt and -t obj).
 */
#ifndef BE_RV32_H
#define BE_RV32_H

#include "ir4.h"

u32 be_rv32_enc_r(u32 f7, u32 rs2, u32 rs1, u32 f3, u32 rd, u32 op);
u32 be_rv32_enc_i(i32 imm, u32 rs1, u32 f3, u32 rd, u32 op);
u32 be_rv32_enc_s(i32 imm, u32 rs2, u32 rs1, u32 f3);
u32 be_rv32_enc_b(i32 imm, u32 rs2, u32 rs1, u32 f3);
u32 be_rv32_enc_u(u32 imm20, u32 rd, u32 op);
u32 be_rv32_enc_j(i32 imm, u32 rd);

#define enc_r be_rv32_enc_r
#define enc_i be_rv32_enc_i
#define enc_s be_rv32_enc_s
#define enc_b be_rv32_enc_b
#define enc_u be_rv32_enc_u
#define enc_j be_rv32_enc_j

#endif /* BE_RV32_H */
