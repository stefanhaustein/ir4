#include "strpool.h"
#include "vec.h"

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
    char **new_slots = (char **)calloc(new_cap, sizeof(char *));
    size_t i;
    size_t mask = new_cap - 1;
    if (!new_slots)
        ir4_die("out of memory in string intern table");
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
