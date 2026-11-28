/* ir4c.c - CLI driver for IR4th */
#include "codebuf.h"
#include "frontend.h"
#include "path.h"
#if IR4_ENABLE_BC
#include "backend/bc.h"
#endif

static const Platform *const platforms[] = {
#if IR4_ENABLE_CPM
    &plat_z80_cpm,
#endif
#if IR4_ENABLE_ZX81
    &plat_z80_zx81,
#endif
#if IR4_ENABLE_AGON
    &plat_ez80_agon,
#endif
#if IR4_ENABLE_SIM65
    &plat_6502_sim,
#endif
#if IR4_ENABLE_SIM65816
    &plat_65816_sim,
#endif
#if IR4_ENABLE_X16GS
    &plat_65816_x16gs,
#endif
#if IR4_ENABLE_C64
    &plat_6502_c64,
#endif
#if IR4_ENABLE_X16
    &plat_6502_x16, &plat_6502_x16_cart,
#endif
#if IR4_ENABLE_JIT
    &plat_arm64_jit,
#endif
#if IR4_ENABLE_ELF
    &plat_rv32_virt, &plat_rv32_obj,
#endif
#if IR4_ENABLE_BC
    &plat_bc32, &plat_web, &plat_web_js,
#endif
    NULL};

static void print_usage(void) {
    fprintf(stderr, "Usage: ir4c [-t z80|ez80|6502|arm64|rv32|bc|cpm|zx81|agon|sim65|c64|x16|x16-cart|virt|obj] "
                    "[-b hex_addr] [-o out.bin] [-I dir] [-O0|-O1] [--platform name] "
                    "[--zp hex_addr] [--stack-zp hex_addr] [--no-stack-zp] [--run] <input.ir4> [-- <args...>]\n");
}

#ifdef IR4_INTERACTIVE_ARGS
int main(void) {
    int argc = 1;
    char *argv_buf[2];
    char **argv = argv_buf;
#else
int main(int argc, char **argv) {
#endif
#if IR4_ENABLE_Z80
    const Backend *be = &be_z80;
    size_t base = 0x0100;
#elif IR4_ENABLE_EZ80
    const Backend *be = &be_ez80;
    size_t base = 0x040000UL;
#elif IR4_ENABLE_6502
    const Backend *be = &be_6502;
    size_t base = 0x0801;
#elif IR4_ENABLE_65816
    const Backend *be = &be_65816;
    size_t base = 0x010000UL;
#elif IR4_ENABLE_ARM64
    const Backend *be = &be_arm64;
    size_t base = 0x0000;
#elif IR4_ENABLE_RV32
    const Backend *be = &be_rv32;
    size_t base = 0x0000;
#elif IR4_ENABLE_BC
    const Backend *be = &be_bc;
    size_t base = 0x0100;
#else
#error "At least one IR4_ENABLE_* backend must be enabled"
#endif
    const Platform *plat = NULL;
    const char *in_path = NULL;
    const char *out_path = NULL;
    char default_out[32];
    const char *platform = NULL;
    int target_set = 0;
    int base_set = 0;
    int run_argc = 0;
    char **run_argv = NULL;
    int i;
    int j;
    FILE *in_fp;
    FILE *out_fp;

#ifdef IR4_INTERACTIVE_ARGS
    argv_buf[0] = "ir4c";
    argv_buf[1] = NULL;
#endif
    path_set_exe(argc > 0 ? argv[0] : NULL);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            i++;
            target_set = 1;
            if (0) {
#if IR4_ENABLE_Z80
            } else if (strcmp(argv[i], "z80") == 0) {
                be = &be_z80;
                if (!base_set)
                    base = 0x0100;
#endif
#if IR4_ENABLE_6502
            } else if (strcmp(argv[i], "6502") == 0) {
                be = &be_6502;
                if (!base_set)
                    base = 0x0801;
#endif
#if IR4_ENABLE_65816
            } else if (strcmp(argv[i], "65816") == 0) {
                be = &be_65816;
                if (!base_set)
                    base = 0x010000UL;
#endif
#if IR4_ENABLE_ARM64
            } else if (strcmp(argv[i], "arm64") == 0) {
                be = &be_arm64;
                if (!base_set)
                    base = 0x0000;
#endif
#if IR4_ENABLE_EZ80
            } else if (strcmp(argv[i], "ez80") == 0) {
                be = &be_ez80;
                if (!base_set)
                    base = 0x040000UL;
#endif
#if IR4_ENABLE_RV32
            } else if (strcmp(argv[i], "rv32") == 0) {
                be = &be_rv32;
                if (!base_set)
                    base = 0x0000;
#endif
#if IR4_ENABLE_BC
            } else if (strcmp(argv[i], "bc") == 0) {
                be = &be_bc;
                if (!base_set)
                    base = 0x0100;
#endif
            } else {
                const Platform *matched = NULL;
                for (j = 0; platforms[j]; j++) {
                    if (strcmp(platforms[j]->flag, "--run") != 0 &&
                        strcmp(argv[i], platforms[j]->flag + 2) == 0) {
                        matched = platforms[j];
                        break;
                    }
                }
                if (!matched) {
                    fprintf(stderr, "Unknown target: %s\n", argv[i]);
                    return 1;
                }
                if (plat != NULL) {
                    fprintf(stderr, "%s and -t %s cannot be combined\n", plat->flag, argv[i]);
                    return 1;
                }
                plat = matched;
                be = plat->be;
                if (!base_set)
                    base = plat->default_base;
                if (!platform && plat->req_platform)
                    platform = plat->req_platform;
            }
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            i++;
            base = (size_t)strtoul(argv[i], NULL, 0);
            base_set = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            i++;
            out_path = argv[i];
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            i++;
            ir4_add_lib_root(argv[i]);
        } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
            ir4_add_lib_root(argv[i] + 2);
        } else if (strcmp(argv[i], "-O0") == 0) {
            ir4_set_opt_level(0);
        } else if (strcmp(argv[i], "-O1") == 0 || strcmp(argv[i], "-O") == 0) {
            ir4_set_opt_level(1);
        } else if (strcmp(argv[i], "--platform") == 0 && i + 1 < argc) {
            i++;
            platform = argv[i];
#if IR4_ENABLE_6502
        } else if (strcmp(argv[i], "--zp") == 0 && i + 1 < argc) {
            i++;
            be_6502_set_zp((u8)strtoul(argv[i], NULL, 0));
        } else if (strcmp(argv[i], "--stack-zp") == 0 && i + 1 < argc) {
            const char *arg;
            char *colon;
            u8 zp_base;
            u8 zp_init = 0;
            i++;
            arg = argv[i];
            zp_base = (u8)strtoul(arg, &colon, 0);
            if (colon && *colon == ':') {
                zp_init = (u8)strtoul(colon + 1, NULL, 0);
            }
            be_6502_set_stack_zp(zp_base, zp_init);
        } else if (strcmp(argv[i], "--no-stack-zp") == 0) {
            be_6502_set_stack_zp(0, 0);
        } else if (strcmp(argv[i], "--compact") == 0) {
            be_6502_set_compact(1);
        } else if (strcmp(argv[i], "--no-compact") == 0) {
            be_6502_set_compact(0);
        } else if (strcmp(argv[i], "--auto-compact") == 0) {
            be_6502_set_compact(2);
#endif
        } else if (strcmp(argv[i], "--run") == 0) {
            const Platform *matched = NULL;
            for (j = 0; platforms[j]; j++) {
                if (strcmp(platforms[j]->flag, "--run") == 0) {
                    matched = platforms[j];
                    break;
                }
            }
            if (!matched) {
                print_usage();
                return 1;
            }
            if (plat != NULL) {
                fprintf(stderr, "%s and --run cannot be combined\n", plat->flag);
                return 1;
            }
            plat = matched;
            if (!platform && plat->req_platform) {
                platform = plat->req_platform;
            }
        } else if (strcmp(argv[i], "--") == 0) {
            run_argc = argc - (i + 1);
            run_argv = argv + (i + 1);
            break;
        } else if (argv[i][0] == '-') {
            print_usage();
            return 1;
        } else {
            in_path = argv[i];
        }
    }

#ifdef IR4_INTERACTIVE_ARGS
    if (!plat && !target_set && platforms[0] != NULL) {
        plat = platforms[0];
        be = plat->be;
        if (!base_set)
            base = plat->default_base;
        if (!platform && plat->req_platform)
            platform = plat->req_platform;
    }
#endif

    if (plat) {
#if IR4_ENABLE_JIT && IR4_ENABLE_BC
        if (plat == &plat_arm64_jit && target_set && be == &be_bc) {
            plat = &plat_bc_run;
            if (!platform && plat->req_platform)
                platform = plat->req_platform;
        }
#endif
        if (!target_set) {
            be = plat->be;
        } else if (be != plat->be) {
            fprintf(stderr, "%s requires -t %s\n", plat->flag, plat->be->name);
            return 1;
        }
        if (!base_set) {
            base = plat->default_base;
        }
    }

    if (!out_path) {
        const char *ext = (plat && plat->default_ext) ? plat->default_ext : "bin";
        sprintf(default_out, "out.%s", ext);
        out_path = default_out;
    }

    if (!in_path) {
#ifdef IR4_INTERACTIVE_ARGS
        static char in_buf[128];
        static char out_buf[128];
        size_t len;
        printf("Input (.ir4): ");
        fflush(stdout);
        if (!fgets(in_buf, sizeof(in_buf), stdin))
            return 1;
        len = strlen(in_buf);
        while (len > 0 && (in_buf[len - 1] == '\n' || in_buf[len - 1] == '\r'))
            in_buf[--len] = '\0';
        if (len == 0)
            return 1;
        in_path = in_buf;
        printf("Output [%s]: ", out_path);
        fflush(stdout);
        if (fgets(out_buf, sizeof(out_buf), stdin)) {
            len = strlen(out_buf);
            while (len > 0 && (out_buf[len - 1] == '\n' || out_buf[len - 1] == '\r'))
                out_buf[--len] = '\0';
            if (len > 0)
                out_path = out_buf;
        }
#else
        print_usage();
        return 1;
#endif
    }

#if IR4_ENABLE_JIT
    ir4_jit_set_args(in_path, run_argc, run_argv);
#else
    (void)run_argc;
    (void)run_argv;
#endif

    in_fp = fopen(in_path, "r");
    if (!in_fp) {
        fprintf(stderr, "ir4c: cannot open input %s\n", in_path);
        return 1;
    }

    cb_reset(base);
#if IR4_ENABLE_ELF
    be_rv32_set_obj(plat == &plat_rv32_obj);
#endif
    if (plat && plat->begin) {
        plat->begin(base);
    }
    ir4_compile_stream(in_fp, in_path, be, platform);
    /* lexer.c closes sf->fp on EOF to conserve file handles on 8-slot targets */

    if (plat && plat->end) {
        int main_idx = ir4_find_func("main");
        if (plat->needs_main && main_idx < 0) {
            ir4_status_clear();
            fprintf(stderr, "%s requires a 'main' function\n", plat->flag);
            return 1;
        }
        plat->end(main_idx);
    }

    /* Place data segment right after code on 16/24-bit targets */
#if IR4_ENABLE_ARM64
    if (be != &be_arm64)
#endif
#if IR4_ENABLE_RV32
    if (be != &be_rv32)
#endif
    {
        size_t data_addr;
        (void)cb_alloc_data(0, be->word_bytes);
        data_addr = cb_is_banked() ? (cb_get_bank(0).base + cb_get_bank(0).len) : cb_cur_addr();
        cb_patch_data_relocs(data_addr);
    }

    ir4_status_file("Writing", out_path, 0);
    out_fp = fopen(out_path, "wb");
    if (!out_fp) {
        ir4_status_clear();
        fprintf(stderr, "ir4c: cannot open output %s\n", out_path);
        return 1;
    }

    if (plat && plat->write) {
#if IR4_ENABLE_X16GS
        x16gs_set_out_path(out_path);
#endif
        if (!plat->write(out_fp, base)) {
            ir4_status_clear();
            fprintf(stderr, "ir4c: write failed: %s\n", out_path);
            fclose(out_fp);
            return 1;
        }
    } else {
        int pad_align = 1;
#if IR4_ENABLE_ARM64
        pad_align = (be != &be_arm64);
#endif
        if (!cb_write_raw(out_fp, pad_align)) {
            ir4_status_clear();
            fprintf(stderr, "ir4c: write failed: %s\n", out_path);
            fclose(out_fp);
            return 1;
        }
    }
    fclose(out_fp);
    ir4_status_clear();

    printf("Compiled %s -> %s (target: %s, base: 0x%04lX, size: %lu bytes)\n", in_path, out_path,
           be->name, (unsigned long)base, (unsigned long)cb_pc());

    if (plat && plat->run) {
        int main_idx = ir4_find_func("main");
        if (main_idx < 0) {
            fprintf(stderr, "%s requires a 'main' function\n", plat->flag);
            return 1;
        }
        plat->run((size_t)main_idx);
    }

    return 0;
}
