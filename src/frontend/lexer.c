/* lexer.c - Tokenizer, #line directives, and require resolution */
#include "internal.h"
#include "path.h"
#include "strpool.h"

VEC_OF(SrcVec, SrcFile);

static SrcVec src_stack;
static u16 next_req_group = 1;
static StrVec extra_lib_roots;
static StrVec seen_files;
static StrBuf tok_sb;

void ir4_add_lib_root(const char *path) {
    if (!path || path[0] == '\0')
        return;
    VEC_PUSH(extra_lib_roots, path);
}

#if defined(__ez80__) || defined(__EZ80__) || defined(IR4_MINIMAL) || defined(IR4_STATUS)
static size_t status_len = 0;

static const char *short_path(const char *path) {
    const char *p = path;
    const char *last = path;
    if (!path)
        return "";
    while (*p) {
        if (*p == '/' || *p == '\\')
            last = p + 1;
        p++;
    }
    return last;
}

void ir4_status(const char *msg) {
    size_t n = strlen(msg) + 2;
    size_t i;
    fprintf(stdout, "\r  %s", msg);
    for (i = n; i < status_len; i++)
        fputc(' ', stdout);
    fflush(stdout);
    status_len = n;
}

void ir4_status_file(const char *prefix, const char *path, u32 line_num) {
    char buf[72];
    const char *sp = short_path(path);
    if (line_num > 0)
        sprintf(buf, "%s %.40s:%lu", prefix, sp, (unsigned long)line_num);
    else
        sprintf(buf, "%s %.48s...", prefix, sp);
    ir4_status(buf);
}

void ir4_status_prog(const char *prefix, size_t done, size_t total) {
    char buf[72];
    sprintf(buf, "%s (%lu/%lu)...", prefix, (unsigned long)done, (unsigned long)total);
    ir4_status(buf);
}

void ir4_status_clear(void) {
    size_t i;
    if (status_len > 0) {
        fputc('\r', stdout);
        for (i = 0; i < status_len; i++)
            fputc(' ', stdout);
        fputc('\r', stdout);
        fflush(stdout);
        status_len = 0;
    }
}
#else
void ir4_status(const char *msg) { (void)msg; }
void ir4_status_file(const char *prefix, const char *path, u32 line_num) {
    (void)prefix;
    (void)path;
    (void)line_num;
}
void ir4_status_prog(const char *prefix, size_t done, size_t total) {
    (void)prefix;
    (void)done;
    (void)total;
}
void ir4_status_clear(void) {}
#endif

static void bump_line(SrcFile *cur) {
    cur->line_num++;
    if ((cur->line_num & 63) == 0)
        ir4_status_file("Parsing", cur->path, cur->line_num);
}

void ir4_die(const char *msg) {
    ir4_status_clear();
    if (src_stack.len > 0) {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        fprintf(stderr, "%s:%lu: error: ir4c: %s\n", cur->path, (unsigned long)cur->line_num, msg);
    } else {
        fprintf(stderr, "error: ir4c: %s\n", msg);
    }
    exit(1);
}

void lex_init(FILE *in, const char *path) {
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
    ir4_status_file("Parsing", root_path, 0);
}

SrcFile *lex_cur(void) {
    return (src_stack.len > 0) ? &src_stack.data[src_stack.len - 1] : NULL;
}

StackType parse_type_tok(const char *tok) {
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

int src_getc(SrcFile *sf) {
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

void src_ungetc(SrcFile *sf, int c) {
    sf->pushback = c;
}

int skip_ws_and_comments(void) {
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
                cur = &src_stack.data[src_stack.len - 1];
                ir4_status_file("Parsing", cur->path, cur->line_num);
                continue;
            }
            return EOF;
        }
        if (c == '\n') {
            bump_line(cur);
            continue;
        }
        if (isspace((unsigned char)c))
            continue;
        if (c == '\\') {
            int c2 = src_getc(cur);
            if (c2 == EOF || c2 == '\n' || isspace((unsigned char)c2)) {
                if (c2 == '\n') {
                    bump_line(cur);
                } else if (c2 != EOF) {
                    while ((c = src_getc(cur)) != EOF && c != '\n')
                        ;
                    if (c == '\n')
                        bump_line(cur);
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

const char *next_tok(void) {
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
                bump_line(cur);
        }
        return tok_sb.data;
    }
}

const char *next_tok_intern(void) { return next_tok() ? sb_intern(&tok_sb) : NULL; }

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

void push_require_ex(const char *name, int lib_only) {
    const char *cands[3];
    int n_cands = 0;
    int c;
    FILE *found_fps[3];
    const char *found_paths[3];
    u8 found_ranks[3];
    int found_cnt = 0;
    int seen_cnt = 0;

    if (name[0] != '/') {
        if (platform_name != NULL && platform_name[0] != '\0') {
            cands[n_cands++] = path_join(platform_name, name);
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
        ir4_status_file("Parsing", src_stack.data[src_stack.len - 1].path, 0);
        return;
    }
    if (seen_cnt > 0)
        return;

    {
        const SrcFile *cur = &src_stack.data[src_stack.len - 1];
        ir4_status_clear();
        fprintf(stderr, "%s:%lu: error: ir4c: cannot open required file '%s'\n", cur->path,
                (unsigned long)cur->line_num, name);
        exit(1);
    }
}

const char *read_require_path(void) {
    int c = skip_ws_and_comments();
    SrcFile *cur = &src_stack.data[src_stack.len - 1];

    if (c != '"')
        ir4_die("expected quoted filename after 'require'");
    return read_quoted_intern(cur, "unterminated filename in 'require'");
}

void push_require(const char *name) { push_require_ex(name, 0); }

int parse_int(const char *s, i32 *val) {
    char *endp;
    int base = 0;
    const char *p = s;

    if (*p == '$') {
        base = 16;
        p++;
        if (*p == '\0')
            return 0;
    } else if ((p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) ||
               ((p[0] == '-' || p[0] == '+') && p[1] == '0' &&
                (p[2] == 'b' || p[2] == 'B'))) {
        int neg = (p[0] == '-');
        p += (p[0] == '-' || p[0] == '+') ? 3 : 2;
        if (*p == '\0')
            return 0;
        *val = (i32)strtol(p, &endp, 2);
        if (neg)
            *val = -*val;
        return (*endp == '\0');
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

int parse_addr(const char *s, u32 *val) {
    unsigned long uv = 0;
    const char *p = s;
    unsigned int base = 10;

    if (!s || *s == '\0' || *s == '-')
        return 0;
    if (*p == '+')
        p++;
    if (*p == '$') {
        base = 16;
        p++;
    } else if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        base = 2;
        p += 2;
    }
    if (*p == '\0')
        return 0;
    while (*p) {
        unsigned int d;
        if (*p >= '0' && *p <= '9')
            d = (unsigned int)(*p - '0');
        else if (base == 16 && *p >= 'a' && *p <= 'f')
            d = (unsigned int)(*p - 'a' + 10);
        else if (base == 16 && *p >= 'A' && *p <= 'F')
            d = (unsigned int)(*p - 'A' + 10);
        else
            return 0;
        if (d >= base)
            return 0;
        if (uv > (0xFFFFFFFFUL - d) / base)
            return 0;
        uv = uv * base + d;
        p++;
    }
    *val = (u32)uv;
    return 1;
}

int parse_float(const char *s, double *val) {
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
