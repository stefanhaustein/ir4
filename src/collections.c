#include "collections.h"
#include "ir4.h"

void *vec_grow(void *data, size_t *cap, size_t elem_size, size_t need) {
    size_t new_cap = *cap ? (*cap >= 256 ? *cap + (*cap >> 1) : *cap * 2) : 4;
    void *p;
    while (new_cap < need)
        new_cap = (new_cap >= 256) ? new_cap + (new_cap >> 1) : new_cap * 2;
    p = realloc(data, new_cap * elem_size);
    if (!p)
        ir4_die("out of memory");
    *cap = new_cap;
    return p;
}

void *vec_calloc(size_t n, size_t elem_size) {
    void *p = calloc(n, elem_size);
    if (!p)
        ir4_die("out of memory");
    return p;
}

/* String interning pool */
static char **ht_slots = NULL;
static size_t ht_cap = 0;
static size_t ht_len = 0;

static size_t hash_str_n(const char *s, size_t len) {
    size_t h = 5381U;
    size_t i;
    for (i = 0; i < len; i++) {
        h = ((h << 5) + h) ^ (unsigned char)s[i];
    }
    return h;
}

static void ht_grow(size_t new_cap) {
    char **new_slots = (char **)vec_calloc(new_cap, sizeof(char *));
    size_t i;
    size_t mask = new_cap - 1;
    for (i = 0; i < ht_cap; i++) {
        char *p = ht_slots[i];
        if (p) {
            size_t slot = hash_str_n(p, strlen(p)) & mask;
            while (new_slots[slot])
                slot = (slot + 1) & mask;
            new_slots[slot] = p;
        }
    }
    free(ht_slots);
    ht_slots = new_slots;
    ht_cap = new_cap;
}

const char *intern_find(const char *s) {
    size_t len = strlen(s);
    size_t mask, slot;
    if (ht_len == 0)
        return NULL;
    mask = ht_cap - 1;
    slot = hash_str_n(s, len) & mask;
    while (ht_slots[slot]) {
        const char *p = ht_slots[slot];
        if (memcmp(p, s, len) == 0 && p[len] == '\0')
            return p;
        slot = (slot + 1) & mask;
    }
    return NULL;
}

const char *intern_str_n(const char *s, size_t len) {
    size_t mask, slot;
    char *copy;
    if (ht_len * 10 >= ht_cap * 7)
        ht_grow(ht_cap ? ht_cap * 2 : 512);
    mask = ht_cap - 1;
    slot = hash_str_n(s, len) & mask;
    while (ht_slots[slot]) {
        const char *p = ht_slots[slot];
        if (memcmp(p, s, len) == 0 && p[len] == '\0')
            return p;
        slot = (slot + 1) & mask;
    }
    copy = (char *)malloc(len + 1);
    if (!copy)
        ir4_die("out of memory in string intern table");
    if (len > 0)
        memcpy(copy, s, len);
    copy[len] = '\0';
    ht_slots[slot] = copy;
    ht_len++;
    return copy;
}

const char *intern_str(const char *s) { return intern_str_n(s, strlen(s)); }

/* Growable string buffer */
void sb_reset(StrBuf *sb) {
    sb->len = 0;
    if (sb->data)
        sb->data[0] = '\0';
}

void sb_putc(StrBuf *sb, char c) {
    VEC_ENSURE(*sb, sb->len + 2);
    sb->data[sb->len++] = c;
    sb->data[sb->len] = '\0';
}

void sb_append_n(StrBuf *sb, const char *s, size_t n) {
    if (n == 0)
        return;
    VEC_ENSURE(*sb, sb->len + n + 1);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

void sb_append(StrBuf *sb, const char *s) { sb_append_n(sb, s, strlen(s)); }

const char *sb_intern(StrBuf *sb) { return intern_str_n(sb->data ? sb->data : "", sb->len); }

/* Pointer-keyed symbol hash table */
static size_t sym_ptr_hash(const char *p) {
    size_t v = (size_t)p;
    return (v >> 2) ^ (v >> 9);
}

static void sym_ht_grow(SymHt *ht, size_t new_cap) {
    const char **nkeys = (const char **)vec_calloc(new_cap, sizeof(const char *));
    size_t *nvals = (size_t *)vec_calloc(new_cap, sizeof(size_t));
    size_t i;
    size_t mask = new_cap - 1;
    for (i = 0; i < ht->cap; i++) {
        if (ht->keys[i]) {
            size_t slot = sym_ptr_hash(ht->keys[i]) & mask;
            while (nkeys[slot])
                slot = (slot + 1) & mask;
            nkeys[slot] = ht->keys[i];
            nvals[slot] = ht->vals[i];
        }
    }
    free(ht->keys);
    free(ht->vals);
    ht->keys = nkeys;
    ht->vals = nvals;
    ht->cap = new_cap;
}

int sym_ht_find(const SymHt *ht, const char *p) {
    size_t mask, slot;
    if (ht->len == 0)
        return -1;
    mask = ht->cap - 1;
    slot = sym_ptr_hash(p) & mask;
    while (ht->keys[slot]) {
        if (ht->keys[slot] == p)
            return (int)ht->vals[slot];
        slot = (slot + 1) & mask;
    }
    return -1;
}

void sym_ht_put(SymHt *ht, const char *p, size_t val) {
    size_t mask, slot;
    if (ht->len * 10 >= ht->cap * 7)
        sym_ht_grow(ht, ht->cap ? ht->cap * 2 : 128);
    mask = ht->cap - 1;
    slot = sym_ptr_hash(p) & mask;
    while (ht->keys[slot]) {
        if (ht->keys[slot] == p) {
            ht->vals[slot] = val;
            return;
        }
        slot = (slot + 1) & mask;
    }
    ht->keys[slot] = p;
    ht->vals[slot] = val;
    ht->len++;
}

void sym_ht_clear(SymHt *ht) {
    if (ht->keys && ht->cap > 0)
        memset(ht->keys, 0, ht->cap * sizeof(const char *));
    ht->len = 0;
}
