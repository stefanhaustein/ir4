/* jit.c - Apple Silicon macOS MAP_JIT Runner for IR4th */
#include "codebuf.h"
#include "frontend.h"

static const char *jit_prog_name = NULL;
static int jit_user_argc = 0;
static char **jit_user_argv = NULL;

void ir4_jit_set_args(const char *prog, int argc, char **argv) {
    jit_prog_name = prog;
    jit_user_argc = (argc > 0 && argv) ? argc : 0;
    jit_user_argv = jit_user_argc ? argv : NULL;
}

#if defined(__APPLE__) && defined(__aarch64__)
#include <dlfcn.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>

static long jit_get_argc(void) { return jit_prog_name ? (long)(1 + jit_user_argc) : 0; }

static const char *jit_get_argv(long idx) {
    if (idx == 0)
        return jit_prog_name;
    if (idx > 0 && idx <= (long)jit_user_argc)
        return jit_user_argv[idx - 1];
    return NULL;
}

/* Tiny ARM64 C-to-IR4th wrapper emitted at the end of code_buf:
 *   STP X29, X30, [SP, #-16]!
 *   STR X28, [SP, #-16]!
 *   MOV X28, X0         (data_base passed as arg0)
 *   BL  <main_addr>
 *   LDR X0, [SP], #16   (or MOV X0, #0 if main returns void)
 *   LDR X28, [SP], #16
 *   LDP X29, X30, [SP], #16
 *   RET
 */
static void arm64_run_jit(size_t main_idx) {
    const FuncSig *sig = ir4_func_sig(main_idx);
    size_t main_addr = sig->addr;
    size_t entry_pc = cb_pc();
    i32 diff;
    u32 imm26;
    void *mem;
    void *data_mem = NULL;
    long (*jit_fn)(void *);
    long result;

    if (sig->in_types.len != 0 || sig->out_types.len > 1) {
        fprintf(stderr, "--run requires 'main' with 0 inputs and at most 1 output\n");
        exit(1);
    }

    /* Emit C-ABI trampoline calling IR4th 'main' and popping result into X0 */
    cb_emit16(0x7BFD);
    cb_emit16(0xA9BF); /* STP X29, X30, [SP, #-16]! */
    cb_emit16(0x0FFC);
    cb_emit16(0xF81F); /* STR X28, [SP, #-16]! */
    cb_emit16(0x03FC);
    cb_emit16(0xAA00); /* MOV X28, X0 */
    diff = (i32)main_addr - (i32)cb_cur_addr();
    imm26 = ((u32)(diff >> 2)) & 0x03FFFFFFUL;
    cb_emit16((u16)((0x94000000UL | imm26) & 0xFFFFU));
    cb_emit16((u16)(((0x94000000UL | imm26) >> 16) & 0xFFFFU));
    if (sig->out_types.len == 1) {
        cb_emit16(0x07E0);
        cb_emit16(0xF841); /* LDR X0, [SP], #16 */
    } else {
        cb_emit16(0x0000);
        cb_emit16(0xD280); /* MOV X0, #0 */
    }
    cb_emit16(0x07FC);
    cb_emit16(0xF841); /* LDR X28, [SP], #16 */
    cb_emit16(0x7BFD);
    cb_emit16(0xA8C1); /* LDP X29, X30, [SP], #16 */
    cb_emit16(0x03C0);
    cb_emit16(0xD65F); /* RET */

    {
        size_t map_sz = cb_pc() ? cb_pc() : 1;
        mem = mmap(NULL, map_sz, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT,
                   -1, 0);
        if (mem == MAP_FAILED) {
            perror("mmap MAP_JIT");
            exit(1);
        }

        {
            size_t alloc_sz = cb_data_size() + (1024 * 1024);
            data_mem = calloc(1, alloc_sz);
            if (!data_mem) {
                perror("calloc data");
                munmap(mem, map_sz);
                exit(1);
            }
            if (cb_init_data_size() > 0) {
                memcpy(data_mem, cb_init_data(), cb_init_data_size());
            }
            {
                size_t i;
                for (i = 0; i < cb_data_init_reloc_count(); i++) {
                    size_t patch_off;
                    size_t target_val;
                    u8 addr_bytes;
                    Bool is_code;
                    unsigned long addr;
                    cb_get_data_init_reloc(i, &patch_off, &target_val, &addr_bytes, &is_code);
                    addr = (unsigned long)((u8 *)(is_code ? mem : data_mem) + target_val);
                    if (addr_bytes == 8) {
                        memcpy((u8 *)data_mem + patch_off, &addr, 8);
                    }
                }
            }
        }

        {
            size_t i;
            for (i = 0; i < cb_extern_reloc_count(); i++) {
                size_t off;
                const char *name = cb_extern_reloc_name(i, &off);
                void *sym;
                unsigned long addr;
                if (strcmp(name, "__ir4_jit_argc") == 0) {
                    sym = (void *)jit_get_argc;
                } else if (strcmp(name, "__ir4_jit_argv") == 0) {
                    sym = (void *)jit_get_argv;
                } else {
                    sym = dlsym(RTLD_DEFAULT, name);
                }
                if (!sym) {
                    fprintf(stderr, "JIT: unresolved extern '%s'\n", name);
                    exit(1);
                }
                addr = (unsigned long)sym;
                cb_patch32(off, (u32)(0xD2800009UL | (((addr >> 0) & 0xFFFFUL) << 5)));
                cb_patch32(off + 4, (u32)(0xF2A00009UL | (((addr >> 16) & 0xFFFFUL) << 5)));
                cb_patch32(off + 8, (u32)(0xF2C00009UL | (((addr >> 32) & 0xFFFFUL) << 5)));
                cb_patch32(off + 12, (u32)(0xF2E00009UL | (((addr >> 48) & 0xFFFFUL) << 5)));
            }
        }

        pthread_jit_write_protect_np(0);
        memcpy(mem, cb_data(), cb_pc());
        pthread_jit_write_protect_np(1);
        sys_icache_invalidate(mem, cb_pc());

        jit_fn = (long (*)(void *))((u8 *)mem + entry_pc);
        result = jit_fn(data_mem);
        printf("JIT main() returned: %ld\n", result);

        free(data_mem);
        munmap(mem, map_sz);
    }
}
#else
static void arm64_run_jit(size_t main_idx) {
    (void)main_idx;
    fprintf(stderr, "--run is only supported on Apple Silicon macOS\n");
    exit(1);
}
#endif

const Platform plat_arm64_jit = {"--run", &be_arm64, 0x0000, NULL,         1,
                                 NULL,    NULL,      NULL,   arm64_run_jit};
