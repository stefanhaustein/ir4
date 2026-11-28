/* collections.h - Dynamic vectors, string buffer, interning pool, and symbol map */
#ifndef IR4_COLLECTIONS_H
#define IR4_COLLECTIONS_H

#include <stdlib.h>
#include <string.h>

/* Dynamic typed vector */
#define VEC_OF(Name, T) \
    typedef struct {    \
        T *data;        \
        size_t len;     \
        size_t cap;     \
    } Name

void *vec_grow(void *data, size_t *cap, size_t elem_size, size_t need);
void *vec_calloc(size_t n, size_t elem_size);

#define NEW(T) ((T *)vec_calloc(1, sizeof(T)))

#define VEC_ENSURE(v, need)                                                                       \
    (((need) > (v).cap) ? ((v).data = vec_grow((v).data, &(v).cap, sizeof(*(v).data), (need)))    \
                        : (v).data)

#define VEC_ADD(v) (VEC_ENSURE((v), (v).len + 1), &(v).data[(v).len++])

#define VEC_PUSH(v, val) (*VEC_ADD(v) = (val))

#define VEC_INSERT(v, idx, val)                                                                   \
    do {                                                                                          \
        size_t _i = (idx);                                                                        \
        VEC_ENSURE((v), (v).len + 1);                                                             \
        if (_i < (v).len) {                                                                       \
            memmove(&(v).data[_i + 1], &(v).data[_i], ((v).len - _i) * sizeof(*(v).data));        \
        }                                                                                         \
        (v).data[_i] = (val);                                                                     \
        (v).len++;                                                                                \
    } while (0)

/* Growable string scratch buffer */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_reset(StrBuf *sb);
void sb_putc(StrBuf *sb, char c);
void sb_append_n(StrBuf *sb, const char *s, size_t n);
void sb_append(StrBuf *sb, const char *s);
const char *sb_intern(StrBuf *sb);

/* String interning pool (DJB2a open-addressing) */
const char *intern_find(const char *s);
const char *intern_find_n(const char *s, size_t len);
const char *intern_str(const char *s);
const char *intern_str_n(const char *s, size_t len);

/* Pointer-keyed open-addressing hash table (for interned symbol -> index) */
typedef struct {
    const char **keys;
    size_t *vals;
    size_t cap;
    size_t len;
} SymHt;

int sym_ht_find(const SymHt *ht, const char *p);
void sym_ht_put(SymHt *ht, const char *p, size_t val);
void sym_ht_clear(SymHt *ht);

#endif
