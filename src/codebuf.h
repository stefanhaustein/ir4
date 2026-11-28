/* codebuf.h - Shared code & data emission buffer API
 *
 * Manages the target machine-code buffer, initialized/BSS data segment,
 * data-offset relocations (patched once data placement is known), and
 * named external symbol relocations (resolved by JIT or ELF .o output).
 */
#ifndef CODEBUF_H
#define CODEBUF_H

#include "ir4.h"

void cb_reset(size_t base);
void cb_set_spill_file(FILE *fp);
void cb_cleanup(void);
size_t cb_pc(void);
size_t cb_cur_addr(void);
u8 cb_read_u8(size_t offset);
void cb_read_code(void *dst, size_t offset, size_t len);
Bool cb_write_code(FILE *fp, size_t len);
void cb_flush(void);

void cb_emit8(u8 b);
void cb_emit16(u16 w);
void cb_emit24(u32 w);
void cb_emit32(u32 w);
void cb_emit8_8(u8 a, u8 b);
void cb_emit8_16(u8 a, u16 w);
void cb_emit8_8_8(u8 a, u8 b, u8 c);
void cb_rewind(size_t n);

typedef struct {
    size_t code_len;
    size_t data_len;
    size_t relocs_len;
    size_t data_init_relocs_len;
    size_t ext_relocs_len;
} CbCheckpoint;

CbCheckpoint cb_checkpoint(void);
void cb_restore(CbCheckpoint cp);

void cb_patch_locally8(size_t patch_offset, u8 val);
void cb_patch_locally16(size_t patch_offset, u16 target_addr);
void cb_patch_locally24(size_t patch_offset, u32 target_addr);
void cb_patch_locally32(size_t patch_offset, u32 val);
void cb_patch_globally8(size_t patch_offset, u8 val);
void cb_patch_globally16(size_t patch_offset, u16 target_addr);
void cb_patch_globally24(size_t patch_offset, u32 target_addr);
void cb_patch_globally32(size_t patch_offset, u32 val);
void cb_put_le16(u8 *p, u16 v);
void cb_put_le32(u8 *p, u32 v);
Bool cb_write_raw(FILE *fp, Bool include_data);

size_t cb_alloc_data(size_t size, u8 align);
size_t cb_data_size(void);
void cb_emit_data_u8(u8 b);
void cb_emit_data_u16(u16 w);
void cb_emit_data_word(i32 val, u8 word_bytes);
const u8 *cb_init_data(void);
size_t cb_init_data_size(void);

#define CB_HEAP_START_OFF ((size_t)-1)

void cb_add_data_reloc(size_t patch_offset, size_t data_offset);
void cb_add_data_reloc24(size_t patch_offset, size_t data_offset);
void cb_add_data_reloc32(size_t patch_offset, size_t data_offset);
void cb_add_data_reloc_bank(size_t patch_offset, size_t data_offset);
void cb_add_data_reloc_split(size_t lo_offset, size_t hi_offset, size_t data_offset);
void cb_patch_data_relocs(size_t data_base);

void cb_add_data_init_reloc(size_t patch_data_off, size_t target_val, u8 addr_bytes, Bool is_code);
size_t cb_data_init_reloc_count(void);
void cb_get_data_init_reloc(size_t idx, size_t *patch_data_off, size_t *target_val, u8 *addr_bytes,
                            Bool *is_code);
void cb_patch_data_init_reloc(size_t idx, size_t code_addr);
void cb_patch_data_init_relocs_base(u32 code_base, u32 data_base);

void cb_add_extern_reloc(size_t patch_offset, const char *name);
size_t cb_extern_reloc_count(void);
const char *cb_extern_reloc_name(size_t idx, size_t *patch_offset);

typedef struct {
    size_t base;
    size_t start_pc;
    size_t len;
} BankDesc;

void cb_set_bank_config(size_t bank_base, size_t bank_size);
Bool cb_is_banked(void);
size_t cb_bank_base(void);
size_t cb_bank_size(void);
void cb_begin_bank(u16 bank_id);
void cb_end_banks(void);
u16 cb_cur_bank(void);
size_t cb_bank_count(void);
BankDesc cb_get_bank(u16 bank_id);

#endif /* CODEBUF_H */
