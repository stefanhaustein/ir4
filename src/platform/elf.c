/* elf.c - ELF32 ET_EXEC (--virt) & ET_REL (--obj) writers for RV32 (Strict C89) */
#include "codebuf.h"
#include "frontend.h"
#include "backend/rv32.h"

#define VIRT_BASE 0x80000000UL
#define VIRT_UART0 0x10000000UL
#define VIRT_TEST 0x00100000UL
#define ELF_R_RISCV_HI20 26U
#define ELF_R_RISCV_LO12_I 27U
#define RV32_DSTACK_SIZE 4096U

static u16 obj_main_off = 0;
static u16 obj_main_size = 0;
static int obj_has_main = 0;
static u16 obj_s1_reloc = 0;
static u16 obj_s2_reloc = 0;
static int obj_has_s2_reloc = 0;

static void init_elf32_hdr(u8 *hdr, u16 e_type) {
    memset(hdr, 0, 52);
    hdr[0] = 0x7F;
    hdr[1] = 'E';
    hdr[2] = 'L';
    hdr[3] = 'F';
    hdr[4] = 1; /* ELFCLASS32 */
    hdr[5] = 1; /* ELFDATA2LSB */
    hdr[6] = 1; /* EV_CURRENT */
    put_le16(hdr + 16, e_type);
    put_le16(hdr + 18, 243); /* EM_RISCV */
    put_le32(hdr + 20, 1);   /* EV_CURRENT */
    put_le16(hdr + 40, 52);  /* e_ehsize */
}

/* --- QEMU virt (ET_EXEC) --- */

static void virt_begin(size_t base) {
    (void)base;
    /* Placeholder JAL x0, entry_off; patched by virt_end() */
    emit32(enc_j(0, 0));
}

static int virt_write(FILE *fp, size_t base) {
    u8 hdr[84];
    u32 filesz = (u32)cb_pc() + (u32)cb_init_data_size();
    u32 memsz = (u32)cb_pc() + (u32)cb_data_size();
    (void)base;
    init_elf32_hdr(hdr, 2);        /* ET_EXEC */
    put_le32(hdr + 24, VIRT_BASE); /* e_entry */
    put_le32(hdr + 28, 52);        /* e_phoff */
    put_le16(hdr + 42, 32);        /* e_phentsize */
    put_le16(hdr + 44, 1);         /* e_phnum */

    /* Program Header (32 bytes): PT_LOAD */
    memset(hdr + 52, 0, 32);
    put_le32(hdr + 52, 1);         /* p_type = PT_LOAD */
    put_le32(hdr + 56, 84);        /* p_offset */
    put_le32(hdr + 60, VIRT_BASE); /* p_vaddr */
    put_le32(hdr + 64, VIRT_BASE); /* p_paddr */
    put_le32(hdr + 68, filesz);    /* p_filesz */
    put_le32(hdr + 72, memsz);     /* p_memsz */
    put_le32(hdr + 76, 7);         /* p_flags = PF_R|PF_W|PF_X */
    put_le32(hdr + 80, 4);         /* p_align */

    if (fwrite(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
        return 0;
    return cb_write_raw(fp, 1);
}

static void virt_end(int main_idx) {
    const FuncSig *sig = ir4_func_sig((u8)main_idx);
    u16 entry_off = cb_pc();
    u16 db_patch_off;
    u32 data_addr;
    u32 hi;
    i32 lo;

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--virt requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    patch32(0, enc_j((i32)entry_off, 0));

    /* sp = 0x80200000, s1 (data stack) = 0x80100000 */
    emit32(enc_u(0x80200000UL, 2, 0x37UL)); /* LUI sp, 0x80200 */
    emit32(enc_u(0x80100000UL, 9, 0x37UL)); /* LUI s1, 0x80100 */

    /* s2 (x18) = data base address; patched after stub */
    db_patch_off = cb_pc();
    emit32(enc_u(0, 18, 0x37UL));        /* LUI s2, 0 */
    emit32(enc_i(0, 18, 0, 18, 0x13UL)); /* ADDI s2, s2, 0 */

    /* JAL ra, main */
    emit32(enc_j((i32)sig->addr - (i32)cb_pc(), 1));

    if (sig->out_types.len == 1) {
        u16 div_loop;
        u16 print_loop;
        emit32(enc_i(0, 9, 2, 10, 0x03UL));   /* LW a0, 0(s1) */
        emit32(enc_i(4, 9, 0, 9, 0x13UL));    /* ADDI s1, s1, 4 */
        emit32(enc_u(VIRT_UART0, 7, 0x37UL)); /* LUI t2, 0x10000 */

        emit32(enc_b(16, 0, 10, 5));               /* BGE a0, x0, +16 */
        emit32(enc_i('-', 0, 0, 5, 0x13UL));       /* ADDI t0, x0, '-' */
        emit32(enc_s(0, 5, 7, 0));                 /* SB t0, 0(t2) */
        emit32(enc_r(0x20, 10, 0, 0, 10, 0x33UL)); /* SUB a0, x0, a0 */

        emit32(enc_i(0, 2, 0, 28, 0x13UL)); /* ADDI t3, sp, 0 */
        emit32(enc_i(10, 0, 0, 6, 0x13UL)); /* ADDI t1, x0, 10 */

        div_loop = cb_pc();
        emit32(enc_r(0x01, 6, 10, 7, 5, 0x33UL));              /* REMU t0, a0, t1 */
        emit32(enc_r(0x01, 6, 10, 5, 10, 0x33UL));             /* DIVU a0, a0, t1 */
        emit32(enc_i('0', 5, 0, 5, 0x13UL));                   /* ADDI t0, t0, '0' */
        emit32(enc_i(-4, 2, 0, 2, 0x13UL));                    /* ADDI sp, sp, -4 */
        emit32(enc_s(0, 5, 2, 2));                             /* SW t0, 0(sp) */
        emit32(enc_b((i32)div_loop - (i32)cb_pc(), 0, 10, 1)); /* BNE a0, x0, div_loop */

        print_loop = cb_pc();
        emit32(enc_i(0, 2, 2, 5, 0x03UL));                       /* LW t0, 0(sp) */
        emit32(enc_i(4, 2, 0, 2, 0x13UL));                       /* ADDI sp, sp, 4 */
        emit32(enc_s(0, 5, 7, 0));                               /* SB t0, 0(t2) */
        emit32(enc_b((i32)print_loop - (i32)cb_pc(), 28, 2, 1)); /* BNE sp, t3, print_loop */

        emit32(enc_i('\n', 0, 0, 5, 0x13UL)); /* ADDI t0, x0, '\n' */
        emit32(enc_s(0, 5, 7, 0));            /* SB t0, 0(t2) */
    }

    /* Exit QEMU virt cleanly via sifive_test (0x100000 <- 0x5555) */
    emit32(enc_u(VIRT_TEST, 5, 0x37UL));    /* LUI t0, 0x100 */
    emit32(enc_u(0x00005000UL, 6, 0x37UL)); /* LUI t1, 0x5 */
    emit32(enc_i(0x555, 6, 0, 6, 0x13UL));  /* ADDI t1, t1, 0x555 */
    emit32(enc_s(0, 6, 5, 2));              /* SW t1, 0(t0) */
    emit32(enc_j(0, 0));                    /* J . */

    data_addr = VIRT_BASE + (u32)cb_pc();
    cb_patch_data_init_relocs_base(VIRT_BASE, data_addr);
    hi = (data_addr + 0x800UL) & 0xFFFFF000UL;
    lo = (i32)(data_addr - hi);
    patch32(db_patch_off, enc_u(hi, 18, 0x37UL));
    patch32((u16)(db_patch_off + 4), enc_i(lo, 18, 0, 18, 0x13UL));
}

/* --- Relocatable Object (ET_REL, .o) --- */

static int write_zeros(FILE *fp, u32 n) {
    u8 z[64];
    memset(z, 0, sizeof(z));
    while (n > 0) {
        u32 chunk = n > sizeof(z) ? (u32)sizeof(z) : n;
        if (fwrite(z, 1, chunk, fp) != chunk)
            return 0;
        n -= chunk;
    }
    return 1;
}

static void add_rela(u8 *buf, u32 *cnt, u32 off, u32 sym, u32 type, i32 addend) {
    u8 *p = buf + (*cnt) * 12U;
    put_le32(p + 0, off);
    put_le32(p + 4, (sym << 8) | (type & 0xFFU));
    put_le32(p + 8, (u32)addend);
    (*cnt)++;
}

static void add_sym(u8 *buf, u32 *cnt, u32 name_off, u32 val, u32 size, u8 bind, u8 type,
                    u16 shndx) {
    u8 *p = buf + (*cnt) * 16U;
    put_le32(p + 0, name_off);
    put_le32(p + 4, val);
    put_le32(p + 8, size);
    p[12] = (u8)((bind << 4) | (type & 0x0FU));
    p[13] = 0;
    put_le16(p + 14, shndx);
    (*cnt)++;
}

static void write_shdr(u8 *p, u32 name, u32 type, u32 flags, u32 off, u32 size, u32 link, u32 info,
                       u32 align, u32 entsize) {
    memset(p, 0, 40);
    put_le32(p + 0, name);
    put_le32(p + 4, type);
    put_le32(p + 8, flags);
    put_le32(p + 16, off);
    put_le32(p + 20, size);
    put_le32(p + 24, link);
    put_le32(p + 28, info);
    put_le32(p + 32, align);
    put_le32(p + 36, entsize);
}

static void obj_end(int main_idx) {
    obj_has_main = 0;
    obj_has_s2_reloc = 0;
    if (main_idx < 0)
        return;

    {
        const FuncSig *sig = ir4_func_sig((u8)main_idx);
        if (sig->in_types.len != 0 || sig->out_types.len > 1) {
            fprintf(stderr, "--obj requires 'main' with 0 inputs and at most 1 output\n");
            exit(1);
        }

        obj_has_main = 1;
        obj_main_off = cb_pc();

        emit32(enc_i(-16, 2, 0, 2, 0x13UL)); /* ADDI sp, sp, -16 */
        emit32(enc_s(12, 1, 2, 2));          /* SW ra, 12(sp) */
        emit32(enc_s(8, 9, 2, 2));           /* SW s1, 8(sp) */
        emit32(enc_s(4, 18, 2, 2));          /* SW s2, 4(sp) */

        /* s1 = .bss + 4096 */
        obj_s1_reloc = cb_pc();
        emit32(enc_u(0, 9, 0x37UL));       /* LUI s1, 0 */
        emit32(enc_i(0, 9, 0, 9, 0x13UL)); /* ADDI s1, s1, 0 */

        /* s2 = .data + 0 */
        if (cb_data_size() > 0) {
            obj_has_s2_reloc = 1;
            obj_s2_reloc = cb_pc();
            emit32(enc_u(0, 18, 0x37UL));        /* LUI s2, 0 */
            emit32(enc_i(0, 18, 0, 18, 0x13UL)); /* ADDI s2, s2, 0 */
        }

        emit32(enc_j((i32)sig->addr - (i32)cb_pc(), 1)); /* JAL ra, ir4 main */

        if (sig->out_types.len == 1) {
            emit32(enc_i(0, 9, 2, 10, 0x03UL)); /* LW a0, 0(s1) */
            emit32(enc_i(4, 9, 0, 9, 0x13UL));  /* ADDI s1, s1, 4 */
        } else {
            emit32(enc_i(0, 0, 0, 10, 0x13UL)); /* ADDI a0, x0, 0 */
        }

        emit32(enc_i(4, 2, 2, 18, 0x03UL)); /* LW s2, 4(sp) */
        emit32(enc_i(8, 2, 2, 9, 0x03UL));  /* LW s1, 8(sp) */
        emit32(enc_i(12, 2, 2, 1, 0x03UL)); /* LW ra, 12(sp) */
        emit32(enc_i(16, 2, 0, 2, 0x13UL)); /* ADDI sp, sp, 16 */
        emit32(enc_i(0, 1, 0, 0, 0x67UL));  /* RET */

        obj_main_size = (u16)(cb_pc() - obj_main_off);
    }
}

static int obj_write(FILE *fp, size_t base) {
    static const char shstrtab[] = "\0.text\0.data\0.bss\0.rela.text\0.symtab\0.strtab\0.shstrtab";
    u8 ehdr[52];
    u8 shdrs[8 * 40];
    u8 symbuf[260 * 16];
    u8 relabuf[520 * 12];
    char strtab[4096];
    const char *ext_names[256];
    u32 ext_syms[256];
    u32 ext_cnt = 0;
    u32 sym_cnt = 0;
    u32 rela_cnt = 0;
    u32 str_len = 1;
    u32 text_sz = (u32)cb_pc();
    u32 text_pad = (4U - (text_sz & 3U)) & 3U;
    u32 data_sz = (u32)cb_data_size();
    u32 init_sz = (u32)cb_init_data_size();
    u32 data_pad = (4U - (data_sz & 3U)) & 3U;
    u32 rela_sz, sym_sz, shstr_sz, str_pad;
    u32 off_text, off_data, off_rela, off_sym, off_str, off_shstr, off_shdr;
    size_t reloc_cnt = cb_extern_reloc_count();
    size_t i;
    (void)base;

    strtab[0] = '\0';
    memset(symbuf, 0, sizeof(symbuf));
    memset(relabuf, 0, sizeof(relabuf));

    /* Local symbols: 0=UNDEF, 1=.data section, 2=.bss section */
    add_sym(symbuf, &sym_cnt, 0, 0, 0, 0, 0, 0);
    add_sym(symbuf, &sym_cnt, 0, 0, 0, 0, 3, 2);
    add_sym(symbuf, &sym_cnt, 0, 0, 0, 0, 3, 3);

    if (obj_has_main) {
        u32 name_off = str_len;
        memcpy(strtab + str_len, "main", 5);
        str_len += 5U;
        add_sym(symbuf, &sym_cnt, name_off, obj_main_off, obj_main_size, 1, 2, 1);

        add_rela(relabuf, &rela_cnt, obj_s1_reloc, 2, ELF_R_RISCV_HI20, (i32)RV32_DSTACK_SIZE);
        add_rela(relabuf, &rela_cnt, (u32)obj_s1_reloc + 4U, 2, ELF_R_RISCV_LO12_I,
                 (i32)RV32_DSTACK_SIZE);
        if (obj_has_s2_reloc) {
            add_rela(relabuf, &rela_cnt, obj_s2_reloc, 1, ELF_R_RISCV_HI20, 0);
            add_rela(relabuf, &rela_cnt, (u32)obj_s2_reloc + 4U, 1, ELF_R_RISCV_LO12_I, 0);
        }
    }

    for (i = 0; i < reloc_cnt; i++) {
        size_t patch_off = 0;
        const char *name = cb_extern_reloc_name(i, &patch_off);
        u32 sym_idx = 0;
        u32 j;
        for (j = 0; j < ext_cnt; j++) {
            if (strcmp(ext_names[j], name) == 0) {
                sym_idx = ext_syms[j];
                break;
            }
        }
        if (sym_idx == 0) {
            u32 len = (u32)strlen(name) + 1U;
            if (ext_cnt >= 256U || str_len + len > sizeof(strtab)) {
                ir4_die("too many extern symbols for RV32 object");
            }
            sym_idx = sym_cnt;
            ext_names[ext_cnt] = name;
            ext_syms[ext_cnt] = sym_idx;
            ext_cnt++;
            memcpy(strtab + str_len, name, len);
            add_sym(symbuf, &sym_cnt, str_len, 0, 0, 1, 0, 0);
            str_len += len;
        }
        add_rela(relabuf, &rela_cnt, patch_off, sym_idx, ELF_R_RISCV_HI20, 0);
        add_rela(relabuf, &rela_cnt, (u32)patch_off + 4U, sym_idx, ELF_R_RISCV_LO12_I, 0);
    }

    rela_sz = rela_cnt * 12U;
    sym_sz = sym_cnt * 16U;
    shstr_sz = (u32)sizeof(shstrtab);
    str_pad = (4U - ((str_len + shstr_sz) & 3U)) & 3U;

    off_text = 52U;
    off_data = off_text + text_sz + text_pad;
    off_rela = off_data + data_sz + data_pad;
    off_sym = off_rela + rela_sz;
    off_str = off_sym + sym_sz;
    off_shstr = off_str + str_len;
    off_shdr = off_shstr + shstr_sz + str_pad;

    init_elf32_hdr(ehdr, 1);       /* ET_REL */
    put_le32(ehdr + 32, off_shdr); /* e_shoff */
    put_le16(ehdr + 46, 40);       /* e_shentsize */
    put_le16(ehdr + 48, 8);        /* e_shnum */
    put_le16(ehdr + 50, 7);        /* e_shstrndx */

    memset(shdrs, 0, sizeof(shdrs));
    write_shdr(shdrs + 1 * 40, 1, 1, 6, off_text, text_sz, 0, 0, 4, 0);            /* .text */
    write_shdr(shdrs + 2 * 40, 7, 1, 3, off_data, data_sz, 0, 0, 4, 0);            /* .data */
    write_shdr(shdrs + 3 * 40, 13, 8, 3, off_rela, RV32_DSTACK_SIZE, 0, 0, 16, 0); /* .bss */
    write_shdr(shdrs + 4 * 40, 18, 4, 0x40U, off_rela, rela_sz, 5, 1, 4, 12);      /* .rela.text */
    write_shdr(shdrs + 5 * 40, 29, 2, 0, off_sym, sym_sz, 6, 3, 4, 16);            /* .symtab */
    write_shdr(shdrs + 6 * 40, 37, 3, 0, off_str, str_len, 0, 0, 1, 0);            /* .strtab */
    write_shdr(shdrs + 7 * 40, 45, 3, 0, off_shstr, shstr_sz, 0, 0, 1, 0);         /* .shstrtab */

    if (fwrite(ehdr, 1, sizeof(ehdr), fp) != sizeof(ehdr))
        return 0;
    if (text_sz > 0 && fwrite(cb_data(), 1, text_sz, fp) != text_sz)
        return 0;
    if (!write_zeros(fp, text_pad))
        return 0;
    if (init_sz > 0 && fwrite(cb_init_data(), 1, init_sz, fp) != init_sz)
        return 0;
    if (!write_zeros(fp, (data_sz - init_sz) + data_pad))
        return 0;
    if (rela_sz > 0 && fwrite(relabuf, 1, rela_sz, fp) != rela_sz)
        return 0;
    if (fwrite(symbuf, 1, sym_sz, fp) != sym_sz)
        return 0;
    if (fwrite(strtab, 1, str_len, fp) != str_len)
        return 0;
    if (fwrite(shstrtab, 1, shstr_sz, fp) != shstr_sz)
        return 0;
    if (!write_zeros(fp, str_pad))
        return 0;
    if (fwrite(shdrs, 1, sizeof(shdrs), fp) != sizeof(shdrs))
        return 0;
    return 1;
}

const Platform plat_rv32_virt = {"--virt",   &be_rv32, 0x0000,     "virt", 1,
                                 virt_begin, virt_end, virt_write, NULL};

const Platform plat_rv32_obj = {"--obj", &be_rv32, 0x0000, NULL, 0, NULL, obj_end, obj_write, NULL};
