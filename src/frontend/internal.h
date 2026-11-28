/* internal.h - Internal declarations shared by frontend modules */
#ifndef FRONTEND_INT_H
#define FRONTEND_INT_H

#include "frontend.h"
#include "vec.h"

VEC_OF(ByteVec, u8);
VEC_OF(LocalVec, LocalVar);
VEC_OF(SizeVec, size_t);
VEC_OF(FuncVec, FuncSig *);

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
    IR_JTAB
} IrKind;

typedef struct {
    u8 kind; /* IrKind */
    union {
        i32 w;
        u32 a;
        double f;
        size_t offset;
        struct {
            u8 op;   /* LocalOp */
            u8 type; /* StackType */
            u16 stack_bytes;
            size_t slot;
        } loc;
        struct {
            u8 op; /* StackOp */
            u8 t1; /* StackType */
            u8 t2; /* StackType */
        } stk;
        u8 op; /* IrOp */
        size_t func_idx;
        struct {
            u8 cond; /* BranchCond */
            size_t target;
        } br;
        struct {
            size_t start;
            size_t count;
        } jtab;
    } u;
} IrInstr;

enum { CODE_REL_SELF = 1, CODE_REL_STACK = 2, CODE_REL_DATA = 3 };

typedef struct {
    size_t ir_off;
    size_t addend;
    u8 kind; /* CODE_REL_* */
} CodeReloc;

VEC_OF(IrVec, IrInstr);

typedef struct {
    IrVec ir;
    size_t raw_start;
    size_t raw_len;
    size_t reloc_start;
    size_t reloc_cnt;
    size_t param_cnt;
    LocalVec locals;
    Bool reachable;
    Bool is_code;
} WordIR;

VEC_OF(WordVec, WordIR *);
VEC_OF(CodeRelocVec, CodeReloc);

typedef struct {
    FILE *fp;
    const char *path;
    u32 line_num;
    u32 hdr_asm_hash;
    u16 req_group;
    u8 req_rank;
    u8 can_reopen;
    u8 buf[512];
    size_t buf_pos;
    size_t buf_len;
    int pushback;
} SrcFile;

/* Shared compiler state (defined in frontend.c) */
extern const Backend *be;
extern const char *platform_name;
extern FuncVec funcs;
extern WordVec words;
extern SizeVec jtab_targets;
extern ByteVec raw_code_buf;
extern CodeRelocVec code_relocs;
extern SizeVec init_funcs;

size_t alloc_func(FuncSig **sig_out, WordIR **w_out);
IrInstr *emit_ir(IrKind kind, WordIR *w);

/* Lexer & require loader (defined in lexer.c) */
void lex_init(FILE *in, const char *path);
SrcFile *lex_cur(void);
int src_getc(SrcFile *sf);
void src_ungetc(SrcFile *sf, int c);
int skip_ws_and_comments(void);
const char *next_tok(void);
const char *next_tok_intern(void);
void lex_begin_code_asm(void);
void lex_feed_code_tok(const char *tok);
void lex_finish_code_asm(void);
void push_require_ex(const char *name, int lib_only);
const char *read_require_path(void);
void push_require(const char *name);
StackType parse_type_tok(const char *tok);
int parse_int(const char *s, i32 *val);
int parse_addr(const char *s, u32 *val);
int parse_float(const char *s, double *val);

/* DCE & Pass 2 emitter (defined in ir_emit.c) */
void ir_emit_reset(void);
void run_reachability_dce(void);
void emit_program(void);

#endif /* FRONTEND_INT_H */
