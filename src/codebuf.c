/* codebuf.c - Shared binary code & data emission buffer for IR4th */
#include "codebuf.h"

typedef struct {
    Bool split;
    u8 width;
    size_t off1;
    size_t off2;
    size_t data_offset;
} DataReloc;

typedef struct {
    size_t patch_offset;
    const char *name;
} ExternReloc;

typedef struct {
    size_t patch_data_off;
    size_t target_val;
    u8 addr_bytes;
    Bool is_code;
} DataInitReloc;

VEC_OF(ByteVec, u8);
VEC_OF(DataRelocVec, DataReloc);
VEC_OF(DataInitRelocVec, DataInitReloc);
VEC_OF(ExternRelocVec, ExternReloc);
VEC_OF(BankDescVec, BankDesc);

static ByteVec code_buf;
static size_t flushed_len = 0;
static size_t flush_watermark = 0;
static FILE *spill_fp = NULL;
static size_t base_addr = 0x0100; /* Default: CP/M .COM load address */
static ByteVec data_buf;
static size_t data_init_size = 0;
static DataRelocVec relocs;
static DataInitRelocVec data_init_relocs;
static ExternRelocVec ext_relocs;
static BankDescVec banks;
static size_t cfg_bank_base = 0;
static size_t cfg_bank_size = 0;
static u16 cur_bank_id = 0;

/* Code buffer & emission */

void cb_cleanup(void) {
    spill_fp = NULL;
    flushed_len = 0;
    flush_watermark = 0;
}

void cb_set_spill_file(FILE *fp) {
    cb_cleanup();
    spill_fp = fp;
}

void cb_reset(size_t base) {
    BankDesc *b0;
    cb_cleanup();
    code_buf.len = 0;
    base_addr = base;
    data_buf.len = 0;
    data_init_size = 0;
    relocs.len = 0;
    data_init_relocs.len = 0;
    ext_relocs.len = 0;
    banks.len = 0;
    cfg_bank_base = 0;
    cfg_bank_size = 0;
    cur_bank_id = 0;
    b0 = VEC_ADD(banks);
    b0->base = base;
    b0->start_pc = 0;
    b0->len = 0;
}

size_t cb_pc(void) { return flushed_len + code_buf.len; }

size_t cb_cur_addr(void) {
    size_t pc = cb_pc();
    if (cfg_bank_size == 0 || cur_bank_id == 0) {
        return base_addr + pc;
    }
    return banks.data[cur_bank_id].base + (pc - banks.data[cur_bank_id].start_pc);
}

u8 cb_read_u8(size_t offset) {
    u8 b = 0;
    cb_read_code(&b, offset, 1);
    return b;
}

void cb_read_code(void *dst, size_t offset, size_t len) {
    size_t total = cb_pc();
    u8 *out = (u8 *)dst;
    if (offset + len > total)
        ir4_die("code read out of bounds");
    if (len == 0)
        return;
    if (offset < flushed_len) {
        size_t file_n = flushed_len - offset;
        u8 buf[512];
        if (file_n > len)
            file_n = len;
        if (fseek(spill_fp, (long)offset, SEEK_SET) != 0)
            ir4_die("failed seeking code spill file");
        len -= file_n;
        offset += file_n;
        while (file_n > 0) {
            size_t chunk = (file_n > sizeof(buf)) ? sizeof(buf) : file_n;
            if (fread(buf, 1, chunk, spill_fp) != chunk)
                ir4_die("failed reading code spill file");
            memcpy(out, buf, chunk);
            out += chunk;
            file_n -= chunk;
        }
    }
    if (len > 0) {
        memcpy(out, code_buf.data + (offset - flushed_len), len);
    }
}

Bool cb_write_code(FILE *fp, size_t len) {
    size_t total = cb_pc();
    size_t rem;
    if (len > total)
        ir4_die("code write out of bounds");
    rem = len;
    if (rem > 0 && flushed_len > 0) {
        size_t file_rem = (rem < flushed_len) ? rem : flushed_len;
        u8 buf[512];
        if (fseek(spill_fp, 0, SEEK_SET) != 0)
            return false;
        while (file_rem > 0) {
            size_t chunk = (file_rem > sizeof(buf)) ? sizeof(buf) : file_rem;
            if (fread(buf, 1, chunk, spill_fp) != chunk || fwrite(buf, 1, chunk, fp) != chunk)
                return false;
            file_rem -= chunk;
            rem -= chunk;
        }
    }
    if (rem > 0 && fwrite(code_buf.data, 1, rem, fp) != rem)
        return false;
    return true;
}

void cb_flush(void) {
    if (!spill_fp) {
        flush_watermark = code_buf.len;
        return;
    }
    if (code_buf.len == 0)
        return;
    if (fseek(spill_fp, (long)flushed_len, SEEK_SET) != 0 ||
        fwrite(code_buf.data, 1, code_buf.len, spill_fp) != code_buf.len) {
        ir4_die("failed writing code spill file");
    }
    flushed_len += code_buf.len;
    code_buf.len = 0;
    flush_watermark = 0;
}

void cb_emit8(u8 b) { VEC_PUSH(code_buf, b); }

void cb_emit16(u16 w) {
    cb_emit8((u8)(w & 0xFF));        /* Little-endian low byte */
    cb_emit8((u8)((w >> 8) & 0xFF)); /* Little-endian high byte */
}

void cb_emit24(u32 w) {
    cb_emit8((u8)(w & 0xFFU));
    cb_emit8((u8)((w >> 8) & 0xFFU));
    cb_emit8((u8)((w >> 16) & 0xFFU));
}

void cb_emit32(u32 w) {
    cb_emit16((u16)(w & 0xFFFFU));
    cb_emit16((u16)((w >> 16) & 0xFFFFU));
}

void cb_emit8_8(u8 a, u8 b) {
    cb_emit8(a);
    cb_emit8(b);
}

void cb_emit8_16(u8 a, u16 w) {
    cb_emit8(a);
    cb_emit16(w);
}

void cb_emit8_8_8(u8 a, u8 b, u8 c) {
    cb_emit8(a);
    cb_emit8(b);
    cb_emit8(c);
}

void cb_rewind(size_t n) {
    size_t pc;
    if (n > code_buf.len || code_buf.len - n < flush_watermark) {
        ir4_die("code buffer underflow");
    }
    code_buf.len -= n;
    pc = cb_pc();
    while (relocs.len > 0 && relocs.data[relocs.len - 1].off1 >= pc) {
        relocs.len--;
    }
}

CbCheckpoint cb_checkpoint(void) {
    CbCheckpoint cp;
    cp.code_len = cb_pc();
    cp.data_len = data_buf.len;
    cp.relocs_len = relocs.len;
    cp.data_init_relocs_len = data_init_relocs.len;
    cp.ext_relocs_len = ext_relocs.len;
    return cp;
}

void cb_restore(CbCheckpoint cp) {
    if (cp.code_len < flushed_len + flush_watermark)
        ir4_die("code restore before flush watermark");
    code_buf.len = cp.code_len - flushed_len;
    data_buf.len = cp.data_len;
    relocs.len = cp.relocs_len;
    data_init_relocs.len = cp.data_init_relocs_len;
    ext_relocs.len = cp.ext_relocs_len;
}

static void patch_global_bytes(size_t patch_offset, const u8 *bytes, size_t len) {
    size_t total = cb_pc();
    if (patch_offset + len > total)
        ir4_die("patch offset out of bounds");
    if (patch_offset < flushed_len) {
        size_t file_n = flushed_len - patch_offset;
        if (file_n > len)
            file_n = len;
        if (fseek(spill_fp, (long)patch_offset, SEEK_SET) != 0 ||
            fwrite(bytes, 1, file_n, spill_fp) != file_n) {
            ir4_die("failed patching code spill file");
        }
        bytes += file_n;
        patch_offset += file_n;
        len -= file_n;
    }
    if (len > 0) {
        memcpy(code_buf.data + (patch_offset - flushed_len), bytes, len);
    }
}

void cb_patch_globally8(size_t patch_offset, u8 val) {
    patch_global_bytes(patch_offset, &val, 1);
}

void cb_patch_globally16(size_t patch_offset, u16 target_addr) {
    u8 b[2];
    b[0] = (u8)(target_addr & 0xFFU);
    b[1] = (u8)((target_addr >> 8) & 0xFFU);
    patch_global_bytes(patch_offset, b, 2);
}

void cb_patch_globally24(size_t patch_offset, u32 target_addr) {
    u8 b[3];
    b[0] = (u8)(target_addr & 0xFFU);
    b[1] = (u8)((target_addr >> 8) & 0xFFU);
    b[2] = (u8)((target_addr >> 16) & 0xFFU);
    patch_global_bytes(patch_offset, b, 3);
}

void cb_patch_globally32(size_t patch_offset, u32 val) {
    u8 b[4];
    cb_put_le32(b, val);
    patch_global_bytes(patch_offset, b, 4);
}

void cb_patch_locally8(size_t patch_offset, u8 val) {
    size_t min_off = flushed_len + flush_watermark;
    if (patch_offset < min_off || patch_offset - flushed_len >= code_buf.len) {
        ir4_die("local patch offset out of bounds");
    }
    code_buf.data[patch_offset - flushed_len] = val;
}

void cb_patch_locally16(size_t patch_offset, u16 target_addr) {
    size_t loc;
    size_t min_off = flushed_len + flush_watermark;
    if (patch_offset < min_off || patch_offset - flushed_len + 1 >= code_buf.len) {
        ir4_die("local patch offset out of bounds");
    }
    loc = patch_offset - flushed_len;
    code_buf.data[loc] = (u8)(target_addr & 0xFF);
    code_buf.data[loc + 1] = (u8)((target_addr >> 8) & 0xFF);
}

void cb_patch_locally24(size_t patch_offset, u32 target_addr) {
    size_t loc;
    size_t min_off = flushed_len + flush_watermark;
    if (patch_offset < min_off || patch_offset - flushed_len + 2 >= code_buf.len) {
        ir4_die("local patch offset out of bounds");
    }
    loc = patch_offset - flushed_len;
    code_buf.data[loc] = (u8)(target_addr & 0xFFU);
    code_buf.data[loc + 1] = (u8)((target_addr >> 8) & 0xFFU);
    code_buf.data[loc + 2] = (u8)((target_addr >> 16) & 0xFFU);
}

void cb_patch_locally32(size_t patch_offset, u32 val) {
    cb_patch_locally16(patch_offset, (u16)(val & 0xFFFFU));
    cb_patch_locally16(patch_offset + 2, (u16)((val >> 16) & 0xFFFFU));
}

void cb_put_le16(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFFU);
    p[1] = (u8)((v >> 8) & 0xFFU);
}

void cb_put_le32(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFFUL);
    p[1] = (u8)((v >> 8) & 0xFFUL);
    p[2] = (u8)((v >> 16) & 0xFFUL);
    p[3] = (u8)((v >> 24) & 0xFFUL);
}

Bool cb_write_raw(FILE *fp, Bool include_data) {
    if (spill_fp && fp == spill_fp) {
        cb_flush();
        if (fseek(fp, (long)flushed_len, SEEK_SET) != 0)
            return false;
    } else if (!cb_write_code(fp, cb_pc())) {
        return false;
    }
    if (include_data && data_init_size > 0 &&
        fwrite(data_buf.data, 1, data_init_size, fp) != data_init_size) {
        return false;
    }
    return true;
}

/* Data segment */

size_t cb_alloc_data(size_t size, u8 align) {
    size_t aligned = data_buf.len;
    size_t new_len;
    if (align > 1) {
        if ((align & (align - 1)) == 0) {
            size_t mask = (size_t)(align - 1);
            aligned = (aligned + mask) & ~mask;
        } else {
            size_t rem = aligned % align;
            if (rem != 0)
                aligned += align - rem;
        }
    }
    new_len = aligned + size;
    if (new_len > data_buf.len) {
        VEC_ENSURE(data_buf, new_len);
        memset(data_buf.data + data_buf.len, 0, new_len - data_buf.len);
        data_buf.len = new_len;
    }
    return aligned;
}

size_t cb_data_size(void) { return data_buf.len; }

void cb_emit_data_u8(u8 b) {
    VEC_PUSH(data_buf, b);
    data_init_size = data_buf.len;
}

void cb_emit_data_u16(u16 w) {
    cb_emit_data_u8((u8)(w & 0xFF));
    cb_emit_data_u8((u8)((w >> 8) & 0xFF));
}

void cb_emit_data_word(i32 val, u8 word_bytes) {
    u32 lo = (u32)val;
    u8 fill = (val < 0) ? 0xFF : 0x00;
    u8 i;
    for (i = 0; i < word_bytes && i < 4; i++) {
        cb_emit_data_u8((u8)(lo & 0xFF));
        lo >>= 8;
    }
    for (; i < word_bytes; i++) {
        cb_emit_data_u8(fill);
    }
}

const u8 *cb_init_data(void) { return data_buf.data; }

size_t cb_init_data_size(void) { return data_init_size; }

/* Relocations */

void cb_add_data_reloc(size_t patch_offset, size_t data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = false;
    r->width = 2;
    r->off1 = patch_offset;
    r->off2 = 0;
    r->data_offset = data_offset;
}

void cb_add_data_reloc24(size_t patch_offset, size_t data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = false;
    r->width = 3;
    r->off1 = patch_offset;
    r->off2 = 0;
    r->data_offset = data_offset;
}

void cb_add_data_reloc32(size_t patch_offset, size_t data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = false;
    r->width = 4;
    r->off1 = patch_offset;
    r->off2 = 0;
    r->data_offset = data_offset;
}

void cb_add_data_reloc_bank(size_t patch_offset, size_t data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = false;
    r->width = 1;
    r->off1 = patch_offset;
    r->off2 = 0;
    r->data_offset = data_offset;
}

void cb_add_data_reloc_split(size_t lo_offset, size_t hi_offset, size_t data_offset) {
    DataReloc *r = VEC_ADD(relocs);
    r->split = true;
    r->width = 2;
    r->off1 = lo_offset;
    r->off2 = hi_offset;
    r->data_offset = data_offset;
}

void cb_patch_data_init_relocs_base(u32 code_base, u32 data_base) {
    size_t i;
    for (i = 0; i < data_init_relocs.len; i++) {
        size_t val = data_init_relocs.data[i].is_code
                         ? ((size_t)code_base + data_init_relocs.data[i].target_val)
                         : ((size_t)data_base + data_init_relocs.data[i].target_val);
        size_t off = data_init_relocs.data[i].patch_data_off;
        u8 b;
        for (b = 0; b < data_init_relocs.data[i].addr_bytes; b++) {
            data_buf.data[off + b] = (u8)(val & 0xFF);
            val >>= 8;
        }
    }
}

void cb_patch_data_relocs(size_t data_base) {
    size_t i;
    size_t heap_off = cb_data_size();
    for (i = 0; i < relocs.len; i++) {
        size_t off = relocs.data[i].data_offset;
        size_t addr = data_base + (off == CB_HEAP_START_OFF ? heap_off : off);
        if (relocs.data[i].split) {
            cb_patch_globally8(relocs.data[i].off1, (u8)(addr & 0xFF));
            cb_patch_globally8(relocs.data[i].off2, (u8)((addr >> 8) & 0xFF));
        } else if (relocs.data[i].width == 1) {
            cb_patch_globally8(relocs.data[i].off1, (u8)((addr >> 16) & 0xFF));
        } else if (relocs.data[i].width == 4) {
            cb_patch_globally32(relocs.data[i].off1, (u32)addr);
        } else if (relocs.data[i].width == 3) {
            cb_patch_globally24(relocs.data[i].off1, (u32)addr);
        } else {
            cb_patch_globally16(relocs.data[i].off1, (u16)addr);
        }
    }
    cb_patch_data_init_relocs_base(0, (u32)data_base);
}

void cb_add_data_init_reloc(size_t patch_data_off, size_t target_val, u8 addr_bytes, Bool is_code) {
    DataInitReloc *r = VEC_ADD(data_init_relocs);
    r->patch_data_off = patch_data_off;
    r->target_val = target_val;
    r->addr_bytes = addr_bytes;
    r->is_code = is_code;
}

size_t cb_data_init_reloc_count(void) { return data_init_relocs.len; }

void cb_get_data_init_reloc(size_t idx, size_t *patch_data_off, size_t *target_val, u8 *addr_bytes,
                            Bool *is_code) {
    *patch_data_off = data_init_relocs.data[idx].patch_data_off;
    *target_val = data_init_relocs.data[idx].target_val;
    *addr_bytes = data_init_relocs.data[idx].addr_bytes;
    *is_code = data_init_relocs.data[idx].is_code;
}

void cb_patch_data_init_reloc(size_t idx, size_t code_addr) {
    data_init_relocs.data[idx].target_val = code_addr;
}

void cb_add_extern_reloc(size_t patch_offset, const char *name) {
    ExternReloc *r = VEC_ADD(ext_relocs);
    r->patch_offset = patch_offset;
    r->name = intern_str(name);
}

size_t cb_extern_reloc_count(void) { return ext_relocs.len; }

const char *cb_extern_reloc_name(size_t idx, size_t *patch_offset) {
    *patch_offset = ext_relocs.data[idx].patch_offset;
    return ext_relocs.data[idx].name;
}

/* Banked code */

void cb_set_bank_config(size_t bank_base, size_t bank_size) {
    cfg_bank_base = bank_base;
    cfg_bank_size = bank_size;
}

Bool cb_is_banked(void) { return cfg_bank_size != 0; }

size_t cb_bank_base(void) { return cfg_bank_base; }

size_t cb_bank_size(void) { return cfg_bank_size; }

void cb_begin_bank(u16 bank_id) {
    size_t pc = cb_pc();
    if (cur_bank_id < banks.len) {
        banks.data[cur_bank_id].len = pc - banks.data[cur_bank_id].start_pc;
    }
    while (banks.len <= bank_id) {
        BankDesc *b = VEC_ADD(banks);
        b->base = (banks.len == 1) ? base_addr : cfg_bank_base;
        b->start_pc = pc;
        b->len = 0;
    }
    cur_bank_id = bank_id;
    banks.data[bank_id].base = (bank_id == 0) ? base_addr : cfg_bank_base;
    banks.data[bank_id].start_pc = pc;
    banks.data[bank_id].len = 0;
}

void cb_end_banks(void) {
    if (cur_bank_id < banks.len) {
        banks.data[cur_bank_id].len = cb_pc() - banks.data[cur_bank_id].start_pc;
    }
}

u16 cb_cur_bank(void) { return cur_bank_id; }

size_t cb_bank_count(void) { return banks.len; }

BankDesc cb_get_bank(u16 bank_id) {
    BankDesc d;
    if (bank_id >= banks.len) {
        d.base = 0;
        d.start_pc = 0;
        d.len = 0;
        return d;
    }
    d = banks.data[bank_id];
    if (bank_id == cur_bank_id) {
        d.len = cb_pc() - d.start_pc;
    }
    return d;
}
