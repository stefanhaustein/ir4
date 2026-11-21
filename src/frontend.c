#include "frontend.h"
#include "codebuf.h"
#include "path.h"
#include "strpool.h"
#include "vec.h"

VEC_OF(ByteVec, u8);

typedef enum { BLOCK_IF = 1, BLOCK_BEGIN = 2 } BlockKind;

typedef struct {
    BlockKind kind;
    size_t patch_else;
    size_t patch_end;
    size_t loop_addr;
    TypeVec saved_stack;
    TypeVec else_stack;
    Bool has_else;
    Bool saved_unreach;
    Bool if_unreach;
} BlockInfo;

typedef enum {
    IR_FN_END,
    IR_PUSH_W,
    IR_PUSH_A,
    IR_PUSH_F,
    IR_PUSH_I32,
    IR_PUSH_DATA,
    IR_PUSH_HEAP_START,
    IR_LOCAL_OP,
    IR_STACK_OP,
    IR_ALU_OP,
    IR_CALL,
    IR_CALL_EXTERN,
    IR_PUSH_FN,
    IR_ICALL,
    IR_BR_FWD,
    IR_BR_BACK,
    IR_INLINE
} IrKind;

typedef struct {
    IrKind kind;
    union {
        i32 w;
        u32 a;
        double f;
        size_t offset;
        struct {
            LocalOp op;
            size_t slot;
            StackType type;
            size_t stack_bytes;
        } loc;
        struct {
            StackOp op;
            StackType t1;
            StackType t2;
        } stk;
        IrOp op;
        size_t func_idx;
        struct {
            BranchCond cond;
            size_t target;
        } br;
        size_t inl_idx;
    } u;
} IrInstr;

enum { CODE_REL_SELF = 1, CODE_REL_STACK = 2 };

typedef struct {
    size_t ir_off;
    u16 addend;
    u8 kind;
} CodeReloc;

typedef struct {
    size_t raw_start;
    size_t raw_len;
    size_t reloc_start;
    size_t reloc_cnt;
} InlineBlock;

VEC_OF(LocalVec, LocalVar);
VEC_OF(InlineVec, InlineBlock);

typedef struct {
    size_t ir_start;
    size_t ir_len;
    size_t reloc_start;
    size_t reloc_cnt;
    size_t param_cnt;
    LocalVec locals;
    Bool reachable;
    Bool is_code;
    Bool has_inline;
} WordIR;

VEC_OF(IrVec, IrInstr);
VEC_OF(WordVec, WordIR *);
VEC_OF(CodeRelocVec, CodeReloc);

static IrVec ir_buf;
static size_t fold_fence = 0;
static WordVec words;
static ByteVec raw_code_buf;
static CodeRelocVec code_relocs;
static InlineVec inline_blocks;

typedef struct {
    FILE *fp;
    const char *path;
    u32 line_num;
    u16 req_group;
    u8 req_rank;
    u8 buf[512];
    size_t buf_pos;
    size_t buf_len;
    int pushback;
} SrcFile;

VEC_OF(SrcVec, SrcFile);

static SrcVec src_stack;
static u16 next_req_group = 1;
static StrVec extra_lib_roots;

void ir4_add_lib_root(const char *path) {
    if (!path || path[0] == '\0')
        return;
    VEC_PUSH(extra_lib_roots, path);
}
static StrVec seen_files;
static const Backend *be;
static const char *platform_name = NULL;

VEC_OF(FuncVec, FuncSig *);
static FuncVec funcs;

typedef struct {
    const char *name;
    size_t offset;
    StackType type; /* TY_NONE for :bytes / data buffers; TY_W/TY_A/TY_F for typed globals */
} BufferSym;

static StrBuf tok_sb;

VEC_OF(BufVec, BufferSym *);
static BufVec buffers;

static LocalVec locals;
static TypeVec cur_outs;

VEC_OF(BlockVec, BlockInfo *);

static TypeVec tstack;
static BlockVec blocks;

void ir4_die(const char *msg) {
    if (src_stack.len > 0) {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "ir4c error (%s:%lu): %s\n", cur->path, (unsigned long)cur->line_num, msg);
    } else {
        fprintf(stderr, "ir4c error: %s\n", msg);
    }
    exit(1);
}

static const char *type_name(StackType t) {
    if (t == TY_W)
        return ":i";
    if (t == TY_A)
        return ":a";
    if (t == TY_F)
        return ":f";
    if (t == TY_I32)
        return ":i32";
    return "?";
}

static StackType parse_type_tok(const char *tok) {
    if (strcmp(tok, ":i") == 0)
        return TY_W;
    if (strcmp(tok, ":a") == 0)
        return TY_A;
    if (strcmp(tok, ":f") == 0)
        return TY_F;
    if (strcmp(tok, ":i32") == 0)
        return TY_I32;
    return TY_NONE;
}

static int src_getc(SrcFile *sf) {
    if (sf->pushback >= 0) {
        int c = sf->pushback;
        sf->pushback = -1;
        return c;
    }
    if (sf->buf_pos >= sf->buf_len) {
        sf->buf_len = fread(sf->buf, 1, sizeof(sf->buf), sf->fp);
        sf->buf_pos = 0;
        if (sf->buf_len == 0)
            return EOF;
    }
    return sf->buf[sf->buf_pos++];
}

static void src_ungetc(SrcFile *sf, int c) {
    sf->pushback = c;
}

static int skip_ws_and_comments(void) {
    for (;;) {
        SrcFile *cur;
        int c;
        if (src_stack.len == 0)
            return EOF;
        cur = &src_stack.data[src_stack.len - 1];
        c = src_getc(cur);
        if (c == EOF) {
            if (src_stack.len > 1) {
                fclose(cur->fp);
                src_stack.len--;
                continue;
            }
            return EOF;
        }
        if (c == '\n') {
            cur->line_num++;
            continue;
        }
        if (isspace((unsigned char)c))
            continue;
        if (c == '\\') {
            int c2 = src_getc(cur);
            if (c2 == EOF || c2 == '\n' || isspace((unsigned char)c2)) {
                if (c2 == '\n') {
                    cur->line_num++;
                } else if (c2 != EOF) {
                    while ((c = src_getc(cur)) != EOF && c != '\n')
                        ;
                    if (c == '\n')
                        cur->line_num++;
                }
                continue;
            }
            src_ungetc(cur, c2);
        }
        return c;
    }
}

static const char *read_quoted_intern(SrcFile *sf, const char *err_unterm) {
    int c = src_getc(sf);
    sb_reset(&tok_sb);
    while (c != EOF && c != '\n' && c != '"') {
        sb_putc(&tok_sb, (char)c);
        c = src_getc(sf);
    }
    if (c != '"')
        ir4_die(err_unterm);
    return sb_intern(&tok_sb);
}

static void parse_line_directive(SrcFile *cur, int last_c) {
    int c = last_c;
    u32 new_line = 0;
    int has_digits = 0;

    if (c == '\n' || c == EOF)
        ir4_die("expected line number after '#line'");
    while (c == ' ' || c == '\t')
        c = src_getc(cur);
    while (c >= '0' && c <= '9') {
        new_line = new_line * 10 + (u32)(c - '0');
        has_digits = 1;
        c = src_getc(cur);
    }
    if (!has_digits || new_line == 0)
        ir4_die("invalid line number in '#line'");
    while (c == ' ' || c == '\t')
        c = src_getc(cur);
    if (c == '"') {
        cur->path = read_quoted_intern(cur, "unterminated filename in '#line'");
        c = src_getc(cur);
    }
    while (c != EOF && c != '\n')
        c = src_getc(cur);
    cur->line_num = new_line;
}

static const char *next_tok(void) {
    for (;;) {
        int c = skip_ws_and_comments();
        if (c == EOF)
            return NULL;

        {
            SrcFile *cur = &src_stack.data[src_stack.len - 1];
            sb_reset(&tok_sb);
            while (c != EOF && !isspace((unsigned char)c)) {
                sb_putc(&tok_sb, (char)c);
                c = src_getc(cur);
            }
            if (strcmp(tok_sb.data, "#line") == 0) {
                parse_line_directive(cur, c);
                continue;
            }
            if (c == '\n')
                cur->line_num++;
        }
        return tok_sb.data;
    }
}

static const char *next_tok_intern(void) { return next_tok() ? sb_intern(&tok_sb) : NULL; }

static FILE *try_open_require(const char *path, int *already_seen) {
    size_t i;
    const char *p = intern_find(path);
    *already_seen = 0;
    if (p) {
        for (i = 0; i < seen_files.len; i++) {
            if (seen_files.data[i] == p) {
                *already_seen = 1;
                return NULL;
            }
        }
    }
    return fopen(path, "r");
}

static void push_src_file(FILE *fp, const char *path, u16 req_group, u8 req_rank) {
    const char *ipath = intern_str(path);
    SrcFile *sf = VEC_ADD(src_stack);
    VEC_PUSH(seen_files, ipath);
    sf->fp = fp;
    sf->path = ipath;
    sf->line_num = 1;
    sf->req_group = req_group;
    sf->req_rank = req_rank;
    sf->buf_pos = 0;
    sf->buf_len = 0;
    sf->pushback = -1;
}

static const char *platform_parent(const char *plat) {
    if (plat && strcmp(plat, "x16") == 0)
        return "c64";
    return NULL;
}

static void push_require_ex(const char *name, int lib_only) {
    const char *cands[4];
    int n_cands = 0;
    int c;
    FILE *found_fps[4];
    const char *found_paths[4];
    u8 found_ranks[4];
    int found_cnt = 0;
    int seen_cnt = 0;

    if (name[0] != '/') {
        if (platform_name != NULL && platform_name[0] != '\0') {
            const char *par = platform_parent(platform_name);
            cands[n_cands++] = path_join(platform_name, name);
            if (par)
                cands[n_cands++] = path_join(par, name);
        }
        cands[n_cands++] = path_join(be->name, name);
    }
    cands[n_cands++] = intern_str(name);

    if (!lib_only && src_stack.data[src_stack.len - 1].req_group == 0) {
        for (c = 0; c < n_cands; c++) {
            const char *path = path_resolve(src_stack.data[src_stack.len - 1].path, cands[c]);
            int seen = 0;
            FILE *fp = try_open_require(path, &seen);
            if (seen)
                seen_cnt++;
            else if (fp) {
                found_fps[found_cnt] = fp;
                found_paths[found_cnt] = path;
                found_ranks[found_cnt] = (u8)c;
                found_cnt++;
            }
        }
    }

    if (found_cnt == 0 && seen_cnt == 0 && name[0] != '/') {
        StrVec roots;
        const char *exe_dir = path_exe_dir();
        size_t r;
        roots.data = NULL;
        roots.len = 0;
        roots.cap = 0;
        for (r = 0; r < extra_lib_roots.len; r++) {
            VEC_PUSH(roots, extra_lib_roots.data[r]);
        }
        if (exe_dir != NULL) {
            VEC_PUSH(roots, path_join(exe_dir, "../lib"));
            VEC_PUSH(roots, path_join(exe_dir, "lib"));
        }
        VEC_PUSH(roots, "lib");
        for (r = 0; r < roots.len && found_cnt == 0 && seen_cnt == 0; r++) {
            for (c = 0; c < n_cands; c++) {
                const char *path = path_join(roots.data[r], cands[c]);
                int seen = 0;
                FILE *fp = try_open_require(path, &seen);
                if (seen)
                    seen_cnt++;
                else if (fp) {
                    found_fps[found_cnt] = fp;
                    found_paths[found_cnt] = path;
                    found_ranks[found_cnt] = (u8)c;
                    found_cnt++;
                }
            }
        }
        free(roots.data);
    }

    if (!lib_only && found_cnt == 0 && seen_cnt == 0 &&
        src_stack.data[src_stack.len - 1].req_group != 0) {
        for (c = 0; c < n_cands; c++) {
            const char *path = path_resolve(src_stack.data[src_stack.len - 1].path, cands[c]);
            int seen = 0;
            FILE *fp = try_open_require(path, &seen);
            if (seen)
                seen_cnt++;
            else if (fp) {
                found_fps[found_cnt] = fp;
                found_paths[found_cnt] = path;
                found_ranks[found_cnt] = (u8)c;
                found_cnt++;
            }
        }
    }

    if (found_cnt > 0) {
        u16 grp = next_req_group++;
        int i;
        /* Push least-specific first so most-specific is parsed first on LIFO stack. */
        for (i = found_cnt - 1; i >= 0; i--) {
            push_src_file(found_fps[i], found_paths[i], grp, found_ranks[i]);
        }
        return;
    }
    if (seen_cnt > 0)
        return;

    {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "ir4c error (%s:%u): cannot open required file '%s'\n", cur->path,
                cur->line_num, name);
        exit(1);
    }
}

static const char *read_require_path(void) {
    int c = skip_ws_and_comments();
    SrcFile *cur = &src_stack.data[src_stack.len - 1];

    if (c != '"')
        ir4_die("expected quoted filename after 'require'");
    return read_quoted_intern(cur, "unterminated filename in 'require'");
}

static void push_require(const char *name) { push_require_ex(name, 0); }

static int parse_int(const char *s, i32 *val) {
    char *endp;
    int base = 0;
    const char *p = s;

    if (*p == '$') {
        base = 16;
        p++;
        if (*p == '\0')
            return 0;
    } else {
        if (*p == '-' || *p == '+')
            p++;
        if (!isdigit((unsigned char)*p))
            return 0;
        p = s;
    }

    *val = (i32)strtol(p, &endp, base);
    return (*endp == '\0');
}

static int parse_float(const char *s, double *val) {
    char *endp;
    const char *p = s;

    if (*p == '-' || *p == '+')
        p++;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        return 0;
    if (!isdigit((unsigned char)*p) && *p != '.')
        return 0;
    if (*p == '.' && !isdigit((unsigned char)p[1]))
        return 0;
    if (strchr(p, '.') == NULL && strchr(p, 'e') == NULL && strchr(p, 'E') == NULL) {
        return 0;
    }

    *val = strtod(s, &endp);
    return (*endp == '\0');
}

static size_t tstack_bytes(void) {
    size_t bytes = 0;
    size_t i;
    for (i = 0; i < tstack.len; i++) {
        StackType t = tstack.data[i];
        if (t == TY_F)
            bytes += be->float_bytes;
        else if (t == TY_A)
            bytes += be->addr_bytes;
        else if (t == TY_I32)
            bytes += 4;
        else
            bytes += be->word_bytes;
    }
    return bytes;
}

static void tpush(StackType t) { VEC_PUSH(tstack, t); }

static StackType tpop(void) {
    if (tstack.len == 0)
        ir4_die("type stack underflow");
    return tstack.data[--tstack.len];
}

static void tpop_expect(StackType expected) {
    StackType got = tpop();
    if (got != expected) {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "ir4c error (%s:%lu): expected %s, got %s\n", cur->path,
                (unsigned long)cur->line_num, type_name(expected), type_name(got));
        exit(1);
    }
}

static void check_return_stack(void) {
    size_t i;
    if (tstack.len != cur_outs.len)
        ir4_die("return stack depth mismatch");
    for (i = 0; i < cur_outs.len; i++) {
        if (tstack.data[i] != cur_outs.data[i])
            ir4_die("return stack type mismatch");
    }
}

static void stack_copy(TypeVec *dst, const TypeVec *src) {
    VEC_ENSURE(*dst, src->len);
    if (src->len > 0)
        memcpy(dst->data, src->data, src->len * sizeof(StackType));
    dst->len = src->len;
}

static int stack_eq(const TypeVec *a, const TypeVec *b) {
    return a->len == b->len &&
           (a->len == 0 || memcmp(a->data, b->data, a->len * sizeof(StackType)) == 0);
}

static void free_block(BlockInfo *b) {
    free(b->saved_stack.data);
    free(b->else_stack.data);
    free(b);
}

static int find_local(const char *name) {
    size_t i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < locals.len; i++) {
        if (locals.data[i].name == p)
            return (int)i;
    }
    return -1;
}

int ir4_find_func(const char *name) {
    size_t i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < funcs.len; i++) {
        if (funcs.data[i]->name == p)
            return (int)i;
    }
    return -1;
}

static int find_buffer(const char *name) {
    size_t i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < buffers.len; i++) {
        if (buffers.data[i]->name == p)
            return (int)i;
    }
    return -1;
}

typedef struct {
    const char *name;
    StackType type;
    i32 w_val;
    u32 a_val;
    double f_val;
    u16 req_group;
    u8 req_rank;
    u8 is_default;
} ConstSym;

VEC_OF(ConstVec, ConstSym *);
static ConstVec consts;

static int find_const(const char *name) {
    size_t i;
    const char *p = intern_find(name);
    if (!p)
        return -1;
    for (i = 0; i < consts.len; i++) {
        if (consts.data[i]->name == p)
            return (int)i;
    }
    return -1;
}

size_t ir4_func_addr(size_t idx) {
    if (idx >= funcs.len)
        ir4_die("function index out of bounds");
    return funcs.data[idx]->addr;
}

const FuncSig *ir4_func_sig(size_t idx) {
    if (idx >= funcs.len)
        ir4_die("function index out of bounds");
    return funcs.data[idx];
}

typedef struct {
    const char *name;
    IrOp op;
    StackType in1;
    StackType in2;
    StackType out;
} BuiltinDef;

static const BuiltinDef builtins[] = {/* Native word (:i) arithmetic & bitwise */
                                      {"+", OP_ADD, TY_W, TY_W, TY_W},
                                      {"-", OP_SUB, TY_W, TY_W, TY_W},
                                      {"*", OP_MUL, TY_W, TY_W, TY_W},
                                      {"/", OP_DIV, TY_W, TY_W, TY_W},
                                      {"mod", OP_MOD, TY_W, TY_W, TY_W},
                                      {"u/", OP_UDIV, TY_W, TY_W, TY_W},
                                      {"umod", OP_UMOD, TY_W, TY_W, TY_W},
                                      {"and", OP_AND, TY_W, TY_W, TY_W},
                                      {"or", OP_OR, TY_W, TY_W, TY_W},
                                      {"xor", OP_XOR, TY_W, TY_W, TY_W},
                                      {"invert", OP_INVERT, TY_W, TY_NONE, TY_W},
                                      {"<<", OP_LSHIFT, TY_W, TY_W, TY_W},
                                      {">>", OP_ARSHIFT, TY_W, TY_W, TY_W},
                                      {"u>>", OP_RSHIFT, TY_W, TY_W, TY_W},

                                      /* Address (:a) arithmetic & comparisons (unsigned implied) */
                                      {"a+", OP_AADD, TY_W, TY_A, TY_A},
                                      {"a-", OP_ASUB, TY_W, TY_A, TY_A},
                                      {"adiff", OP_ADIFF, TY_A, TY_A, TY_W},
                                      {"a=", OP_AEQ, TY_A, TY_A, TY_W},
                                      {"a<>", OP_ANE, TY_A, TY_A, TY_W},
                                      {"a<", OP_ALT, TY_A, TY_A, TY_W},
                                      {"a>", OP_AGT, TY_A, TY_A, TY_W},

                                      /* Floating-point (:f) arithmetic & comparisons */
                                      {"f+", OP_FADD, TY_F, TY_F, TY_F},
                                      {"f-", OP_FSUB, TY_F, TY_F, TY_F},
                                      {"f*", OP_FMUL, TY_F, TY_F, TY_F},
                                      {"f/", OP_FDIV, TY_F, TY_F, TY_F},
                                      {"fnegate", OP_FNEG, TY_F, TY_NONE, TY_F},
                                      {"f=", OP_FEQ, TY_F, TY_F, TY_W},
                                      {"f<>", OP_FNE, TY_F, TY_F, TY_W},
                                      {"f<", OP_FLT, TY_F, TY_F, TY_W},
                                      {"f>", OP_FGT, TY_F, TY_F, TY_W},

                                      /* 32-bit integer (:i32) arithmetic, bitwise & comparisons */
                                      {"i32+", OP_ADD32, TY_I32, TY_I32, TY_I32},
                                      {"i32-", OP_SUB32, TY_I32, TY_I32, TY_I32},
                                      {"i32*", OP_MUL32, TY_I32, TY_I32, TY_I32},
                                      {"i32/", OP_DIV32, TY_I32, TY_I32, TY_I32},
                                      {"i32mod", OP_MOD32, TY_I32, TY_I32, TY_I32},
                                      {"u32/", OP_UDIV32, TY_I32, TY_I32, TY_I32},
                                      {"u32mod", OP_UMOD32, TY_I32, TY_I32, TY_I32},
                                      {"i32and", OP_AND32, TY_I32, TY_I32, TY_I32},
                                      {"i32or", OP_OR32, TY_I32, TY_I32, TY_I32},
                                      {"i32xor", OP_XOR32, TY_I32, TY_I32, TY_I32},
                                      {"i32invert", OP_INV32, TY_I32, TY_NONE, TY_I32},
                                      {"i32<<", OP_SHL32, TY_W, TY_I32, TY_I32},
                                      {"i32>>", OP_ASHR32, TY_W, TY_I32, TY_I32},
                                      {"u32>>", OP_RSHR32, TY_W, TY_I32, TY_I32},
                                      {"i32=", OP_EQ32, TY_I32, TY_I32, TY_W},
                                      {"i32<>", OP_NE32, TY_I32, TY_I32, TY_W},
                                      {"i32<", OP_LT32, TY_I32, TY_I32, TY_W},
                                      {"i32>", OP_GT32, TY_I32, TY_I32, TY_W},
                                      {"i32<=", OP_LE32, TY_I32, TY_I32, TY_W},
                                      {"i32>=", OP_GE32, TY_I32, TY_I32, TY_W},
                                      {"u32<", OP_ULT32, TY_I32, TY_I32, TY_W},
                                      {"u32>", OP_UGT32, TY_I32, TY_I32, TY_W},
                                      {"u32<=", OP_ULE32, TY_I32, TY_I32, TY_W},
                                      {"u32>=", OP_UGE32, TY_I32, TY_I32, TY_W},

                                      /* Native word (:i) comparisons */
                                      {"=", OP_EQ, TY_W, TY_W, TY_W},
                                      {"<>", OP_NE, TY_W, TY_W, TY_W},
                                      {"<", OP_LT, TY_W, TY_W, TY_W},
                                      {">", OP_GT, TY_W, TY_W, TY_W},
                                      {"<=", OP_LE, TY_W, TY_W, TY_W},
                                      {">=", OP_GE, TY_W, TY_W, TY_W},
                                      {"u<", OP_ULT, TY_W, TY_W, TY_W},
                                      {"u>", OP_UGT, TY_W, TY_W, TY_W},
                                      {"u<=", OP_ULE, TY_W, TY_W, TY_W},
                                      {"u>=", OP_UGE, TY_W, TY_W, TY_W},

                                      /* Memory load/store */
                                      {"i8@", OP_I8FETCH, TY_A, TY_NONE, TY_W},
                                      {"u8@", OP_U8FETCH, TY_A, TY_NONE, TY_W},
                                      {"i8!", OP_I8STORE, TY_A, TY_W, TY_NONE},
                                      {"i16@", OP_I16FETCH, TY_A, TY_NONE, TY_W},
                                      {"u16@", OP_U16FETCH, TY_A, TY_NONE, TY_W},
                                      {"i16!", OP_I16STORE, TY_A, TY_W, TY_NONE},
                                      {"@", OP_FETCH, TY_A, TY_NONE, TY_W},
                                      {"!", OP_STORE, TY_A, TY_W, TY_NONE},
                                      {"a@", OP_AFETCH, TY_A, TY_NONE, TY_A},
                                      {"a!", OP_ASTORE, TY_A, TY_A, TY_NONE},
                                      {"f@", OP_FFETCH, TY_A, TY_NONE, TY_F},
                                      {"f!", OP_FSTORE, TY_A, TY_F, TY_NONE},
                                      {"i32@", OP_I32FETCH, TY_A, TY_NONE, TY_I32},
                                      {"i32!", OP_I32STORE, TY_A, TY_I32, TY_NONE},

                                      /* Type conversions */
                                      {"sext", OP_SEXT, TY_W, TY_NONE, TY_W},
                                      {"i>a", OP_W2A, TY_W, TY_NONE, TY_A},
                                      {"a>i", OP_A2W, TY_A, TY_NONE, TY_W},
                                      {"i>f", OP_W2F, TY_W, TY_NONE, TY_F},
                                      {"f>i", OP_F2W, TY_F, TY_NONE, TY_W},
                                      {"i>i32", OP_W2I32, TY_W, TY_NONE, TY_I32},
                                      {"i>u32", OP_U2I32, TY_W, TY_NONE, TY_I32},
                                      {"i32>i", OP_I322W, TY_I32, TY_NONE, TY_W}};

static IrInstr *emit_ir(IrKind kind, WordIR *w) {
    IrInstr *ins = VEC_ADD(ir_buf);
    memset(ins, 0, sizeof(IrInstr));
    ins->kind = kind;
    w->ir_len++;
    return ins;
}

static i32 sext_word(u32 u, u8 word_bytes) {
    u32 mask = (word_bytes == 2) ? 0xFFFFUL : 0xFFFFFFUL;
    u32 sign = (word_bytes == 2) ? 0x8000UL : 0x800000UL;
    u &= mask;
    return (u & sign) ? (i32)(u | ~mask) : (i32)u;
}

static int try_fold_const_w(IrOp op, i32 a, i32 b, i32 *out) {
    if (be->word_bytes == 2 || be->word_bytes == 3) {
        u32 mask = (be->word_bytes == 2) ? 0xFFFFUL : 0xFFFFFFUL;
        u32 ua = (u32)a & mask;
        u32 ub = (u32)b & mask;
        i32 sa = sext_word(ua, be->word_bytes);
        i32 sb = sext_word(ub, be->word_bytes);
        switch (op) {
        case OP_ADD:
            *out = sext_word(ua + ub, be->word_bytes);
            return 1;
        case OP_SUB:
            *out = sext_word(ua - ub, be->word_bytes);
            return 1;
        case OP_MUL:
            *out = sext_word(ua * ub, be->word_bytes);
            return 1;
        case OP_UDIV:
            if (ub == 0)
                return 0;
            *out = sext_word(ua / ub, be->word_bytes);
            return 1;
        case OP_UMOD:
            if (ub == 0)
                return 0;
            *out = sext_word(ua % ub, be->word_bytes);
            return 1;
        case OP_DIV:
        case OP_MOD: {
            i32 smin = (be->word_bytes == 2) ? -32768L : -8388608L;
            if (sb == 0 || (sa == smin && sb == -1))
                return 0;
            *out = (op == OP_DIV) ? (sa / sb) : (sa % sb);
            return 1;
        }
        case OP_AND:
            *out = sext_word(ua & ub, be->word_bytes);
            return 1;
        case OP_OR:
            *out = sext_word(ua | ub, be->word_bytes);
            return 1;
        case OP_XOR:
            *out = sext_word(ua ^ ub, be->word_bytes);
            return 1;
        case OP_LSHIFT:
            if (ub >= (u32)(be->word_bytes * 8))
                return 0;
            *out = sext_word(ua << ub, be->word_bytes);
            return 1;
        case OP_RSHIFT:
            if (ub >= (u32)(be->word_bytes * 8))
                return 0;
            *out = sext_word(ua >> ub, be->word_bytes);
            return 1;
        case OP_ARSHIFT:
            if (ub >= (u32)(be->word_bytes * 8))
                return 0;
            if (sa >= 0 || ub == 0)
                *out = sext_word(ua >> ub, be->word_bytes);
            else
                *out = sext_word((ua >> ub) | (~0UL << (be->word_bytes * 8 - ub)), be->word_bytes);
            return 1;
        case OP_EQ:
            *out = (sa == sb) ? -1 : 0;
            return 1;
        case OP_NE:
            *out = (sa != sb) ? -1 : 0;
            return 1;
        case OP_LT:
            *out = (sa < sb) ? -1 : 0;
            return 1;
        case OP_GT:
            *out = (sa > sb) ? -1 : 0;
            return 1;
        case OP_LE:
            *out = (sa <= sb) ? -1 : 0;
            return 1;
        case OP_GE:
            *out = (sa >= sb) ? -1 : 0;
            return 1;
        case OP_ULT:
            *out = (ua < ub) ? -1 : 0;
            return 1;
        case OP_UGT:
            *out = (ua > ub) ? -1 : 0;
            return 1;
        case OP_ULE:
            *out = (ua <= ub) ? -1 : 0;
            return 1;
        case OP_UGE:
            *out = (ua >= ub) ? -1 : 0;
            return 1;
        default:
            return 0;
        }
    }
    if (be->word_bytes == 8) {
        if ((op == OP_ADD || op == OP_SUB) &&
            (a < -0x3FFFFFFF || a > 0x3FFFFFFF || b < -0x3FFFFFFF || b > 0x3FFFFFFF)) {
            return 0;
        }
        if ((op == OP_ULT || op == OP_UGT || op == OP_ULE || op == OP_UGE) && (a < 0 || b < 0)) {
            return 0;
        }
    }
    switch (op) {
    case OP_ADD:
        *out = (i32)((u32)a + (u32)b);
        return 1;
    case OP_SUB:
        *out = (i32)((u32)a - (u32)b);
        return 1;
    case OP_AND:
        *out = (i32)((u32)a & (u32)b);
        return 1;
    case OP_OR:
        *out = (i32)((u32)a | (u32)b);
        return 1;
    case OP_XOR:
        *out = (i32)((u32)a ^ (u32)b);
        return 1;
    case OP_MUL:
        if (be->word_bytes == 8)
            return 0;
        *out = (i32)((u32)a * (u32)b);
        return 1;
    case OP_LSHIFT:
        if (be->word_bytes == 8 || (u32)b >= 32)
            return 0;
        *out = (i32)((u32)a << (u32)b);
        return 1;
    case OP_RSHIFT:
        if ((be->word_bytes == 8 && a < 0) || (u32)b >= 32)
            return 0;
        *out = (i32)((u32)a >> (u32)b);
        return 1;
    case OP_ARSHIFT:
        if ((u32)b >= 32)
            return 0;
        if (a >= 0 || (u32)b == 0)
            *out = (i32)((u32)a >> (u32)b);
        else
            *out = (i32)(((u32)a >> (u32)b) | (~0UL << (32 - (u32)b)));
        return 1;
    case OP_EQ:
        *out = (a == b) ? -1 : 0;
        return 1;
    case OP_NE:
        *out = (a != b) ? -1 : 0;
        return 1;
    case OP_LT:
        *out = (a < b) ? -1 : 0;
        return 1;
    case OP_GT:
        *out = (a > b) ? -1 : 0;
        return 1;
    case OP_LE:
        *out = (a <= b) ? -1 : 0;
        return 1;
    case OP_GE:
        *out = (a >= b) ? -1 : 0;
        return 1;
    case OP_ULT:
        *out = ((u32)a < (u32)b) ? -1 : 0;
        return 1;
    case OP_UGT:
        *out = ((u32)a > (u32)b) ? -1 : 0;
        return 1;
    case OP_ULE:
        *out = ((u32)a <= (u32)b) ? -1 : 0;
        return 1;
    case OP_UGE:
        *out = ((u32)a >= (u32)b) ? -1 : 0;
        return 1;
    default:
        return 0;
    }
}

static IrOp swapped_store_op(IrOp op) {
    switch (op) {
    case OP_I8STORE:
        return OP_SI8STORE;
    case OP_I16STORE:
        return OP_SI16STORE;
    case OP_STORE:
        return OP_SSTORE;
    case OP_ASTORE:
        return OP_SASTORE;
    case OP_FSTORE:
        return OP_SFSTORE;
    default:
        return op;
    }
}

#define BUILTIN_HT_SIZE 512
static short builtin_ht[BUILTIN_HT_SIZE];
static short builtin_next[sizeof(builtins) / sizeof(builtins[0])];
static int builtin_ht_init = 0;

static u32 builtin_hash(const char *s) {
    u32 h = 2166136261UL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619UL;
    }
    return h;
}

static void init_builtin_ht(void) {
    size_t n = sizeof(builtins) / sizeof(builtins[0]);
    size_t i;
    for (i = 0; i < n; i++)
        builtin_next[i] = -1;
    for (i = n; i > 0; i--) {
        size_t idx = i - 1;
        u32 slot = builtin_hash(builtins[idx].name) & (BUILTIN_HT_SIZE - 1);
        while (builtin_ht[slot] != 0 &&
               strcmp(builtins[builtin_ht[slot] - 1].name, builtins[idx].name) != 0) {
            slot = (slot + 1) & (BUILTIN_HT_SIZE - 1);
        }
        builtin_next[idx] = (builtin_ht[slot] != 0) ? (short)(builtin_ht[slot] - 1) : -1;
        builtin_ht[slot] = (short)(idx + 1);
    }
    builtin_ht_init = 1;
}

static int handle_builtin(const char *tok, WordIR *w) {
    StackType a, b;
    size_t i;
    IrInstr *ins;

    /* Stack manipulation (polymorphic) */
    if (strcmp(tok, "dup") == 0) {
        a = tpop();
        tpush(a);
        tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DUP;
        ins->u.stk.t1 = (StackType)a;
        ins->u.stk.t2 = TY_NONE;
        return 1;
    }
    if (strcmp(tok, "drop") == 0) {
        a = tpop();
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_DROP;
        ins->u.stk.t1 = (StackType)a;
        ins->u.stk.t2 = TY_NONE;
        return 1;
    }
    if (strcmp(tok, "swap") == 0) {
        b = tpop();
        a = tpop();
        tpush(b);
        tpush(a);
        ins = emit_ir(IR_STACK_OP, w);
        ins->u.stk.op = OP_SWAP;
        ins->u.stk.t1 = (StackType)b;
        ins->u.stk.t2 = (StackType)a;
        return 1;
    }

    {
        int matched_name = 0;
        u32 slot;
        short cur_bi;
        if (!builtin_ht_init)
            init_builtin_ht();
        slot = builtin_hash(tok) & (BUILTIN_HT_SIZE - 1);
        while (builtin_ht[slot] != 0 &&
               strcmp(builtins[builtin_ht[slot] - 1].name, tok) != 0) {
            slot = (slot + 1) & (BUILTIN_HT_SIZE - 1);
        }
        cur_bi = (builtin_ht[slot] != 0) ? (short)(builtin_ht[slot] - 1) : -1;
        for (; cur_bi >= 0; cur_bi = builtin_next[cur_bi]) {
            int need;
            i = (size_t)cur_bi;
            if (be->word_bytes < 4 && strcmp(be->name, "bc") != 0 &&
                strcmp(be->name, "ez80") != 0 &&
                (builtins[i].op == OP_MUL32 || builtins[i].op == OP_DIV32 ||
                 builtins[i].op == OP_MOD32 || builtins[i].op == OP_UDIV32 ||
                 builtins[i].op == OP_UMOD32 || builtins[i].op == OP_SHL32 ||
                 builtins[i].op == OP_ASHR32 || builtins[i].op == OP_RSHR32)) {
                continue;
            }
            matched_name = 1;
            need = (builtins[i].in2 != TY_NONE) ? 2 : 1;
            if (tstack.len < (size_t)need)
                continue;
            if (tstack.data[tstack.len - 1] != builtins[i].in1)
                continue;
            if (builtins[i].in2 != TY_NONE && tstack.data[tstack.len - 2] != builtins[i].in2)
                continue;

            tstack.len -= need;
            if (builtins[i].out != TY_NONE)
                tpush(builtins[i].out);
            if (builtins[i].op == OP_INVERT && w->ir_len >= fold_fence + 1) {
                IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                if (p1->kind == IR_PUSH_W) {
                    p1->u.w = ~p1->u.w;
                    return 1;
                }
            }
            if ((builtins[i].op == OP_W2A || builtins[i].op == OP_W2I32 ||
                 builtins[i].op == OP_U2I32) &&
                w->ir_len >= fold_fence + 1) {
                IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                if (p1->kind == IR_PUSH_W) {
                    if (builtins[i].op == OP_W2A) {
                        p1->kind = IR_PUSH_A;
                        p1->u.a = (u32)p1->u.w;
                    } else {
                        p1->kind = IR_PUSH_I32;
                    }
                    return 1;
                }
            }
            if (w->ir_len >= fold_fence + 1) {
                IrOp sop = swapped_store_op(builtins[i].op);
                if (sop != builtins[i].op) {
                    IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                    if (p1->kind == IR_STACK_OP && p1->u.stk.op == OP_SWAP) {
                        p1->kind = IR_ALU_OP;
                        p1->u.op = sop;
                        return 1;
                    }
                }
            }
            if (w->ir_len >= fold_fence + 2) {
                IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 2];
                IrInstr *p2 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                i32 folded;
                if (p1->kind == IR_PUSH_W && p2->kind == IR_PUSH_W &&
                    try_fold_const_w(builtins[i].op, p1->u.w, p2->u.w, &folded)) {
                    p1->u.w = folded;
                    w->ir_len--;
                    ir_buf.len--;
                    return 1;
                }
                if (p1->kind == IR_PUSH_DATA && p2->kind == IR_PUSH_W &&
                    (builtins[i].op == OP_AADD || builtins[i].op == OP_ASUB)) {
                    i32 delta = (builtins[i].op == OP_ASUB) ? (i32)(0UL - (u32)p2->u.w) : p2->u.w;
                    if (delta >= 0) {
                        if ((u32)delta <= 0xFFFFUL) {
                            p1->u.offset += (size_t)delta;
                            w->ir_len--;
                            ir_buf.len--;
                            return 1;
                        }
                    } else {
                        size_t mag = (size_t)(0UL - (u32)delta);
                        if (mag <= p1->u.offset) {
                            p1->u.offset -= mag;
                            w->ir_len--;
                            ir_buf.len--;
                            return 1;
                        }
                    }
                }
            }
            if (w->ir_len >= fold_fence + 1) {
                IrOp rop = builtins[i].op;
                IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                if (p1->kind == IR_PUSH_W) {
                    if ((p1->u.w == 0 &&
                         (rop == OP_ADD || rop == OP_SUB || rop == OP_AADD || rop == OP_ASUB ||
                          rop == OP_OR || rop == OP_XOR || rop == OP_LSHIFT || rop == OP_RSHIFT ||
                          rop == OP_ARSHIFT)) ||
                        (p1->u.w == -1 && rop == OP_AND)) {
                        w->ir_len--;
                        ir_buf.len--;
                        return 1;
                    }
                    if (p1->u.w == 0 && (rop == OP_MUL || rop == OP_AND)) {
                        p1->kind = IR_STACK_OP;
                        p1->u.stk.op = OP_DROP;
                        p1->u.stk.t1 = TY_W;
                        p1->u.stk.t2 = TY_NONE;
                        ins = emit_ir(IR_PUSH_W, w);
                        ins->u.w = 0;
                        return 1;
                    }
                }
                if (rop == OP_MUL || rop == OP_UDIV || rop == OP_UMOD || rop == OP_MUL32 ||
                    rop == OP_UDIV32 || rop == OP_UMOD32) {
                    IrInstr *p1 = &ir_buf.data[w->ir_start + w->ir_len - 1];
                    u8 is32 = (rop == OP_MUL32 || rop == OP_UDIV32 || rop == OP_UMOD32);
                    if ((!is32 && p1->kind == IR_PUSH_W) || (is32 && p1->kind == IR_PUSH_I32)) {
                        u32 mask = (be->word_bytes == 2)   ? 0xFFFFUL
                                   : (be->word_bytes == 3) ? 0xFFFFFFUL
                                                           : 0xFFFFFFFFUL;
                        u32 v = (!is32 && be->word_bytes < 4) ? ((u32)p1->u.w & mask) : (u32)p1->u.w;
                        if (v == 1 && (rop == OP_MUL || rop == OP_UDIV || rop == OP_MUL32 ||
                                       rop == OP_UDIV32)) {
                            w->ir_len--;
                            ir_buf.len--;
                            return 1;
                        }
                        if (v >= 2 && (v & (v - 1)) == 0 &&
                            (is32 || be->word_bytes < 4 || p1->u.w > 0)) {
                            u8 sh = 0;
                            while ((v >> sh) > 1)
                                sh++;
                            if (rop == OP_UMOD || rop == OP_UMOD32) {
                                p1->u.w = (i32)(v - 1);
                                ins = emit_ir(IR_ALU_OP, w);
                                ins->u.op = is32 ? OP_AND32 : OP_AND;
                                return 1;
                            }
                            p1->kind = IR_PUSH_W;
                            p1->u.w = (i32)sh;
                            ins = emit_ir(IR_ALU_OP, w);
                            ins->u.op = (rop == OP_MUL)     ? OP_LSHIFT
                                        : (rop == OP_UDIV)  ? OP_RSHIFT
                                        : (rop == OP_MUL32) ? OP_SHL32
                                                            : OP_RSHR32;
                            return 1;
                        }
                    }
                }
            }
            ins = emit_ir(IR_ALU_OP, w);
            ins->u.op = builtins[i].op;
            return 1;
        }
        if (matched_name) {
            char msg[128];
            sprintf(msg, "type mismatch or stack underflow for '%s'", tok);
            ir4_die(msg);
        }
    }

    return 0;
}

typedef struct {
    size_t src_slot;
    size_t dst_slot;
    size_t size;
    const char *name;
} BytesParamCopy;

VEC_OF(BytesCopyVec, BytesParamCopy);
static BytesCopyVec cur_bytes_copies;
static size_t cur_bytes_cnt_slot = 0;

static void flush_bytes_params(u8 *params_flushed) {
    size_t k;
    if (*params_flushed)
        return;
    *params_flushed = 1;
    for (k = 0; k < cur_bytes_copies.len; k++) {
        size_t slot = locals.len;
        LocalVar *lv = VEC_ADD(locals);
        lv->name = cur_bytes_copies.data[k].name;
        lv->type = TY_A;
        lv->slot = slot;
        lv->size = cur_bytes_copies.data[k].size;
        cur_bytes_copies.data[k].dst_slot = slot;
    }
}

static void parse_icall_sig(FuncSig *sig) {
    const char *tok;
    u8 in_outs = 0;

    while ((tok = next_tok()) != NULL) {
        StackType ty;
        if (strcmp(tok, ")") == 0) {
            if (in_outs && sig->out_types.len == 0)
                ir4_die("expected return type after '->'");
            return;
        }
        if (strcmp(tok, "->") == 0) {
            if (in_outs)
                ir4_die("duplicate '->' in signature");
            in_outs = 1;
            continue;
        }
        ty = parse_type_tok(tok);
        if (ty == TY_NONE)
            ir4_die("expected type token (:i, :a, :f, :i32) in indirect call signature");
        if (in_outs) {
            VEC_PUSH(sig->out_types, ty);
        } else {
            VEC_PUSH(sig->in_types, ty);
        }
    }
    ir4_die("unterminated '(' in indirect call");
}

static void parse_sig_body(FuncSig *sig, size_t *param_cnt_out, int allow_bytes) {
    const char *tok;
    size_t param_cnt = 0;
    u8 in_outs = 0;
    u8 params_flushed = 0;

    locals.len = 0;
    cur_outs.len = 0;
    cur_bytes_copies.len = 0;
    cur_bytes_cnt_slot = 0;
    sig->out_reg = intern_str("");

    /* Parse inputs -> outputs ) (after '(' has been consumed) */
    while ((tok = next_tok()) != NULL) {
        StackType ty;
        const char *ty_tok;
        if (strcmp(tok, ")") == 0) {
            flush_bytes_params(&params_flushed);
            if (in_outs && sig->out_types.len == 0)
                ir4_die("expected return type after '->'");
            if (param_cnt_out)
                *param_cnt_out = param_cnt;
            return;
        }
        if (strcmp(tok, "->") == 0) {
            if (in_outs)
                ir4_die("duplicate '->' in signature");
            flush_bytes_params(&params_flushed);
            in_outs = 1;
            continue;
        }

        if (in_outs) {
            ty = parse_type_tok(tok);
            if (ty == TY_NONE) {
                if (!allow_bytes && cur_outs.len == 0 && sig->out_reg[0] == '\0') {
                    sig->out_reg = intern_str(tok);
                    tok = next_tok();
                    if (!tok)
                        ir4_die("expected type token (:i, :a, :f) after extern register");
                    ty = parse_type_tok(tok);
                }
                if (ty == TY_NONE)
                    ir4_die("expected type token (:i, :a, :f) in signature");
            }
            VEC_PUSH(sig->out_types, ty);
            VEC_PUSH(cur_outs, ty);
        } else {
            size_t slot = locals.len;
            LocalVar *lv;
            if (parse_type_tok(tok) != TY_NONE) {
                ir4_die("expected identifier before type in signature");
            }
            lv = VEC_ADD(locals);
            lv->name = intern_str(tok);
            ty_tok = next_tok();
            if (!ty_tok)
                ir4_die("expected type token (:i, :a, :f) after identifier in signature");
            if (strcmp(ty_tok, ":bytes") == 0) {
                i32 sz = 0;
                char bp_name[32];
                BytesParamCopy *bc;
                ty_tok = next_tok();
                if (!ty_tok || !parse_int(ty_tok, &sz) || sz <= 0 || sz > 0xFFFF) {
                    ir4_die("invalid byte count after ':bytes'");
                }
                if (!allow_bytes)
                    ir4_die("':bytes' not allowed in extern signature");
                bc = VEC_ADD(cur_bytes_copies);
                bc->src_slot = slot;
                bc->size = (size_t)sz;
                bc->name = lv->name;
                sprintf(bp_name, "__bp%u", (unsigned)param_cnt);
                lv->name = intern_str(bp_name);
                lv->type = TY_A;
                lv->slot = slot;
                lv->size = 0;
                VEC_PUSH(sig->in_types, TY_A);
                VEC_PUSH(sig->in_regs, lv->name);
                param_cnt++;
                continue;
            }
            ty = parse_type_tok(ty_tok);
            if (ty == TY_NONE)
                ir4_die("expected type token (:i, :a, :f) in signature");
            lv->type = ty;
            lv->slot = slot;
            lv->size = 0;
            VEC_PUSH(sig->in_types, ty);
            VEC_PUSH(sig->in_regs, lv->name);
            param_cnt++;
        }
    }

    ir4_die("unexpected EOF in signature (missing ')')");
}

static void compile_body(WordIR *w);

static void emit_bytes_param_copies(WordIR *w) {
    size_t k;
    for (k = 0; k < cur_bytes_copies.len; k++) {
        IrInstr *ins;
        size_t src = cur_bytes_copies.data[k].src_slot;
        size_t dst = cur_bytes_copies.data[k].dst_slot;
        size_t sz = cur_bytes_copies.data[k].size;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_ADDR;
        ins->u.loc.slot = dst;
        ins->u.loc.type = TY_A;
        ins->u.loc.stack_bytes = 0;

        ins = emit_ir(IR_LOCAL_OP, w);
        ins->u.loc.op = LOCAL_GET;
        ins->u.loc.slot = src;
        ins->u.loc.type = TY_A;
        ins->u.loc.stack_bytes = be->addr_bytes;

        ins = emit_ir(IR_PUSH_W, w);
        ins->u.w = (i32)sz;

        ins = emit_ir(IR_ALU_OP, w);
        ins->u.op = OP_BCOPY;
        fold_fence = w->ir_len;
    }
}

static Bool sigs_match(const FuncSig *a, const FuncSig *b) {
    return a->in_types.len == b->in_types.len && a->out_types.len == b->out_types.len &&
           (a->in_types.len == 0 ||
            memcmp(a->in_types.data, b->in_types.data, a->in_types.len * sizeof(StackType)) == 0) &&
           (a->out_types.len == 0 ||
            memcmp(a->out_types.data, b->out_types.data, a->out_types.len * sizeof(StackType)) == 0);
}

static void free_sig_vecs(FuncSig *s) {
    free(s->in_types.data);
    free(s->in_regs.data);
    free(s->out_types.data);
    s->in_types.data = NULL;
    s->in_regs.data = NULL;
    s->out_types.data = NULL;
}

static size_t alloc_func(FuncSig **sig_out, WordIR **w_out) {
    size_t idx = funcs.len;
    FuncSig *sig = NEW(FuncSig);
    WordIR *w = NEW(WordIR);
    VEC_PUSH(funcs, sig);
    VEC_PUSH(words, w);
    if (sig_out)
        *sig_out = sig;
    if (w_out)
        *w_out = w;
    return idx;
}

static void compile_word(int is_default, int is_hot) {
    const char *tok;
    const char *name;
    FuncSig *sig;
    WordIR *w;
    WordIR discard_w;
    int discard_body = 0;
    size_t saved_ir_count = ir_buf.len;
    size_t fidx;
    int existing;
    size_t param_cnt = 0;
    u16 cur_grp = src_stack.data[src_stack.len - 1].req_group;
    u8 cur_rank = src_stack.data[src_stack.len - 1].req_rank;

    name = next_tok_intern();
    if (!name)
        ir4_die("expected word name after 'fn'");
    tok = next_tok();
    if (!tok || strcmp(tok, "(") != 0)
        ir4_die("expected '(' after word name");

    existing = ir4_find_func(name);
    if (existing >= 0) {
        FuncSig fn_sig;
        FuncSig *ex = funcs.data[existing];
        if (ex->is_extern) {
            ir4_die("duplicate function definition");
        }
        if (ex->is_defined) {
            if (cur_grp == 0 || ex->req_group != cur_grp) {
                ir4_die("duplicate function definition");
            }
            if (cur_rank == ex->req_rank) {
                if (is_default && ex->is_default) {
                    ir4_die("duplicate 'default' function definition");
                }
                if (!is_default && !ex->is_default) {
                    ir4_die("duplicate function definition");
                }
            }
        }
        fidx = (size_t)existing;
        memset(&fn_sig, 0, sizeof(FuncSig));
        fn_sig.name = intern_str(name);
        tstack.len = 0;
        blocks.len = 0;
        parse_sig_body(&fn_sig, &param_cnt, 1);
        if (!sigs_match(&fn_sig, ex)) {
            ir4_die(ex->is_defined
                        ? "override signature does not match 'default' declaration"
                        : "function signature does not match 'fwd' declaration");
        }
        if ((is_default && ex->is_defined && cur_rank == ex->req_rank) ||
            (ex->is_defined && cur_rank > ex->req_rank)) {
            /* Already overridden by a more-specific file in this require group. */
            free_sig_vecs(&fn_sig);
            discard_body = 1;
            memset(&discard_w, 0, sizeof(WordIR));
            w = &discard_w;
        } else {
            free_sig_vecs(ex);
            *ex = fn_sig;
            ex->is_defined = 1;
            ex->is_default = (u8)is_default;
            ex->is_hot = (u8)is_hot;
            ex->req_group = cur_grp;
            ex->req_rank = cur_rank;
            sig = ex;
            w = words.data[fidx];
            memset(w, 0, sizeof(WordIR));
        }
    } else {
        fidx = alloc_func(&sig, &w);
        sig->name = intern_str(name);
        sig->is_defined = 1;
        sig->is_default = (u8)is_default;
        sig->is_hot = (u8)is_hot;
        sig->req_group = cur_grp;
        sig->req_rank = cur_rank;
        tstack.len = 0;
        blocks.len = 0;
        parse_sig_body(sig, &param_cnt, 1);
    }

    w->param_cnt = param_cnt;
    emit_bytes_param_copies(w);
    compile_body(w);
    if (discard_body) {
        ir_buf.len = saved_ir_count;
    }
}

VEC_OF(IdxVec, size_t);
static IdxVec init_funcs;

static void raw_emit_u8(size_t *out_len, u8 b) {
    VEC_PUSH(raw_code_buf, b);
    (*out_len)++;
}

static void parse_raw_code_body(size_t *out_start, size_t *out_len, size_t *out_reloc_start,
                                size_t *out_reloc_cnt) {
    const char *tok;
    i32 num_val;
    i32 pending = 0;
    int has_pending = 0;

    *out_start = raw_code_buf.len;
    *out_len = 0;
    *out_reloc_start = code_relocs.len;
    *out_reloc_cnt = 0;

    while ((tok = next_tok()) != NULL) {
        if (strcmp(tok, "end") == 0) {
            if (has_pending)
                ir4_die("missing width (:i8, :i16, :i32) in raw code");
            return;
        } else if (strcmp(tok, ":i8") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i8'");
            if (pending < -128 || pending > 255)
                ir4_die(":i8 value out of range");
            raw_emit_u8(out_len, (u8)(pending & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i16") == 0) {
            u16 v;
            if (!has_pending)
                ir4_die("expected integer before ':i16'");
            if (pending < -32768 || pending > 65535)
                ir4_die(":i16 value out of range");
            v = (u16)(pending & 0xFFFF);
            raw_emit_u8(out_len, (u8)(v & 0xFF));
            raw_emit_u8(out_len, (u8)((v >> 8) & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i32") == 0) {
            u32 v;
            if (!has_pending)
                ir4_die("expected integer before ':i32'");
            v = (u32)pending;
            raw_emit_u8(out_len, (u8)(v & 0xFF));
            raw_emit_u8(out_len, (u8)((v >> 8) & 0xFF));
            raw_emit_u8(out_len, (u8)((v >> 16) & 0xFF));
            raw_emit_u8(out_len, (u8)((v >> 24) & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":ar") == 0 || strcmp(tok, ":as") == 0) {
            CodeReloc *rel;
            if (!has_pending)
                ir4_die("expected offset before ':ar' or ':as'");
            if (pending < 0 || pending > 0xFFFF)
                ir4_die("relocation offset out of range");
            rel = VEC_ADD(code_relocs);
            rel->ir_off = *out_len;
            rel->addend = (u16)pending;
            rel->kind = (strcmp(tok, ":ar") == 0) ? CODE_REL_SELF : CODE_REL_STACK;
            if (rel->kind == CODE_REL_STACK && !be->stack_data_off) {
                ir4_die("':as' is only supported on targets with a RAM data stack");
            }
            (*out_reloc_cnt)++;
            raw_emit_u8(out_len, 0x00);
            raw_emit_u8(out_len, 0x00);
            has_pending = 0;
        } else if (parse_int(tok, &num_val)) {
            if (has_pending)
                ir4_die("missing width (:i8, :i16, :i32) before literal in raw code");
            pending = num_val;
            has_pending = 1;
        } else {
            ir4_die("unexpected token in raw code");
        }
    }
    ir4_die("unexpected EOF in raw code (missing 'end')");
}

static void compile_code(int is_hot) {
    const char *tok;
    const char *name;
    FuncSig *sig;
    WordIR *w;
    size_t fidx;

    int existing;
    u16 cur_grp = src_stack.data[src_stack.len - 1].req_group;
    u8 cur_rank = src_stack.data[src_stack.len - 1].req_rank;

    name = next_tok_intern();
    if (!name)
        ir4_die("expected word name after 'code'");
    existing = ir4_find_func(name);
    if (existing >= 0) {
        FuncSig code_sig;
        FuncSig *ex = funcs.data[existing];
        if (!ex->is_defined || ex->is_extern || !ex->is_default || cur_grp == 0 ||
            ex->req_group != cur_grp) {
            ir4_die("duplicate function definition");
        }
        tok = next_tok();
        if (!tok || strcmp(tok, "(") != 0)
            ir4_die("expected '(' after 'code' name");
        fidx = (size_t)existing;
        memset(&code_sig, 0, sizeof(FuncSig));
        code_sig.name = name;
        parse_sig_body(&code_sig, NULL, 0);
        if (!sigs_match(&code_sig, ex)) {
            ir4_die("override signature does not match 'default' declaration");
        }
        free_sig_vecs(ex);
        *ex = code_sig;
        ex->is_defined = 1;
        ex->is_default = 0;
        ex->is_hot = (Bool)is_hot;
        ex->req_group = cur_grp;
        ex->req_rank = cur_rank;
        sig = ex;
        w = words.data[fidx];
        memset(w, 0, sizeof(WordIR));
    } else {
        tok = next_tok();
        if (!tok || strcmp(tok, "(") != 0)
            ir4_die("expected '(' after 'code' name");
        fidx = alloc_func(&sig, &w);
        sig->name = name;
        sig->is_defined = 1;
        sig->is_hot = (Bool)is_hot;
        sig->req_group = cur_grp;
        sig->req_rank = cur_rank;
        parse_sig_body(sig, NULL, 0);
    }

    w->is_code = 1;
    parse_raw_code_body(&w->ir_start, &w->ir_len, &w->reloc_start, &w->reloc_cnt);
}

static void compile_init(void) {
    FuncSig *sig;
    WordIR *w;
    size_t fidx;
    char iname[32];

    fidx = alloc_func(&sig, &w);
    sprintf(iname, "__init_%u", (unsigned)init_funcs.len);
    sig->name = intern_str(iname);
    sig->is_defined = 1;
    VEC_PUSH(init_funcs, fidx);

    tstack.len = 0;
    blocks.len = 0;
    cur_outs.len = 0;
    locals.len = 0;
    compile_body(w);
}

typedef struct {
    const char *name;
    size_t target_ir;
    Bool defined;
} LabelInfo;

typedef struct {
    size_t ir_idx;
    size_t label_idx;
} LabelRef;

VEC_OF(LabelVec, LabelInfo);
VEC_OF(LabelRefVec, LabelRef);

static IrOp global_fetch_op(StackType ty) {
    if (ty == TY_A)
        return OP_AFETCH;
    if (ty == TY_F)
        return OP_FFETCH;
    if (ty == TY_I32)
        return OP_I32FETCH;
    return OP_FETCH;
}

static IrOp global_store_op(StackType ty) {
    if (ty == TY_A)
        return OP_ASTORE;
    if (ty == TY_F)
        return OP_FSTORE;
    if (ty == TY_I32)
        return OP_I32STORE;
    return OP_STORE;
}

static size_t global_type_bytes(StackType ty) {
    if (ty == TY_A)
        return be->addr_bytes;
    if (ty == TY_F)
        return be->float_bytes;
    if (ty == TY_I32)
        return 4;
    return be->word_bytes;
}

static size_t find_or_add_label(LabelVec *labels, const char *name) {
    size_t i;
    const char *p = intern_str(name);
    LabelInfo *li;
    for (i = 0; i < labels->len; i++) {
        if (labels->data[i].name == p)
            return i;
    }
    li = VEC_ADD(*labels);
    li->name = p;
    li->target_ir = 0;
    li->defined = 0;
    return labels->len - 1;
}

static void emit_label_br(WordIR *w, LabelVec *labels, LabelRefVec *refs, size_t lidx,
                          BranchCond cond) {
    IrInstr *ins;
    if (labels->data[lidx].defined) {
        ins = emit_ir(IR_BR_BACK, w);
        ins->u.br.cond = cond;
        ins->u.br.target = labels->data[lidx].target_ir;
    } else {
        LabelRef *ref = VEC_ADD(*refs);
        ref->ir_idx = w->ir_len;
        ref->label_idx = lidx;
        ins = emit_ir(IR_BR_FWD, w);
        ins->u.br.cond = cond;
    }
    fold_fence = w->ir_len;
}

/* Parse a local 'var <name> :<type>' declaration and return its local slot. */
static int parse_local_var(int allow_bytes) {
    const char *vname, *vtype;
    StackType ty;
    size_t slot;
    LocalVar *lv;
    vname = next_tok_intern();
    if (!vname)
        ir4_die("expected local variable name after 'var'");
    if (parse_type_tok(vname) != TY_NONE)
        ir4_die("expected identifier after 'var'");
    if (find_local(vname) >= 0)
        ir4_die("duplicate local variable name");
    vtype = next_tok();
    if (!vtype)
        ir4_die("expected type after local 'var' name");
    slot = locals.len;
    lv = VEC_ADD(locals);
    lv->name = vname;
    lv->slot = slot;
    if (strcmp(vtype, ":bytes") == 0) {
        i32 sz = 0;
        if (!allow_bytes)
            ir4_die("cannot assign to ':bytes' local");
        vtype = next_tok();
        if (!vtype || !parse_int(vtype, &sz) || sz <= 0 || sz > 0xFFFF)
            ir4_die("invalid byte count after ':bytes'");
        lv->type = TY_A;
        lv->size = (size_t)sz;
    } else {
        ty = parse_type_tok(vtype);
        if (ty == TY_NONE)
            ir4_die("expected type token (:i, :i32, :a, :f, :bytes) after 'var'");
        lv->type = ty;
        lv->size = 0;
    }
    return (int)slot;
}

static void compile_body(WordIR *w) {
    const char *tok;
    IrInstr *ins;
    Bool unreachable = false;
    i32 num_val;
    LabelVec labels;
    LabelRefVec label_refs;

    labels.data = NULL;
    labels.len = 0;
    labels.cap = 0;
    label_refs.data = NULL;
    label_refs.len = 0;
    label_refs.cap = 0;

    w->ir_start = ir_buf.len;
    w->ir_len = 0;
    fold_fence = 0;

    /* Parse, type-check, and record IR for word body (Pass 1) */
    while ((tok = next_tok()) != NULL) {
        int idx;

        if (strcmp(tok, "end") == 0) {
            size_t r;
            if (blocks.len != 0)
                ir4_die("unclosed control block at 'end'");
            for (r = 0; r < label_refs.len; r++) {
                size_t lidx = label_refs.data[r].label_idx;
                if (!labels.data[lidx].defined)
                    ir4_die("undefined label");
                ir_buf.data[w->ir_start + label_refs.data[r].ir_idx].u.br.target =
                    labels.data[lidx].target_ir;
            }
            free(labels.data);
            free(label_refs.data);
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
            }
            if (locals.len > 0) {
                VEC_ENSURE(w->locals, locals.len);
                memcpy(w->locals.data, locals.data, locals.len * sizeof(LocalVar));
                w->locals.len = locals.len;
            }
            return;
        }

        if (strcmp(tok, "var") == 0) {
            parse_local_var(1);
            continue;
        }

        if (strcmp(tok, "return") == 0) {
            if (!unreachable) {
                check_return_stack();
                emit_ir(IR_FN_END, w);
                unreachable = 1;
            }
            continue;
        }

        /* Bare integer literal defaults to :i, bare float literal defaults to :f */
        if (parse_int(tok, &num_val)) {
            tpush(TY_W);
            ins = emit_ir(IR_PUSH_W, w);
            ins->u.w = num_val;
            continue;
        }
        {
            double fval;
            if (parse_float(tok, &fval)) {
                tpush(TY_F);
                ins = emit_ir(IR_PUSH_F, w);
                ins->u.f = fval;
                continue;
            }
        }

        /* Address of local or typed global: & <ident> */
        if (strcmp(tok, "&") == 0) {
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '&'");
            idx = find_local(tok);
            if (idx >= 0) {
                LocalVar *lv = &locals.data[idx];
                if (lv->size > 0)
                    ir4_die("':bytes' local is already an address");
                ins = emit_ir(IR_LOCAL_OP, w);
                ins->u.loc.op = LOCAL_ADDR;
                ins->u.loc.slot = lv->slot;
                ins->u.loc.type = lv->type;
                ins->u.loc.stack_bytes = tstack_bytes();
                tpush(TY_A);
                continue;
            }
            idx = find_buffer(tok);
            if (idx >= 0) {
                BufferSym *bs = buffers.data[idx];
                if (bs->type == TY_NONE)
                    ir4_die("':bytes' or 'data' symbol is already an address");
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                tpush(TY_A);
                continue;
            }
            idx = ir4_find_func(tok);
            if (idx >= 0) {
                if (funcs.data[idx]->is_extern)
                    ir4_die("cannot take address of 'extern' function");
                ins = emit_ir(IR_PUSH_FN, w);
                ins->u.func_idx = (size_t)idx;
                tpush(TY_A);
                continue;
            }
            ir4_die("unknown variable or function in '&'");
        }

        if (strcmp(tok, "(") == 0) {
            FuncSig icall_sig;
            size_t k;
            memset(&icall_sig, 0, sizeof(FuncSig));
            parse_icall_sig(&icall_sig);
            tpop_expect(TY_A);
            for (k = icall_sig.in_types.len; k > 0; k--) {
                tpop_expect(icall_sig.in_types.data[k - 1]);
            }
            for (k = 0; k < icall_sig.out_types.len; k++) {
                tpush(icall_sig.out_types.data[k]);
            }
            free_sig_vecs(&icall_sig);
            emit_ir(IR_ICALL, w);
            continue;
        }

        /* Local or typed global add-assign: +-> <ident> */
        if (strcmp(tok, "+->") == 0) {
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '+->'");
            idx = find_local(tok);
            if (idx >= 0) {
                LocalVar *lv = &locals.data[idx];
                StackType dt = (lv->type == TY_A) ? TY_W : lv->type;
                if (lv->size > 0)
                    ir4_die("cannot '+->' to ':bytes' local");
                tpop_expect(dt);
                if (lv->type == TY_W || lv->type == TY_A) {
                    ins = emit_ir(IR_LOCAL_OP, w);
                    ins->u.loc.op = LOCAL_ADD;
                    ins->u.loc.slot = lv->slot;
                    ins->u.loc.type = lv->type;
                    ins->u.loc.stack_bytes = tstack_bytes();
                } else {
                    ins = emit_ir(IR_LOCAL_OP, w);
                    ins->u.loc.op = LOCAL_GET;
                    ins->u.loc.slot = lv->slot;
                    ins->u.loc.type = lv->type;
                    ins->u.loc.stack_bytes = tstack_bytes() + 4;
                    ins = emit_ir(IR_ALU_OP, w);
                    ins->u.op = (lv->type == TY_F) ? OP_FADD : OP_ADD32;
                    ins = emit_ir(IR_LOCAL_OP, w);
                    ins->u.loc.op = LOCAL_SET;
                    ins->u.loc.slot = lv->slot;
                    ins->u.loc.type = lv->type;
                    ins->u.loc.stack_bytes = tstack_bytes();
                }
                continue;
            }
            idx = find_buffer(tok);
            if (idx < 0)
                ir4_die("unknown variable in '+->'");
            {
                BufferSym *bs = buffers.data[idx];
                StackType dt;
                if (bs->type == TY_NONE)
                    ir4_die("cannot '+->' to ':bytes' or 'data' symbol");
                dt = (bs->type == TY_A) ? TY_W : bs->type;
                tpop_expect(dt);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                ins = emit_ir(IR_STACK_OP, w);
                ins->u.stk.op = OP_SWAP;
                ins->u.stk.t1 = dt;
                ins->u.stk.t2 = TY_A;
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = (bs->type == TY_F) ? OP_FETCH :
                            (bs->type == TY_I32) ? OP_FETCH :
                            (bs->type == TY_A) ? OP_AFETCH : OP_FETCH;
                if (bs->type == TY_A) {
                    ins = emit_ir(IR_STACK_OP, w);
                    ins->u.stk.op = OP_SWAP;
                    ins->u.stk.t1 = TY_W;
                    ins->u.stk.t2 = TY_A;
                }
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = (bs->type == TY_F) ? OP_FADD :
                            (bs->type == TY_I32) ? OP_ADD32 :
                            (bs->type == TY_A) ? OP_AADD : OP_ADD;
                ins = emit_ir(IR_STACK_OP, w);
                ins->u.stk.op = OP_SWAP;
                ins->u.stk.t1 = TY_A;
                ins->u.stk.t2 = bs->type;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = global_store_op(bs->type);
            }
            continue;
        }

        /* Local or typed global assignment: -> <ident> */
        if (strcmp(tok, "->") == 0) {
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '->'");
            idx = (strcmp(tok, "var") == 0) ? parse_local_var(0) : find_local(tok);
            if (idx >= 0) {
                LocalVar *lv = &locals.data[idx];
                if (lv->size > 0)
                    ir4_die("cannot assign to ':bytes' local");
                tpop_expect(lv->type);
                if ((lv->type == TY_W || lv->type == TY_A) && w->ir_len >= fold_fence + 3) {
                    IrInstr *p0 = &ir_buf.data[w->ir_start + w->ir_len - 3];
                    IrInstr *p1 = p0 + 1;
                    IrInstr *p2 = p0 + 2;
                    if (p0->kind == IR_LOCAL_OP && p0->u.loc.op == LOCAL_GET &&
                        p0->u.loc.slot == lv->slot && p1->kind == IR_PUSH_W &&
                        p2->kind == IR_ALU_OP &&
                        (p2->u.op == OP_ADD || p2->u.op == OP_SUB ||
                         p2->u.op == OP_AADD || p2->u.op == OP_ASUB)) {
                        i32 delta = (p2->u.op == OP_SUB || p2->u.op == OP_ASUB)
                                        ? (i32)(0 - (u32)p1->u.w)
                                        : p1->u.w;
                        p0->kind = IR_PUSH_W;
                        p0->u.w = delta;
                        p1->kind = IR_LOCAL_OP;
                        p1->u.loc.op = LOCAL_ADD;
                        p1->u.loc.slot = lv->slot;
                        p1->u.loc.type = lv->type;
                        p1->u.loc.stack_bytes = tstack_bytes();
                        w->ir_len--;
                        ir_buf.len--;
                        continue;
                    }
                }
                ins = emit_ir(IR_LOCAL_OP, w);
                ins->u.loc.op = LOCAL_SET;
                ins->u.loc.slot = lv->slot;
                ins->u.loc.type = lv->type;
                ins->u.loc.stack_bytes = tstack_bytes();
                continue;
            }
            idx = find_buffer(tok);
            if (idx < 0)
                ir4_die("unknown variable in '->'");
            {
                BufferSym *bs = buffers.data[idx];
                if (bs->type == TY_NONE)
                    ir4_die("cannot assign to ':bytes' or 'data' symbol");
                tpop_expect(bs->type);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = global_store_op(bs->type);
            }
            continue;
        }

        /* Control flow */
        if (strcmp(tok, "if") == 0) {
            BlockInfo *b;
            tpop_expect(TY_W);
            b = NEW(BlockInfo);
            VEC_PUSH(blocks, b);
            b->kind = BLOCK_IF;
            b->has_else = 0;
            b->saved_unreach = unreachable;
            b->if_unreach = 0;
            stack_copy(&b->saved_stack, &tstack);
            b->patch_else = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JZ;
            continue;
        }
        if (strcmp(tok, "else") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_IF)
                ir4_die("'else' without 'if'");
            b = blocks.data[blocks.len - 1];
            b->has_else = 1;
            b->if_unreach = unreachable;
            stack_copy(&b->else_stack, &tstack);
            stack_copy(&tstack, &b->saved_stack);
            unreachable = b->saved_unreach;
            b->patch_end = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JMP;
            ir_buf.data[w->ir_start + b->patch_else].u.br.target = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "then") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_IF)
                ir4_die("'then' without 'if'");
            b = blocks.data[--blocks.len];
            if (!b->has_else) {
                if (!unreachable && !stack_eq(&tstack, &b->saved_stack))
                    ir4_die("unbalanced stack in 'if ... then'");
                stack_copy(&tstack, &b->saved_stack);
                unreachable = b->saved_unreach;
                ir_buf.data[w->ir_start + b->patch_else].u.br.target = w->ir_len;
            } else {
                if (!b->if_unreach && !unreachable) {
                    if (!stack_eq(&tstack, &b->else_stack))
                        ir4_die("branch stack mismatch between 'if' and 'else'");
                } else if (!b->if_unreach && unreachable) {
                    stack_copy(&tstack, &b->else_stack);
                    unreachable = 0;
                }
                ir_buf.data[w->ir_start + b->patch_end].u.br.target = w->ir_len;
            }
            free_block(b);
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "begin") == 0) {
            BlockInfo *b = NEW(BlockInfo);
            VEC_PUSH(blocks, b);
            b->kind = BLOCK_BEGIN;
            b->loop_addr = w->ir_len;
            fold_fence = w->ir_len;
            stack_copy(&b->saved_stack, &tstack);
            continue;
        }
        if (strcmp(tok, "while") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_BEGIN)
                ir4_die("'while' without 'begin'");
            b = blocks.data[blocks.len - 1];
            tpop_expect(TY_W);
            b->patch_end = w->ir_len;
            ins = emit_ir(IR_BR_FWD, w);
            ins->u.br.cond = BR_JZ;
            continue;
        }
        if (strcmp(tok, "repeat") == 0) {
            BlockInfo *b;
            if (blocks.len == 0 || blocks.data[blocks.len - 1]->kind != BLOCK_BEGIN)
                ir4_die("'repeat' without 'begin'");
            b = blocks.data[--blocks.len];
            if (!stack_eq(&tstack, &b->saved_stack))
                ir4_die("loop body must leave stack even at 'repeat'");
            ins = emit_ir(IR_BR_BACK, w);
            ins->u.br.cond = BR_JMP;
            ins->u.br.target = b->loop_addr;
            ir_buf.data[w->ir_start + b->patch_end].u.br.target = w->ir_len;
            free_block(b);
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "::") == 0) {
            size_t lidx;
            if (blocks.len != 0)
                ir4_die("'::' inside control block");
            tok = next_tok();
            if (!tok)
                ir4_die("expected label name after '::'");
            if (!unreachable && tstack.len != 0)
                ir4_die("stack must be empty at '::'");
            tstack.len = 0;
            unreachable = 0;
            lidx = find_or_add_label(&labels, tok);
            if (labels.data[lidx].defined)
                ir4_die("duplicate label");
            labels.data[lidx].defined = 1;
            labels.data[lidx].target_ir = w->ir_len;
            fold_fence = w->ir_len;
            continue;
        }
        if (strcmp(tok, "jmp") == 0) {
            size_t lidx;
            tok = next_tok();
            if (!tok)
                ir4_die("expected label name after 'jmp'");
            if (!unreachable && tstack.len != 0)
                ir4_die("stack must be empty at 'jmp'");
            lidx = find_or_add_label(&labels, tok);
            if (!unreachable) {
                emit_label_br(w, &labels, &label_refs, lidx, BR_JMP);
                unreachable = 1;
            }
            continue;
        }
        if (strcmp(tok, "jz") == 0) {
            size_t lidx;
            tok = next_tok();
            if (!tok)
                ir4_die("expected label name after 'jz'");
            tpop_expect(TY_W);
            if (!unreachable && tstack.len != 0)
                ir4_die("stack must be empty after condition at 'jz'");
            lidx = find_or_add_label(&labels, tok);
            if (!unreachable) {
                emit_label_br(w, &labels, &label_refs, lidx, BR_JZ);
            }
            continue;
        }

        if (strcmp(tok, "inline") == 0) {
            InlineBlock ib;
            parse_raw_code_body(&ib.raw_start, &ib.raw_len, &ib.reloc_start, &ib.reloc_cnt);
            w->has_inline = 1;
            if (!unreachable) {
                IrInstr *ins = emit_ir(IR_INLINE, w);
                ins->u.inl_idx = inline_blocks.len;
                VEC_PUSH(inline_blocks, ib);
                fold_fence = w->ir_len;
            }
            continue;
        }

        /* Core built-in word */
        if (handle_builtin(tok, w))
            continue;

        /* Built-in heap start address (:a) */
        if (strcmp(tok, "__heap_start") == 0) {
            emit_ir(IR_PUSH_HEAP_START, w);
            tpush(TY_A);
            continue;
        }

        /* Local variable read (or ':bytes' local base address) */
        idx = find_local(tok);
        if (idx >= 0) {
            LocalVar *lv = &locals.data[idx];
            ins = emit_ir(IR_LOCAL_OP, w);
            ins->u.loc.op = (lv->size > 0) ? LOCAL_ADDR : LOCAL_GET;
            ins->u.loc.slot = lv->slot;
            ins->u.loc.type = lv->type;
            ins->u.loc.stack_bytes = tstack_bytes();
            tpush((lv->size > 0) ? TY_A : lv->type);
            continue;
        }

        /* Buffer address or typed global read */
        idx = find_buffer(tok);
        if (idx >= 0) {
            BufferSym *bs = buffers.data[idx];
            if (bs->type == TY_NONE) {
                tpush(TY_A);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
            } else {
                tpush(bs->type);
                ins = emit_ir(IR_PUSH_DATA, w);
                ins->u.offset = bs->offset;
                ins = emit_ir(IR_ALU_OP, w);
                ins->u.op = global_fetch_op(bs->type);
            }
            continue;
        }

        /* Compile-time constant */
        idx = find_const(tok);
        if (idx >= 0) {
            const ConstSym *cs = consts.data[idx];
            tpush(cs->type);
            if (cs->type == TY_W) {
                ins = emit_ir(IR_PUSH_W, w);
                ins->u.w = cs->w_val;
            } else if (cs->type == TY_A) {
                ins = emit_ir(IR_PUSH_A, w);
                ins->u.a = cs->a_val;
            } else if (cs->type == TY_I32) {
                ins = emit_ir(IR_PUSH_I32, w);
                ins->u.w = cs->w_val;
            } else {
                ins = emit_ir(IR_PUSH_F, w);
                ins->u.f = cs->f_val;
            }
            continue;
        }

        /* User word call */
        idx = ir4_find_func(tok);
        if (idx >= 0) {
            FuncSig *callee = funcs.data[idx];
            size_t i;
            for (i = callee->in_types.len; i > 0; i--) {
                tpop_expect(callee->in_types.data[i - 1]);
            }
            for (i = 0; i < callee->out_types.len; i++) {
                tpush(callee->out_types.data[i]);
            }
            if (callee->is_extern) {
                ins = emit_ir(IR_CALL_EXTERN, w);
            } else {
                ins = emit_ir(IR_CALL, w);
            }
            ins->u.func_idx = (size_t)idx;
            continue;
        }

        {
            const SrcFile *cur = &src_stack.data[src_stack.len - 1];
            fprintf(stderr, "ir4c error (%s:%lu): unknown word '%s'\n", cur->path,
                    (unsigned long)cur->line_num, tok);
        }
        exit(1);
    }

    ir4_die("unexpected EOF inside word definition (missing ';')");
}

static Bool can_reach_func(size_t cur, size_t target, Bool *visited) {
    WordIR *w = words.data[cur];
    size_t i;
    if (funcs.data[cur]->is_extern || w->is_code)
        return false;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            size_t callee = ins->u.func_idx;
            if (callee == target)
                return true;
            if (!visited[callee]) {
                visited[callee] = true;
                if (can_reach_func(callee, target, visited))
                    return true;
            }
        }
    }
    return false;
}

#define HOT_CODE_INLINE_MAX 16

static int can_inline_hot_code(size_t fidx, size_t *out_len) {
    FuncSig *sig = funcs.data[fidx];
    WordIR *cw = words.data[fidx];
    u8 ret_op;
    size_t len;
    if (!cw->is_code || !sig->is_hot || cw->ir_len <= 1)
        return 0;
    if (strcmp(be->name, "6502") == 0)
        ret_op = 0x60;
    else if (strcmp(be->name, "z80") == 0 || strcmp(be->name, "ez80") == 0)
        ret_op = 0xC9;
    else
        return 0;
    len = cw->ir_len - 1;
    if (len > HOT_CODE_INLINE_MAX || raw_code_buf.data[cw->ir_start + len] != ret_op)
        return 0;
    *out_len = len;
    return 1;
}

static void mark_reachable(size_t fidx) {
    WordIR *w = words.data[fidx];
    size_t i;
    if (funcs.data[fidx]->is_extern)
        return;
    if (!funcs.data[fidx]->is_defined)
        ir4_die("called 'fwd' function was never defined");
    if (w->reachable)
        return;
    w->reachable = 1;
    if (w->is_code)
        return;
    for (i = 0; i < w->ir_len; i++) {
        const IrInstr *ins = &ir_buf.data[w->ir_start + i];
        if (ins->kind == IR_CALL) {
            size_t inl_len = 0;
            if (!can_inline_hot_code(ins->u.func_idx, &inl_len))
                mark_reachable(ins->u.func_idx);
        } else if (ins->kind == IR_PUSH_FN) {
            mark_reachable(ins->u.func_idx);
        }
    }
}

typedef struct {
    size_t patch_offset;
    size_t target_ir;
} PendingBr;

typedef struct {
    size_t call_pc;
    size_t call_addr;
    size_t func_idx;
    Bool is_fn_ptr;
} FwdCall;

VEC_OF(FwdCallVec, FwdCall);
VEC_OF(SizeVec, size_t);
VEC_OF(PendingBrVec, PendingBr);

static FwdCallVec fwd_calls;
static size_t heap_start_off = 0;

static void emit_raw_code_block(size_t raw_start, size_t raw_len, size_t reloc_start,
                                  size_t reloc_cnt) {
    size_t i;
    size_t base_pc = cb_pc();
    size_t base_addr = be->cur_addr();
    for (i = 0; i < raw_len; i++) {
        emit8(raw_code_buf.data[raw_start + i]);
    }
    for (i = 0; i < reloc_cnt; i++) {
        const CodeReloc *rel = &code_relocs.data[reloc_start + i];
        size_t pc = base_pc + rel->ir_off;
        if (rel->kind == CODE_REL_SELF) {
            patch16(pc, (u16)(base_addr + rel->addend));
        } else if (rel->kind == CODE_REL_STACK) {
#if IR4_ENABLE_6502
            if (be == &be_6502 && be_6502_stack_zp_base() != 0) {
                patch16(pc, (u16)(be_6502_stack_zp_base() + rel->addend));
            } else
#endif
            {
                cb_add_data_reloc(pc, be->stack_data_off() + rel->addend);
            }
        }
    }
}

static int ir_is_br_target(const WordIR *w, size_t ir_idx) {
    size_t j;
    for (j = 0; j < w->ir_len; j++) {
        const IrInstr *bi = &ir_buf.data[w->ir_start + j];
        if ((bi->kind == IR_BR_FWD || bi->kind == IR_BR_BACK) && bi->u.br.target == ir_idx)
            return 1;
    }
    return 0;
}

static void emit_word(size_t fidx) {
    static SizeVec ir_addr;
    FuncSig *sig = funcs.data[fidx];
    WordIR *w = words.data[fidx];
    PendingBrVec pending;
    size_t i, p;

    pending.data = NULL;
    pending.len = 0;
    pending.cap = 0;

    if (w->is_code) {
        sig->addr = be->cur_addr();
        sig->has_addr = 1;
        emit_raw_code_block(w->ir_start, w->ir_len, w->reloc_start, w->reloc_cnt);
        return;
    }

    VEC_ENSURE(ir_addr, w->ir_len + 1);
    sig->addr = be->cur_addr();
    sig->has_addr = 1;
    be->fn_begin(sig->name, w->param_cnt, w->locals.data, w->locals.len, sig->is_recursive);

    for (i = 0; i <= w->ir_len; i++) {
        if (be->peep_fence && ir_is_br_target(w, i))
            be->peep_fence();
        for (p = 0; p < pending.len; p++) {
            if (pending.data[p].target_ir == i) {
                be->patch_fwd(pending.data[p].patch_offset);
            }
        }
        if (i == w->ir_len)
            break;
        ir_addr.data[i] = be->cur_addr();
        {
            const IrInstr *ins = &ir_buf.data[w->ir_start + i];
            switch (ins->kind) {
            case IR_FN_END:
                be->fn_end(sig, w->locals.data, w->locals.len);
                break;
            case IR_PUSH_W:
                if (i + 1 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_LOCAL_OP) {
                    const IrInstr *nins = &ir_buf.data[w->ir_start + i + 1];
                    if (nins->u.loc.op == LOCAL_ADD && be->local_add_imm &&
                        be->local_add_imm(nins->u.loc.slot, nins->u.loc.type,
                                          nins->u.loc.stack_bytes, ins->u.w)) {
                        i++;
                        break;
                    }
                }
                if (i + 2 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    ir_buf.data[w->ir_start + i + 2].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf.data[w->ir_start + i + 1].u.op;
                    IrOp mop = ir_buf.data[w->ir_start + i + 2].u.op;
                    if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH &&
                        mop <= OP_FSTORE && be->mem_imm) {
                        i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)ins->u.w) : ins->u.w;
                        if (be->mem_imm(mop, off)) {
                            i += 2;
                            break;
                        }
                    }
                }
                if (i + 1 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP) {
                    IrOp aop = ir_buf.data[w->ir_start + i + 1].u.op;
                    i32 imm = ins->u.w;
                    if (aop == OP_ADD || aop == OP_SUB || aop == OP_AADD || aop == OP_ASUB) {
                        i32 delta = (aop == OP_SUB || aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                        if (be->add_imm && be->add_imm(delta)) {
                            i++;
                            break;
                        }
                    } else if (aop >= OP_EQ && aop <= OP_AGT) {
                        if (i + 2 < w->ir_len && !ir_is_br_target(w, i + 1) &&
                            !ir_is_br_target(w, i + 2)) {
                            const IrInstr *bins = &ir_buf.data[w->ir_start + i + 2];
                            if (bins->kind == IR_BR_FWD && bins->u.br.cond == BR_JZ &&
                                be->cmp_br_fwd) {
                                size_t patch_off = 0;
                                if (be->cmp_br_fwd(aop, 1, imm, &patch_off)) {
                                    PendingBr *pb = VEC_ADD(pending);
                                    pb->patch_offset = patch_off;
                                    pb->target_ir = bins->u.br.target;
                                    i += 2;
                                    break;
                                }
                            } else if (bins->kind == IR_BR_BACK && bins->u.br.cond == BR_JZ &&
                                       be->cmp_br_back) {
                                if (be->cmp_br_back(aop, 1, imm,
                                                    ir_addr.data[bins->u.br.target])) {
                                    i += 2;
                                    break;
                                }
                            }
                        }
                        if (aop <= OP_UGE && be->cmp_imm && be->cmp_imm(aop, imm)) {
                            i++;
                            break;
                        }
                    } else if (aop == OP_LSHIFT || aop == OP_RSHIFT || aop == OP_ARSHIFT ||
                               aop == OP_SHL32 || aop == OP_RSHR32 || aop == OP_ASHR32) {
                        u8 max_sh = (aop >= OP_SHL32 || be->word_bytes == 4) ? 31
                                    : (be->word_bytes == 8)                  ? 63
                                                                             : (u8)(be->word_bytes * 8 - 1);
                        if (imm >= 0 && imm <= (i32)max_sh && be->shift_imm &&
                            be->shift_imm(aop, (u8)imm)) {
                            i++;
                            break;
                        }
                    } else if (aop == OP_AND || aop == OP_OR || aop == OP_XOR) {
                        if (be->bit_imm && be->bit_imm(aop, imm)) {
                            i++;
                            break;
                        }
                    }
                }
                be->push_w(ins->u.w);
                break;
            case IR_PUSH_A:
                be->push_a(ins->u.a);
                break;
            case IR_PUSH_F:
                be->push_f(ins->u.f);
                break;
            case IR_PUSH_I32:
                be->push_i32(ins->u.w);
                break;
            case IR_PUSH_DATA:
                if (i + 1 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP &&
                    be->data_mem &&
                    be->data_mem(ir_buf.data[w->ir_start + i + 1].u.op, ins->u.offset)) {
                    i++;
                    break;
                }
                be->push_data(ins->u.offset);
                break;
            case IR_PUSH_HEAP_START:
                be->push_data(heap_start_off);
                break;
            case IR_LOCAL_OP:
                if (ins->u.loc.op == LOCAL_ADDR && be->local_mem) {
                    if (i + 3 < w->ir_len && ir_buf.data[w->ir_start + i + 1].kind == IR_PUSH_W &&
                        ir_buf.data[w->ir_start + i + 2].kind == IR_ALU_OP &&
                        ir_buf.data[w->ir_start + i + 3].kind == IR_ALU_OP) {
                        IrOp aop = ir_buf.data[w->ir_start + i + 2].u.op;
                        IrOp mop = ir_buf.data[w->ir_start + i + 3].u.op;
                        if ((aop == OP_AADD || aop == OP_ASUB) && mop >= OP_I8FETCH &&
                            mop <= OP_FSTORE) {
                            i32 imm = ir_buf.data[w->ir_start + i + 1].u.w;
                            i32 off = (aop == OP_ASUB) ? (i32)(0UL - (u32)imm) : imm;
                            if (be->local_mem(ins->u.loc.slot, off, mop, ins->u.loc.stack_bytes)) {
                                i += 3;
                                break;
                            }
                        }
                    } else if (i + 1 < w->ir_len &&
                               ir_buf.data[w->ir_start + i + 1].kind == IR_ALU_OP) {
                        IrOp mop = ir_buf.data[w->ir_start + i + 1].u.op;
                        if (mop >= OP_I8FETCH && mop <= OP_FSTORE &&
                            be->local_mem(ins->u.loc.slot, 0, mop, ins->u.loc.stack_bytes)) {
                            i += 1;
                            break;
                        }
                    }
                }
                be->local_op(ins->u.loc.op, ins->u.loc.slot, ins->u.loc.type,
                             ins->u.loc.stack_bytes);
                break;
            case IR_STACK_OP:
                be->stack_op(ins->u.stk.op, ins->u.stk.t1, ins->u.stk.t2);
                break;
            case IR_ALU_OP:
                if (ins->u.op >= OP_EQ && ins->u.op <= OP_AGT && i + 1 < w->ir_len &&
                    !ir_is_br_target(w, i + 1)) {
                    const IrInstr *bins = &ir_buf.data[w->ir_start + i + 1];
                    if (bins->kind == IR_BR_FWD && bins->u.br.cond == BR_JZ && be->cmp_br_fwd) {
                        size_t patch_off = 0;
                        if (be->cmp_br_fwd(ins->u.op, 0, 0, &patch_off)) {
                            PendingBr *pb = VEC_ADD(pending);
                            pb->patch_offset = patch_off;
                            pb->target_ir = bins->u.br.target;
                            i++;
                            break;
                        }
                    } else if (bins->kind == IR_BR_BACK && bins->u.br.cond == BR_JZ &&
                               be->cmp_br_back) {
                        if (be->cmp_br_back(ins->u.op, 0, 0, ir_addr.data[bins->u.br.target])) {
                            i++;
                            break;
                        }
                    }
                }
                be->op(ins->u.op);
                break;
            case IR_CALL: {
                size_t target = ins->u.func_idx;
                size_t inl_len = 0;
                if (can_inline_hot_code(target, &inl_len)) {
                    emit_raw_code_block(words.data[target]->ir_start, inl_len,
                                        words.data[target]->reloc_start,
                                        words.data[target]->reloc_cnt);
                    break;
                }
                if (funcs.data[target]->has_addr && be->tail_call && i + 1 < w->ir_len &&
                    ir_buf.data[w->ir_start + i + 1].kind == IR_FN_END &&
                    !ir_is_br_target(w, i + 1)) {
#if IR4_ENABLE_6502
                    if (be == &be_6502) {
                        be_6502_set_call_bank(funcs.data[target]->bank);
                    }
#endif
                    if (be->tail_call(funcs.data[target]->addr, sig, w->locals.data,
                                      w->locals.len)) {
                        i++;
                        break;
                    }
                }
                {
                size_t call_addr = be->cur_addr();
                size_t call_pc = cb_pc();
#if IR4_ENABLE_6502
                if (be == &be_6502) {
                    be_6502_set_call_bank(funcs.data[target]->bank);
                }
#endif
                be->call(funcs.data[target]->addr);
                if (!funcs.data[target]->has_addr) {
                    FwdCall *fc = VEC_ADD(fwd_calls);
                    fc->call_pc = call_pc;
                    fc->call_addr = call_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 0;
                }
                }
                break;
            }
            case IR_PUSH_FN: {
                size_t target = ins->u.func_idx;
                size_t fn_addr = be->cur_addr();
                size_t fn_pc = be->push_fn(funcs.data[target]->addr);
                if (!funcs.data[target]->has_addr) {
                    FwdCall *fc = VEC_ADD(fwd_calls);
                    fc->call_pc = fn_pc;
                    fc->call_addr = fn_addr;
                    fc->func_idx = target;
                    fc->is_fn_ptr = 1;
                }
                break;
            }
            case IR_ICALL:
                be->icall();
                break;
            case IR_CALL_EXTERN:
                be->call_extern(funcs.data[ins->u.func_idx]);
                break;
            case IR_BR_FWD: {
                PendingBr *pb = VEC_ADD(pending);
                pb->patch_offset = be->emit_br_fwd(ins->u.br.cond);
                pb->target_ir = ins->u.br.target;
                break;
            }
            case IR_BR_BACK:
                be->emit_br_back(ins->u.br.cond, ir_addr.data[ins->u.br.target]);
                break;
            case IR_INLINE: {
                const InlineBlock *ib = &inline_blocks.data[ins->u.inl_idx];
                if (strcmp(be->name, "bc") == 0)
                    ir4_die("'inline' is not supported on bytecode target");
                if (be->peep_fence)
                    be->peep_fence();
                emit_raw_code_block(ib->raw_start, ib->raw_len, ib->reloc_start, ib->reloc_cnt);
                break;
            }
            }
        }
    }
    free(pending.data);
}

static void flush_vals_word(i32 val) {
    if (be->word_bytes == 2 && (val < -32768 || val > 65535)) {
        ir4_die(":i value out of range");
    }
    if (be->word_bytes == 3 && (val < -8388608L || val > 16777215L)) {
        ir4_die(":i value out of range");
    }
    cb_emit_data_word(val, be->word_bytes);
}

static void emit_vals_string(void) {
    SrcFile *cur = &src_stack.data[src_stack.len - 1];
    for (;;) {
        int c = src_getc(cur);
        if (c == EOF || c == '\n')
            ir4_die("unterminated string literal in 'data'");
        if (c == '"')
            return;
        if (c == '\\') {
            c = src_getc(cur);
            if (c == EOF || c == '\n')
                ir4_die("unterminated escape in string literal");
            switch (c) {
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case '0':
                c = '\0';
                break;
            case '\\':
                c = '\\';
                break;
            case '"':
                c = '"';
                break;
            default:
                ir4_die("unknown escape sequence in string literal");
            }
        }
        cb_emit_data_u8((u8)c);
    }
}

static void compile_data(void) {
    const char *tok;
    const char *name;
    i32 num_val;
    i32 pending = 0;
    int has_pending = 0;
    int ch;

    BufferSym *bs;
    name = next_tok_intern();
    if (!name) {
        ir4_die("expected name after 'data'");
    }
    if (find_buffer(name) >= 0) {
        ir4_die("duplicate global/data name");
    }
    bs = NEW(BufferSym);
    bs->name = name;
    bs->offset = cb_alloc_data(0, be->word_bytes);
    bs->type = TY_NONE;
    VEC_PUSH(buffers, bs);

    while ((ch = skip_ws_and_comments()) != EOF) {
        if (ch == '"') {
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            emit_vals_string();
            continue;
        }
        src_ungetc(&src_stack.data[src_stack.len - 1], ch);
        tok = next_tok();
        if (!tok)
            break;
        if (strcmp(tok, "end") == 0) {
            if (has_pending)
                flush_vals_word(pending);
            return;
        } else if (strcmp(tok, "align") == 0) {
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            cb_alloc_data(0, be->word_bytes);
        } else if (strcmp(tok, "&") == 0) {
            int idx;
            size_t patch_off;
            if (has_pending) {
                flush_vals_word(pending);
                has_pending = 0;
            }
            tok = next_tok();
            if (!tok)
                ir4_die("expected identifier after '&' in 'data'");
            patch_off = cb_data_size();
            idx = find_buffer(tok);
            if (idx >= 0) {
                cb_emit_data_word(0, be->addr_bytes);
                cb_add_data_init_reloc(patch_off, buffers.data[idx]->offset, be->addr_bytes, false);
            } else {
                idx = ir4_find_func(tok);
                if (idx < 0)
                    ir4_die("unknown variable or function in '&' inside 'data'");
                if (funcs.data[idx]->is_extern)
                    ir4_die("cannot take address of 'extern' function in 'data'");
                cb_emit_data_word(0, be->addr_bytes);
                cb_add_data_init_reloc(patch_off, (size_t)idx, be->addr_bytes, true);
            }
        } else if (strcmp(tok, ":i8") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i8'");
            if (pending < -128 || pending > 255)
                ir4_die(":i8 value out of range");
            cb_emit_data_u8((u8)(pending & 0xFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i16") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i16'");
            if (pending < -32768 || pending > 65535)
                ir4_die(":i16 value out of range");
            cb_emit_data_u16((u16)(pending & 0xFFFF));
            has_pending = 0;
        } else if (strcmp(tok, ":i32") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i32'");
            cb_emit_data_u16((u16)((u32)pending & 0xFFFFU));
            cb_emit_data_u16((u16)(((u32)pending >> 16) & 0xFFFFU));
            has_pending = 0;
        } else if (strcmp(tok, ":i") == 0) {
            if (!has_pending)
                ir4_die("expected integer before ':i'");
            flush_vals_word(pending);
            has_pending = 0;
        } else if (parse_int(tok, &num_val)) {
            if (has_pending)
                flush_vals_word(pending);
            pending = num_val;
            has_pending = 1;
        } else {
            ir4_die("unexpected token in 'data'");
        }
    }
    ir4_die("unexpected EOF in 'data' (missing 'end')");
}

static void compile_const(int is_default) {
    const char *name;
    const char *val_tok;
    StackType ty;
    i32 iv = 0;
    double fv = 0.0;
    int is_int;
    int is_float;
    u16 cur_grp = src_stack.data[src_stack.len - 1].req_group;
    u8 cur_rank = src_stack.data[src_stack.len - 1].req_rank;
    int ex_idx;
    ConstSym cs;

    name = next_tok_intern();
    if (!name)
        ir4_die("expected constant name after 'const'");
    if (find_buffer(name) >= 0 || ir4_find_func(name) >= 0)
        ir4_die("constant name conflicts with existing global or function");

    val_tok = next_tok();
    if (!val_tok)
        ir4_die("expected literal value after constant name");
    is_int = parse_int(val_tok, &iv);
    is_float = !is_int && parse_float(val_tok, &fv);

    memset(&cs, 0, sizeof(ConstSym));
    cs.name = name;
    cs.req_group = cur_grp;
    cs.req_rank = cur_rank;
    cs.is_default = (u8)is_default;

    if (is_int) {
        ty = TY_W;
        if (be->word_bytes == 2 && (iv < -32768 || iv > 65535))
            ir4_die(":i constant out of 16-bit range");
        if (be->word_bytes == 3 && (iv < -8388608L || iv > 16777215L))
            ir4_die(":i constant out of 24-bit range");
        cs.w_val = iv;
    } else if (is_float) {
        ty = TY_F;
        cs.f_val = fv;
    } else {
        ir4_die("expected integer or float literal after constant name");
        return;
    }
    cs.type = ty;

    ex_idx = find_const(name);
    if (ex_idx >= 0) {
        ConstSym *ex = consts.data[ex_idx];
        if (cur_grp == 0 || ex->req_group != cur_grp)
            ir4_die("duplicate constant definition");
        if (cur_rank == ex->req_rank) {
            if (is_default && ex->is_default)
                ir4_die("duplicate 'default' constant definition");
            if (!is_default && !ex->is_default)
                ir4_die("duplicate constant definition");
        }
        if (ex->type != ty)
            ir4_die("override type does not match 'default' constant");
        if ((is_default && cur_rank == ex->req_rank) || (cur_rank > ex->req_rank))
            return;
        *ex = cs;
        return;
    }

    {
        ConstSym *ncs = NEW(ConstSym);
        *ncs = cs;
        VEC_PUSH(consts, ncs);
    }
}

void ir4_compile_stream(FILE *in, const char *path, const Backend *target_be,
                        const char *platform) {
    const char *tok;
    i32 num_val;
    const char *root_path = intern_str((path != NULL) ? path : "<stdin>");
    SrcFile *sf;

    src_stack.len = 0;
    sf = VEC_ADD(src_stack);
    sf->fp = in;
    sf->path = root_path;
    sf->line_num = 1;
    sf->req_group = 0;
    sf->req_rank = 0;
    sf->buf_pos = 0;
    sf->buf_len = 0;
    sf->pushback = -1;
    next_req_group = 1;

    seen_files.len = 0;
    VEC_PUSH(seen_files, root_path);

    be = target_be;
    platform_name = platform;
    funcs.len = 0;
    words.len = 0;
    buffers.len = 0;
    consts.len = 0;
    ir_buf.len = 0;
    raw_code_buf.len = 0;
    inline_blocks.len = 0;
    code_relocs.len = 0;
    fwd_calls.len = 0;
    init_funcs.len = 0;

    if (be->word_bytes < 4 && strcmp(be->name, "bc") != 0 && strcmp(be->name, "ez80") != 0) {
        push_require_ex("i32.ir4", 1);
    }

    while ((tok = next_tok()) != NULL) {
        if (strcmp(tok, "fn") == 0) {
            compile_word(0, 0);
        } else if (strcmp(tok, "const") == 0) {
            compile_const(0);
        } else if (strcmp(tok, "default") == 0 || strcmp(tok, "hot") == 0) {
            int is_default = 0;
            int is_hot = 0;
            for (;;) {
                if (strcmp(tok, "default") == 0 && !is_default) {
                    is_default = 1;
                } else if (strcmp(tok, "hot") == 0 && !is_hot) {
                    is_hot = 1;
                } else {
                    break;
                }
                tok = next_tok();
                if (!tok)
                    break;
            }
            if (tok && strcmp(tok, "code") == 0) {
                if (is_default)
                    ir4_die("'default' is not supported on 'code'");
                compile_code(is_hot);
            } else if (tok && strcmp(tok, "const") == 0) {
                if (is_hot)
                    ir4_die("'hot' is not supported on 'const'");
                compile_const(is_default);
            } else {
                if (!tok || strcmp(tok, "fn") != 0) {
                    ir4_die("expected 'fn', 'const', or 'code' after function modifier");
                }
                compile_word(is_default, is_hot);
            }
        } else if (strcmp(tok, "code") == 0) {
            compile_code(0);
        } else if (strcmp(tok, "init") == 0) {
            compile_init();
        } else if (strcmp(tok, "fwd") == 0) {
            FuncSig *sig;
            const char *name = next_tok_intern();
            if (!name)
                ir4_die("expected word name after 'fwd'");
            if (ir4_find_func(name) >= 0)
                ir4_die("duplicate 'fwd' declaration");
            alloc_func(&sig, NULL);
            sig->name = name;
            tok = next_tok();
            if (!tok || strcmp(tok, "(") != 0)
                ir4_die("expected '(' after 'fwd' name");
            parse_sig_body(sig, NULL, 0);
        } else if (strcmp(tok, "extern") == 0) {
            FuncSig ex_sig;
            const char *name;
            int existing;
            u16 cur_grp = src_stack.data[src_stack.len - 1].req_group;
            u8 cur_rank = src_stack.data[src_stack.len - 1].req_rank;
            name = next_tok_intern();
            if (!name)
                ir4_die("expected word name after 'extern'");
            memset(&ex_sig, 0, sizeof(FuncSig));
            ex_sig.name = name;
            ex_sig.is_extern = 1;
            ex_sig.is_defined = 1;
            ex_sig.req_group = cur_grp;
            ex_sig.req_rank = cur_rank;
            tok = next_tok();
            if (!tok)
                ir4_die("expected address or '(' after extern name");
            if (strcmp(tok, "(") == 0) {
                ex_sig.has_addr = 0;
            } else if (parse_int(tok, &num_val)) {
                if (num_val < 0 || num_val > 0xFFFF)
                    ir4_die("invalid extern address");
                ex_sig.addr = (u16)num_val;
                ex_sig.has_addr = 1;
                tok = next_tok();
                if (!tok || strcmp(tok, "(") != 0) {
                    ir4_die("expected '(' after extern address");
                }
            } else {
                ir4_die("expected address or '(' after extern name");
            }
            parse_sig_body(&ex_sig, NULL, 0);
            if (ex_sig.out_types.len > 1)
                ir4_die("extern supports at most 1 return value");
            existing = ir4_find_func(name);
            if (existing >= 0) {
                FuncSig *ex = funcs.data[existing];
                if (!ex->is_defined) {
                    if (!sigs_match(&ex_sig, ex))
                        ir4_die("extern signature does not match 'fwd' declaration");
                    free_sig_vecs(ex);
                    *ex = ex_sig;
                } else if (cur_grp != 0 && ex->req_group == cur_grp && cur_rank != ex->req_rank) {
                    if (!sigs_match(&ex_sig, ex))
                        ir4_die("override extern signature does not match");
                    if (cur_rank > ex->req_rank) {
                        free_sig_vecs(&ex_sig);
                    } else {
                        free_sig_vecs(ex);
                        *ex = ex_sig;
                    }
                } else {
                    free_sig_vecs(&ex_sig);
                    ir4_die("duplicate function definition");
                }
            } else {
                FuncSig *sig;
                alloc_func(&sig, NULL);
                *sig = ex_sig;
            }
        } else if (strcmp(tok, "require") == 0) {
            push_require(read_require_path());
        } else if (strcmp(tok, "data") == 0) {
            compile_data();
        } else if (strcmp(tok, "var") == 0) {
            const char *name = next_tok_intern();
            StackType ty;
            BufferSym *bs;
            if (!name) {
                ir4_die("expected var name after 'var'");
            }
            if (find_buffer(name) >= 0) {
                ir4_die("duplicate global/data name");
            }
            bs = NEW(BufferSym);
            bs->name = name;
            tok = next_tok();
            if (!tok) {
                ir4_die("expected type (:i, :a, :f) or ':bytes' after var name");
            }
            if (strcmp(tok, ":bytes") == 0) {
                long max_bytes = (be->addr_bytes <= 2) ? 0xFFFFL : 0xFFFFFFL;
                tok = next_tok();
                if (!tok || !parse_int(tok, &num_val) || num_val <= 0 || num_val > max_bytes) {
                    ir4_die("invalid byte count after ':bytes'");
                }
                bs->offset = cb_alloc_data((size_t)num_val, be->word_bytes);
                bs->type = TY_NONE;
            } else {
                size_t sz;
                ty = parse_type_tok(tok);
                if (ty == TY_NONE) {
                    ir4_die("expected ':i', ':a', ':f', or ':bytes' after var name");
                }
                sz = global_type_bytes(ty);
                bs->offset = cb_alloc_data(sz, (u8)sz);
                bs->type = ty;
            }
            VEC_PUSH(buffers, bs);
        } else {
            ir4_die("expected top-level 'fn', 'var', 'data', 'extern', or 'require'");
        }
    }

    /* Pass 1.5: Reachability / Dead-Code Elimination from 'main' */
    {
        int main_idx = ir4_find_func("main");
        size_t f;
        if (main_idx >= 0 && init_funcs.len > 0) {
            size_t wrap_idx;
            FuncSig *wsig;
            WordIR *ww;
            IrInstr *ins;
            size_t k;
            funcs.data[main_idx]->name = intern_str("__main_body");
            wrap_idx = alloc_func(&wsig, &ww);
            wsig->name = intern_str("main");
            wsig->is_defined = 1;
            for (k = 0; k < funcs.data[main_idx]->out_types.len; k++) {
                VEC_PUSH(wsig->out_types, funcs.data[main_idx]->out_types.data[k]);
            }
            ww->ir_start = ir_buf.len;
            for (k = 0; k < init_funcs.len; k++) {
                ins = emit_ir(IR_CALL, ww);
                ins->u.func_idx = init_funcs.data[k];
            }
            ins = emit_ir(IR_CALL, ww);
            ins->u.func_idx = (size_t)main_idx;
            emit_ir(IR_FN_END, ww);
            main_idx = (int)wrap_idx;
        }
        if (main_idx >= 0) {
            size_t di;
            mark_reachable((size_t)main_idx);
            for (di = 0; di < cb_data_init_reloc_count(); di++) {
                size_t po, tv;
                u8 ab;
                Bool is_c;
                cb_get_data_init_reloc(di, &po, &tv, &ab, &is_c);
                if (is_c)
                    mark_reachable(tv);
            }
        } else {
            for (f = 0; f < funcs.len; f++) {
                if (!funcs.data[f]->is_extern && funcs.data[f]->is_defined)
                    words.data[f]->reachable = true;
            }
        }
        {
            Bool *visited = (Bool *)vec_calloc(funcs.len ? funcs.len : 1, sizeof(Bool));
            for (f = 0; f < funcs.len; f++) {
                if (words.data[f]->reachable) {
                    memset(visited, 0, funcs.len * sizeof(Bool));
                    funcs.data[f]->is_recursive = can_reach_func(f, f, visited);
                }
            }
            free(visited);
        }

#if IR4_ENABLE_6502
        if (be == &be_6502)
            (void)be_6502_stack_data_off();
#endif
        heap_start_off = cb_alloc_data(0, be->word_bytes);

        /* Pass 2: Emit reachable words and patch forward calls */
        {
#if IR4_ENABLE_6502
            int saved_compact = (be == &be_6502) ? be_6502_get_compact() : 0;
#endif
            Bool *fn_compact = NULL;
            u16 max_bank = 0;
            if (cb_is_banked() && funcs.len > 0) {
                u32 soft_cap = (u32)(cb_bank_size() > 4096 ? cb_bank_size() - 4096 : cb_bank_size());
                u16 cur_b = 1;
                u32 cur_sz = 0;
                for (f = 0; f < funcs.len; f++) {
                    FuncSig *sig = funcs.data[f];
                    WordIR *w = words.data[f];
                    if (sig->is_extern || !w->reachable) {
                        sig->bank = 0;
                        continue;
                    }
                    if (sig->is_hot || w->is_code || strcmp(sig->name, "main") == 0 ||
                        strcmp(sig->name, "init") == 0) {
                        sig->bank = 0;
                    } else {
                        u32 est_sz = (u32)(w->ir_len * 64);
                        if (est_sz == 0)
                            est_sz = 16;
                        if (cur_sz > 0 && cur_sz + est_sz > soft_cap) {
                            cur_b++;
                            cur_sz = 0;
                        }
                        sig->bank = cur_b;
                        cur_sz += est_sz;
                        if (cur_b > max_bank)
                            max_bank = cur_b;
                    }
                }
            }
#if IR4_ENABLE_6502
            else if (be == &be_6502 && saved_compact == 2 && funcs.len > 0) {
                u32 *est = (u32 *)vec_calloc(funcs.len, sizeof(u32));
                u32 *heat = (u32 *)vec_calloc(funcs.len, sizeof(u32));
                u32 total_est = 0;
                size_t max_ir = 1;
                for (f = 0; f < funcs.len; f++) {
                    if (!funcs.data[f]->is_extern && words.data[f]->reachable &&
                        !words.data[f]->is_code) {
                        est[f] = (u32)(words.data[f]->ir_len * 10);
                        total_est += est[f];
                        if (words.data[f]->ir_len > max_ir)
                            max_ir = words.data[f]->ir_len;
                    }
                }
                if (total_est > 10240) {
                    u8 *depth = (u8 *)vec_calloc(max_ir, sizeof(u8));
                    u32 native_budget = 6144;
                    int pass;
                    fn_compact = (Bool *)vec_calloc(funcs.len, sizeof(Bool));
                    for (f = 0; f < funcs.len; f++) {
                        WordIR *w = words.data[f];
                        size_t i;
                        if (funcs.data[f]->is_extern || !w->reachable || w->is_code)
                            continue;
                        fn_compact[f] = !(funcs.data[f]->is_hot || w->has_inline);
                        if (funcs.data[f]->is_hot)
                            heat[f] += 10000;
                        if (funcs.data[f]->is_recursive)
                            heat[f] += 100;
                        memset(depth, 0, w->ir_len);
                        for (i = 0; i < w->ir_len; i++) {
                            const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                            if (ins->kind == IR_BR_BACK) {
                                size_t k;
                                for (k = ins->u.br.target; k <= i; k++) {
                                    if (depth[k] < 255)
                                        depth[k]++;
                                }
                            }
                        }
                        for (i = 0; i < w->ir_len; i++) {
                            const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                            u32 wgt = (depth[i] == 0) ? 0 : (depth[i] == 1 ? 10 : 100);
                            heat[f] += wgt;
                            if (ins->kind == IR_CALL && wgt > 0) {
                                heat[ins->u.func_idx] += wgt * 4;
                            }
                        }
                    }
                    /* Propagate loop heat one hop to callees of hot functions */
                    for (pass = 0; pass < 2; pass++) {
                        for (f = 0; f < funcs.len; f++) {
                            WordIR *w = words.data[f];
                            size_t i;
                            if (funcs.data[f]->is_extern || !w->reachable || w->is_code ||
                                heat[f] == 0)
                                continue;
                            for (i = 0; i < w->ir_len; i++) {
                                const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                                if (ins->kind == IR_CALL && heat[ins->u.func_idx] < heat[f]) {
                                    heat[ins->u.func_idx] += heat[f] / 2;
                                }
                            }
                        }
                    }
                    /* Greedily keep hottest functions (by heat/byte) native up to budget */
                    for (;;) {
                        size_t best_f = funcs.len;
                        u32 best_score = 0;
                        for (f = 0; f < funcs.len; f++) {
                            u32 score;
                            if (!fn_compact[f] || heat[f] == 0 || est[f] > native_budget)
                                continue;
                            score = (heat[f] * 100) / (est[f] ? est[f] : 1);
                            if (score > best_score) {
                                best_score = score;
                                best_f = f;
                            }
                        }
                        if (best_f == funcs.len)
                            break;
                        fn_compact[best_f] = false;
                        native_budget -= est[best_f];
                    }
                    free(depth);
                }
                free(est);
                free(heat);
            }
#endif
            if (cb_is_banked()) {
                u16 b;
#if IR4_ENABLE_6502
                if (be == &be_6502)
                    be_6502_set_compact(0);
#endif
                for (b = 0; b <= max_bank; b++) {
                    if (b > 0)
                        cb_begin_bank(b);
                    for (f = 0; f < funcs.len; f++) {
                        if (!funcs.data[f]->is_extern && words.data[f]->reachable &&
                            funcs.data[f]->bank == b) {
                            emit_word(f);
                        }
                    }
#if IR4_ENABLE_6502
                    if (b == 0 && max_bank > 0 && be == &be_6502) {
                        int need_mul = 0, need_div = 0, need_icall = 0, need_fp = 0;
                        for (f = 0; f < funcs.len; f++) {
                            WordIR *w = words.data[f];
                            size_t i;
                            if (funcs.data[f]->is_extern || !w->reachable || funcs.data[f]->bank == 0)
                                continue;
                            for (i = 0; i < w->ir_len; i++) {
                                const IrInstr *ins = &ir_buf.data[w->ir_start + i];
                                if (ins->kind == IR_ICALL) {
                                    need_icall = 1;
                                } else if (ins->kind == IR_ALU_OP) {
                                    IrOp op = ins->u.op;
                                    if (op == OP_MUL)
                                        need_mul = 1;
                                    else if (op == OP_DIV || op == OP_MOD || op == OP_UDIV || op == OP_UMOD)
                                        need_div = 1;
                                    else if ((op >= OP_FADD && op <= OP_FDIV) ||
                                             (op >= OP_FEQ && op <= OP_FGT) || op == OP_W2F ||
                                             op == OP_F2W)
                                        need_fp = 1;
                                }
                            }
                        }
                        be_6502_ensure_helpers(need_mul, need_div, need_icall, need_fp);
                    }
#endif
                }
                cb_end_banks();
            } else {
                for (f = 0; f < funcs.len; f++) {
                    if (!funcs.data[f]->is_extern && words.data[f]->reachable) {
#if IR4_ENABLE_6502
                        if (funcs.data[f]->is_hot)
                            be_6502_set_compact(0);
                        else if (fn_compact)
                            be_6502_set_compact(fn_compact[f] ? 1 : 0);
                        else if (be == &be_6502 && saved_compact == 2)
                            be_6502_set_compact(0);
#endif
                        emit_word(f);
                    }
                }
            }
#if IR4_ENABLE_6502
            if (be == &be_6502)
                be_6502_set_compact(saved_compact);
#endif
            free(fn_compact);
        }
        {
            size_t c;
            for (c = 0; c < fwd_calls.len; c++) {
                if (fwd_calls.data[c].is_fn_ptr) {
                    be->patch_fn(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                                 funcs.data[fwd_calls.data[c].func_idx]->addr);
                } else {
                    be->patch_call(fwd_calls.data[c].call_pc, fwd_calls.data[c].call_addr,
                                   funcs.data[fwd_calls.data[c].func_idx]->addr);
                }
            }
            for (c = 0; c < cb_data_init_reloc_count(); c++) {
                size_t po, tv;
                u8 ab;
                Bool is_c;
                cb_get_data_init_reloc(c, &po, &tv, &ab, &is_c);
                if (is_c) {
                    cb_patch_data_init_reloc(c, funcs.data[tv]->addr);
                }
            }
        }
    }
}
