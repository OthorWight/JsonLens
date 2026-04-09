/*
 * ============================================================================
 * arena_json.h - Ultra-fast, Arena-backed JSON Parser for C
 * ============================================================================
 */

#ifndef ARENA_JSON_H
#define ARENA_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef ARENA_DEFAULT_BLOCK_SIZE
#define ARENA_DEFAULT_BLOCK_SIZE (8 * 1024) 
#endif

#ifndef ARENA_ALIGNMENT
#define ARENA_ALIGNMENT 8 
#endif

#ifndef ARENA_MAX_BLOCK_SIZE
#define ARENA_MAX_BLOCK_SIZE (32 * 1024 * 1024) 
#endif

/* ============================================================================
 * ARENA ALLOCATOR
 * ============================================================================ */

typedef struct ArenaRegion ArenaRegion;

#ifdef __cplusplus
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
struct ArenaRegion {
    ArenaRegion *next;
    size_t capacity;
    size_t count;
    size_t _padding; 
    uint8_t data[];
};
#ifdef __cplusplus
#pragma GCC diagnostic pop
#endif

typedef struct Arena {
    ArenaRegion *begin;
    ArenaRegion *end;
    uint64_t _inline_buffer[(sizeof(ArenaRegion) + 1024 + 7) / 8];
} Arena;

typedef struct ArenaTemp {
    Arena *arena;
    ArenaRegion *old_end;
    size_t old_count;
} ArenaTemp;

#ifdef __cplusplus
extern "C" {
#endif

void arena_init(Arena *a);
void *arena_alloc_fallback(Arena *a, size_t size);
void *arena_zalloc(Arena *a, size_t size);
void arena_reset(Arena *a);
void arena_free(Arena *a);

static inline void *arena_alloc(Arena *a, size_t size) {
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(size == 0, 0)) return NULL;
    size_t aligned_size = (size + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);
    if (__builtin_expect(a->end != NULL, 1)) {
        size_t new_count = a->end->count + aligned_size;
        if (__builtin_expect(new_count <= a->end->capacity, 1)) {
            void *ptr = a->end->data + a->end->count;
            a->end->count = new_count;
            return ptr;
        }
    }
#else
    if (size == 0) return NULL;
    size_t aligned_size = (size + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);
    if (a->end) {
        size_t new_count = a->end->count + aligned_size;
        if (new_count <= a->end->capacity) {
            void *ptr = a->end->data + a->end->count;
            a->end->count = new_count;
            return ptr;
        }
    }
#endif
    return arena_alloc_fallback(a, size);
}

ArenaTemp arena_temp_begin(Arena *a);
void arena_temp_end(ArenaTemp temp);

#define arena_alloc_struct(a, T) ((T*)arena_alloc(a, sizeof(T)))
#define arena_alloc_array(a, T, count) ((T*)arena_alloc(a, sizeof(T) * (count)))

/* ============================================================================
 * JSON PARSER & DOM
 * ============================================================================ */

typedef struct {
    char msg[128]; 
    int line;      
    int col;       
    size_t offset; 
} JsonError;

typedef enum {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonNode JsonNode;

typedef struct JsonValue {
    union {
        bool boolean;
        double number;
        char *string;
        struct { 
            JsonNode *items; 
            size_t count;
        } list; 
    } as;
    uint64_t type : 4;
    uint64_t offset : 60;
    char *pre_comment;
    char *post_comment;
    char *trailing_comment;
} JsonValue;

struct JsonNode {
    char *pre_comment;
    char *key;       
    JsonValue value; 
};

/* --- Core API --- */
#define JSON_PARSE_STRICT 0
#define JSON_PARSE_ALLOW_COMMENTS 1

JsonValue *json_parse(Arena *main, Arena *scratch, const char *input, size_t len, int flags, JsonError *err);
JsonValue *json_object_get(JsonValue *obj, const char *key);
char *json_serialize(Arena *a, JsonValue *v, bool pretty, bool use_tabs, int indent_step, bool keep_comments);

/* --- New QoL & Mutation API --- */
double      json_object_get_number(JsonValue *obj, const char *key, double fallback);
const char* json_object_get_string(JsonValue *obj, const char *key, const char *fallback);
bool        json_object_get_bool(JsonValue *obj, const char *key, bool fallback);
JsonValue * json_object_get_case_insensitive(JsonValue *obj, const char *key);

void        json_object_remove(JsonValue *obj, const char *key);
void        json_object_replace(Arena *a, JsonValue *obj, const char *key, const JsonValue *val);
JsonValue * json_object_detach(Arena *a, JsonValue *obj, const char *key);
JsonValue * json_clone(Arena *a, const JsonValue *val);

#define json_object_foreach(entry, obj) \
    for (size_t _idx = 0; (obj) && ((obj)->type == JSON_OBJECT) && _idx < (obj)->as.list.count && ((entry) = &(obj)->as.list.items[_idx], 1); ++_idx)
#define json_array_foreach(entry, arr) \
    for (size_t _idx = 0; (arr) && ((arr)->type == JSON_ARRAY) && _idx < (arr)->as.list.count && ((entry) = &(arr)->as.list.items[_idx], 1); ++_idx)

/* --- Builder API --- */
JsonValue *json_create_null(Arena *a);
JsonValue *json_create_bool(Arena *a, bool b);
JsonValue *json_create_number(Arena *a, double num);
JsonValue *json_create_string(Arena *a, const char *str);
JsonValue *json_create_array(Arena *a);
JsonValue *json_create_object(Arena *a);

void json_object_add(Arena *a, JsonValue *obj, const char *key, const JsonValue *val);
void json_object_add_string(Arena *a, JsonValue *obj, const char *key, const char *val);
void json_object_add_number(Arena *a, JsonValue *obj, const char *key, double val);
void json_object_add_bool(Arena *a, JsonValue *obj, const char *key, bool val);

void json_array_append(Arena *a, JsonValue *arr, const JsonValue *val);
void json_array_append_string(Arena *a, JsonValue *arr, const char *val);
void json_array_append_number(Arena *a, JsonValue *arr, double val);

#ifdef __cplusplus
}
#endif

#endif /* ARENA_JSON_H */

/* ============================================================================
 * IMPLEMENTATION
 * ============================================================================ */

#ifdef ARENA_JSON_IMPLEMENTATION

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <tmmintrin.h>
#include <emmintrin.h>
#include <smmintrin.h>
#endif

/* --- Arena Implementation --- */

static ArenaRegion *arena__new_region(size_t capacity) {
    size_t size = sizeof(ArenaRegion) + capacity;
    ArenaRegion *r = (ArenaRegion *)malloc(size);
    if (!r) return NULL;
    r->next = NULL;
    r->capacity = capacity;
    r->count = 0;
    r->_padding = 0;
    return r;
}

void arena_init(Arena *a) {
    ArenaRegion *inline_reg = (ArenaRegion *)a->_inline_buffer;
    inline_reg->next = NULL;
    inline_reg->capacity = 1024;
    inline_reg->count = 0;
    inline_reg->_padding = 0;
    a->begin = inline_reg;
    a->end = inline_reg;
}

void *arena_alloc_fallback(Arena *a, size_t size) {
    size_t aligned_size = (size + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);

    if (a->end == NULL) {
        if (a->begin != NULL) {
            a->end = a->begin;
            a->end->count = 0;
        } else {
            ArenaRegion *inline_reg = (ArenaRegion *)a->_inline_buffer;
            inline_reg->next = NULL;
            inline_reg->capacity = 1024;
            inline_reg->count = 0;
            inline_reg->_padding = 0;
            a->begin = inline_reg;
            a->end = a->begin;
        }
        if (a->end->count + aligned_size <= a->end->capacity) {
            void *ptr = a->end->data + a->end->count;
            a->end->count += aligned_size;
            return ptr;
        }
    }

    while (a->end->next != NULL) {
        ArenaRegion *next = a->end->next;
        if (next->capacity >= aligned_size) {
            a->end = next;
            a->end->count = aligned_size;
            return (void *)a->end->data;
        } else {
            a->end->next = next->next; 
            free(next);                
        }
    }

    size_t new_cap = a->end->capacity * 2;
    if (new_cap > ARENA_MAX_BLOCK_SIZE) {
        new_cap = ARENA_MAX_BLOCK_SIZE;
    }
    if (aligned_size > new_cap) new_cap = aligned_size;
    if (new_cap < ARENA_DEFAULT_BLOCK_SIZE) new_cap = ARENA_DEFAULT_BLOCK_SIZE;

    ArenaRegion *next = arena__new_region(new_cap);
    if (!next) return NULL;

    a->end->next = next;
    a->end = next;
    a->end->count = aligned_size;
    return (void *)a->end->data;
}

void *arena_zalloc(Arena *a, size_t size) {
    void *ptr = arena_alloc(a, size);
    if (ptr) memset(ptr, 0, size);
    return ptr;
}

void arena_reset(Arena *a) {
    if (a->begin) {
        a->end = a->begin;
        a->end->count = 0;
    } else {
        a->end = NULL;
    }
}

void arena_free(Arena *a) {
    ArenaRegion *curr = a->begin;
    ArenaRegion *inline_reg = (ArenaRegion *)a->_inline_buffer;
    while (curr) {
        ArenaRegion *next = curr->next;
        if (curr != inline_reg) {
            free(curr);
        }
        curr = next;
    }
    a->begin = NULL;
    a->end = NULL;
}

ArenaTemp arena_temp_begin(Arena *a) {
    ArenaTemp temp;
    temp.arena = a;
    temp.old_end = a->end;
    temp.old_count = a->end ? a->end->count : 0;
    return temp;
}

void arena_temp_end(ArenaTemp temp) {
    temp.arena->end = temp.old_end;
    if (temp.arena->end) temp.arena->end->count = temp.old_count;
}

/* --- JSON Implementation --- */

#define MAX_JSON_DEPTH 1000

typedef struct KeyCacheEntry {
    const char *source_str;
    size_t source_len;
    char *cached_str;
} KeyCacheEntry;

#define KEY_CACHE_CAPACITY 16384

typedef struct {
    const char *start;
    const char *curr;
    const char *end;
    JsonError *err;
    Arena *scratch; 
    int flags;
    KeyCacheEntry *key_cache;
    uint32_t key_cache_mask;
    char *last_comment;
    char *inline_comment;
} ParseState;

/* --- High-Performance Parsing Helpers --- */

static inline bool is_made_of_eight_digits_fast(const uint8_t *chars) {
    uint64_t val;
    memcpy(&val, chars, 8);
    return (((val & 0xF0F0F0F0F0F0F0F0) |
             (((val + 0x0606060606060606) & 0xF0F0F0F0F0F0F0F0) >> 4)) ==
            0x3333333333333333);
}

static inline uint32_t parse_eight_digits(const uint8_t *chars) {
    uint64_t val;
    memcpy(&val, chars, 8);
    val = (val & 0x0F0F0F0F0F0F0F0F) * 2561 >> 8;
    val = (val & 0x00FF00FF00FF00FF) * 6553601 >> 16;
    return (uint32_t)((val & 0x0000FFFF0000FFFF) * 42949672960001 >> 32);
}

static const double power_of_ten[] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

static inline bool parse_number_fast_path(ParseState *s, double *out_dbl) {
    const char *p = s->curr;
    bool negative = (*p == '-');
    if (negative) p++;

    if (p >= s->end) return false;

    uint64_t i = 0;
    const char *start_digits = p;
    if (*p == '0') {
        p++;
        if (p < s->end && *p >= '0' && *p <= '9') return false;
    } else if (*p >= '1' && *p <= '9') {
        // SWAR parsing: 8 digits at a time!
        while (p + 8 <= s->end && is_made_of_eight_digits_fast((const uint8_t*)p)) {
            i = i * 100000000 + parse_eight_digits((const uint8_t*)p);
            p += 8;
        }
        while (p < s->end && *p >= '0' && *p <= '9') {
            i = i * 10 + (*p - '0');
            p++;
        }
        if (p - start_digits > 15) return false;
    } else {
        return false;
    }

    int64_t exponent = 0;
    bool is_float = false;

    if (p < s->end && *p == '.') {
        is_float = true;
        p++;
        const char *frac_start = p;
        while (p + 8 <= s->end && is_made_of_eight_digits_fast((const uint8_t*)p)) {
            i = i * 100000000 + parse_eight_digits((const uint8_t*)p);
            p += 8;
        }
        while (p < s->end && *p >= '0' && *p <= '9') {
            i = i * 10 + (*p - '0');
            p++;
        }
        if (p == frac_start) return false;
        if (p - start_digits > 16) return false;
        exponent -= (p - frac_start);
    }

    if (p < s->end && (*p == 'e' || *p == 'E')) {
        is_float = true;
        p++;
        bool exp_neg = false;
        if (p < s->end && (*p == '-' || *p == '+')) { exp_neg = (*p == '-'); p++; }
        uint64_t exp_val = 0;
        const char *exp_start = p;
        while (p < s->end && *p >= '0' && *p <= '9') {
            if (exp_val < 1000000) {
                exp_val = exp_val * 10 + (*p - '0');
            }
            p++;
        }
        if (p == exp_start) return false;
        exponent += exp_neg ? -exp_val : exp_val;
    }

    if (is_float) {
        // Fast-path: Exponent within standard double precision limits
        if (exponent >= -22 && exponent <= 22 && i <= 9007199254740991ULL) {
            double d = (double)i;
            if (exponent < 0) d /= power_of_ten[-exponent];
            else d *= power_of_ten[exponent];
            *out_dbl = negative ? -d : d;
            s->curr = p;
            return true;
        }
        return false; // Requires strtod/fallback!
    } else {
        *out_dbl = negative ? -(double)i : (double)i;
        s->curr = p;
        return true; 
    }
}

typedef struct NodeBlock NodeBlock;
struct NodeBlock {
    JsonNode *items;
    size_t capacity;
    size_t count;
    NodeBlock *next;
};

static void set_error(ParseState *s, const char *fmt, ...) {
    if (s->err) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(s->err->msg, sizeof(s->err->msg), fmt, args);
        va_end(args);
        int line = 1, col = 1;
        for (const char *p = s->start; p < s->curr; p++) {
            if (*p == '\n') { line++; col = 1; } else { col++; }
        }
        s->err->line = line;
        s->err->col = col;
        s->err->offset = s->curr - s->start;
    }
}

static void advance(ParseState *s, int n) { s->curr += n; }

static const unsigned char ws_lut[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, /* \t, \n, \r */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0  /* Space */
};

static void skip_whitespace(ParseState *s) {
    const char *p = s->curr;
    const char *end = s->end;

    if (!(s->flags & JSON_PARSE_ALLOW_COMMENTS)) {
        if (p < end && !ws_lut[(unsigned char)*p]) return; // Fast reject for minified JSON
        while (p < end && ws_lut[(unsigned char)*p]) p++;
        s->curr = p;
        return;
    }

    if (p < end && !ws_lut[(unsigned char)*p] && *p != '/') return; // Fast reject for comments

    char *comments = NULL;
    size_t comm_cap = 0, comm_len = 0;
    
    char *inline_comm = NULL;
    size_t inl_comm_cap = 0, inl_comm_len = 0;
    bool seen_newline = false;

    while (1) {
        p = s->curr;
        const char *p_start = p;
        while (p < end && ws_lut[(unsigned char)*p]) p++;
        
        if (!seen_newline) {
            if (memchr(p_start, '\n', p - p_start)) {
                seen_newline = true;
            }
        }
        
        s->curr = p;

        if (p + 1 < end && p[0] == '/') {
            const char *comm_start = p;
            bool was_newline = seen_newline;
            
            if (p[1] == '/') {
                p += 2;
                while (p < end && *p != '\n') p++;
            } else if (p[1] == '*') {
                p += 2;
                while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) {
                    if (*p == '\n') seen_newline = true;
                    p++;
                }
                if (p + 1 < end) p += 2;
            } else {
                break;
            }

                size_t len = p - comm_start;
                
                if (!was_newline && !comments) {
                    if (!inline_comm) {
                        inl_comm_cap = len + 128;
                        inline_comm = arena_alloc_array(s->scratch, char, inl_comm_cap);
                        memcpy(inline_comm, comm_start, len);
                        inl_comm_len = len;
                    } else {
                        if (inl_comm_len + len + 2 > inl_comm_cap) {
                            size_t new_cap = inl_comm_cap + len + 128;
                            char *new_comm = arena_alloc_array(s->scratch, char, new_cap);
                            memcpy(new_comm, inline_comm, inl_comm_len);
                            inline_comm = new_comm;
                            inl_comm_cap = new_cap;
                        }
                        inline_comm[inl_comm_len++] = ' ';
                        memcpy(inline_comm + inl_comm_len, comm_start, len);
                        inl_comm_len += len;
                    }
                    inline_comm[inl_comm_len] = '\0';
                } else {
                    if (!comments) {
                        comm_cap = len + 128;
                        comments = arena_alloc_array(s->scratch, char, comm_cap);
                        memcpy(comments, comm_start, len);
                        comm_len = len;
                    } else {
                        if (comm_len + len + 2 > comm_cap) {
                            size_t new_cap = comm_cap + len + 128;
                            char *new_comm = arena_alloc_array(s->scratch, char, new_cap);
                            memcpy(new_comm, comments, comm_len);
                            comments = new_comm;
                            comm_cap = new_cap;
                        }
                        comments[comm_len++] = '\n';
                        memcpy(comments + comm_len, comm_start, len);
                        comm_len += len;
                    }
                    comments[comm_len] = '\0';
                }
                
                s->curr = p;
                continue;
        }
        break;
    }

    if (inline_comm) {
        if (s->inline_comment) {
            size_t exist_len = strlen(s->inline_comment);
            char *merged = arena_alloc_array(s->scratch, char, exist_len + inl_comm_len + 2);
            memcpy(merged, s->inline_comment, exist_len);
            merged[exist_len] = ' ';
            memcpy(merged + exist_len + 1, inline_comm, inl_comm_len);
            merged[exist_len + 1 + inl_comm_len] = '\0';
            s->inline_comment = merged;
        } else {
            s->inline_comment = inline_comm;
        }
    }

    if (comments) {
        if (s->last_comment) {
            size_t exist_len = strlen(s->last_comment);
            char *merged = arena_alloc_array(s->scratch, char, exist_len + comm_len + 2);
            memcpy(merged, s->last_comment, exist_len);
            merged[exist_len] = '\n';
            memcpy(merged + exist_len + 1, comments, comm_len);
            merged[exist_len + 1 + comm_len] = '\0';
            s->last_comment = merged;
        } else {
            s->last_comment = comments;
        }
    }
}

static JsonValue *make_value(Arena *a, JsonType type) {
    JsonValue *v = arena_alloc_struct(a, JsonValue);
    if (v) {
        v->type = type;
        v->offset = 0;
        v->pre_comment = NULL;
        v->post_comment = NULL;
        v->trailing_comment = NULL;
        if (type == JSON_ARRAY || type == JSON_OBJECT) {
            v->as.list.items = NULL;
            v->as.list.count = 0;
        }
    }
    return v;
}

static char *combine_comments(Arena *a, const char *inline_comm, const char *last_comm) {
    if (inline_comm && last_comm) {
        size_t l1 = strlen(inline_comm);
        size_t l2 = strlen(last_comm);
        char *res = arena_alloc_array(a, char, l1 + l2 + 2);
        strcpy(res, inline_comm);
        res[l1] = '\n';
        strcpy(res + l1 + 1, last_comm);
        return res;
    } else if (inline_comm) {
        size_t len = strlen(inline_comm);
        char *res = arena_alloc_array(a, char, len + 1);
        strcpy(res, inline_comm);
        return res;
    } else if (last_comm) {
        size_t len = strlen(last_comm);
        char *res = arena_alloc_array(a, char, len + 1);
        strcpy(res, last_comm);
        return res;
    }
    return NULL;
}

static bool parse_element(Arena *main, ParseState *s, JsonValue *out_val, int depth);

static const unsigned char string_stop_table[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static bool parse_string(Arena *a, ParseState *s, char **out_str) {
    advance(s, 1); 
    const char *start_content = s->curr;
    const char *scan = s->curr;
    bool has_escapes = false;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
    if (s->end - scan >= 16) {
        const __m128i v_quote = _mm_set1_epi8('"');
        const __m128i v_esc   = _mm_set1_epi8('\\');
        const __m128i v_ctrl_threshold = _mm_set1_epi8(0x20);
        const __m128i v_zero  = _mm_setzero_si128();

        while (scan + 16 <= s->end) {
            __m128i data = _mm_loadu_si128((const __m128i *)scan);
            __m128i is_quote = _mm_cmpeq_epi8(data, v_quote);
            __m128i is_esc   = _mm_cmpeq_epi8(data, v_esc);
            __m128i under_20 = _mm_subs_epu8(v_ctrl_threshold, data);
            __m128i is_ctrl  = _mm_andnot_si128(_mm_cmpeq_epi8(under_20, v_zero), _mm_set1_epi8(-1));
            
            __m128i match = _mm_or_si128(is_quote, _mm_or_si128(is_esc, is_ctrl));
            uint32_t mask = (uint32_t)_mm_movemask_epi8(match);
            if (mask != 0) {
                scan += __builtin_ctz(mask);
                break;
            }
            scan += 16;
        }
    }
#endif
    while (scan < s->end) {
        while (scan < s->end && !string_stop_table[(unsigned char)*scan]) {
            scan++;
        }
        if (scan >= s->end) break;
        unsigned char c = (unsigned char)*scan;
        if (c == '"') break;
        if (c == '\\') {
            has_escapes = true;
            scan++; 
            if (scan >= s->end) { set_error(s, "Unterminated escape"); return false; }
        } else {
            set_error(s, "Control character in string"); return false;
        }
        scan++;
    }
    if (scan >= s->end) { set_error(s, "Unterminated string"); return false; }

    size_t raw_len = scan - start_content;

    KeyCacheEntry *entry = NULL;
    if (raw_len < 128 && s->key_cache) {
        uint32_t hash = 2166136261u;
        for (size_t i = 0; i < raw_len; i++) {
            hash ^= (uint8_t)start_content[i];
            hash *= 16777619;
        }
        uint32_t slot = hash & s->key_cache_mask;
        entry = &s->key_cache[slot];
        
        if (entry->cached_str && entry->source_len == raw_len && memcmp(entry->source_str, start_content, raw_len) == 0) {
            *out_str = entry->cached_str;
            advance(s, (int)raw_len + 1);
            return true;
        }
    }

    if (!has_escapes) {
        char *str = arena_alloc_array(a, char, raw_len + 1);
        if (!str) return false; 
        memcpy(str, start_content, raw_len);
        str[raw_len] = '\0';
        *out_str = str;
        if (entry) {
            entry->source_str = start_content;
            entry->source_len = raw_len;
            entry->cached_str = str;
        }
        advance(s, (int)raw_len + 1); 
        return true;
    }

    char *str = arena_alloc_array(a, char, raw_len + 1);
    if (!str) return false; 
    char *out = str;
    const char *p = start_content;
    
    while (p < scan) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"':  *out++ = '"';  break;
                case '\\': *out++ = '\\'; break;
                case '/':  *out++ = '/';  break;
                case 'b':  *out++ = '\b'; break;
                case 'f':  *out++ = '\f'; break;
                case 'n':  *out++ = '\n'; break;
                case 'r':  *out++ = '\r'; break;
                case 't':  *out++ = '\t'; break;
                case 'u': {
                    unsigned int codepoint = 0;
                    for (int i = 0; i < 4; i++) {
                        p++;
                        if (p >= scan) { set_error(s, "Invalid unicode escape"); return false; }
                        char c = *p;
                        int val = 0;
                        if      (c >= '0' && c <= '9') val = c - '0';
                        else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
                        else { set_error(s, "Invalid unicode escape character"); return false; }
                        codepoint = (codepoint << 4) | val;
                    }
                    if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                        if (p + 6 < scan && p[1] == '\\' && p[2] == 'u') {
                            unsigned int low_surrogate = 0;
                            bool valid = true;
                            for (int i = 0; i < 4; i++) {
                                char c = p[3 + i];
                                int val = 0;
                                if      (c >= '0' && c <= '9') val = c - '0';
                                else if (c >= 'a' && c <= 'f') val = c - 'a' + 10;
                                else if (c >= 'A' && c <= 'F') val = c - 'A' + 10;
                                else { valid = false; break; }
                                low_surrogate = (low_surrogate << 4) | val;
                            }
                            if (valid && low_surrogate >= 0xDC00 && low_surrogate <= 0xDFFF) {
                                codepoint = 0x10000 + (((codepoint - 0xD800) << 10) | (low_surrogate - 0xDC00));
                                p += 6;
                            }
                        }
                    }
                    if (codepoint <= 0x7F) *out++ = (char)codepoint;
                    else if (codepoint <= 0x7FF) {
                        *out++ = (char)(0xC0 | (codepoint >> 6));
                        *out++ = (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint <= 0xFFFF) {
                        *out++ = (char)(0xE0 | (codepoint >> 12));
                        *out++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        *out++ = (char)(0x80 | (codepoint & 0x3F));
                    } else if (codepoint <= 0x10FFFF) {
                        *out++ = (char)(0xF0 | (codepoint >> 18));
                        *out++ = (char)(0x80 | ((codepoint >> 12) & 0x3F));
                        *out++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                        *out++ = (char)(0x80 | (codepoint & 0x3F));
                    }
                    break;
                }
                default: set_error(s, "Invalid escape sequence"); return false;
            }
        } else {
            *out++ = *p;
        }
        p++;
    }
    *out = '\0';
    *out_str = str;
    
    if (entry) {
        entry->source_str = start_content;
        entry->source_len = raw_len;
        entry->cached_str = str;
    }

    advance(s, (int)(scan - start_content) + 1); 
    return true;
}

static bool parse_number(ParseState *s, double *out_num) {
    if (parse_number_fast_path(s, out_num)) return true;

    const char *p = s->curr;
    int64_t mantissa = 0;
    int64_t exponent = 0;
    int sign = 1;

    if (p < s->end && *p == '-') { sign = -1; p++; }
    if (p >= s->end) { set_error(s, "Unexpected end"); return false; }

    if (*p == '0') {
        p++;
        if (p < s->end && (*p >= '0' && *p <= '9')) { set_error(s, "Leading zero"); return false; }
    } else if (*p >= '1' && *p <= '9') {
        while (p < s->end && (*p >= '0' && *p <= '9')) {
            if (mantissa < 9007199254740991LL) {
                mantissa = (mantissa * 10) + (uint64_t)((unsigned char)*p - '0');
            } else { exponent++; }
            p++;
        }
    } else { set_error(s, "Invalid number"); return false; }

    if (p < s->end && *p == '.') {
        p++;
        const char *frac_start = p;
        while (p < s->end && (*p >= '0' && *p <= '9')) {
            if (mantissa < 9007199254740991LL) {
                mantissa = (mantissa * 10) + (uint64_t)((unsigned char)*p - '0');
                exponent--;
            }
            p++;
        }
        if (p == frac_start) { set_error(s, "Expected digit after dot"); return false; }
    }

    if (p < s->end && (*p == 'e' || *p == 'E')) {
        p++;
        int exp_sign = 1;
        if (p < s->end && (*p == '+' || *p == '-')) {
            if (*p == '-') exp_sign = -1;
            p++;
        }
        int64_t e_val = 0;
        const char *exp_start = p;
        while (p < s->end && (*p >= '0' && *p <= '9')) {
            if (e_val < 1000000) {
                e_val = e_val * 10 + (*p - '0');
            }
            p++;
        }
        if (p == exp_start) { set_error(s, "Expected digit in exponent"); return false; }
        exponent += (e_val * exp_sign);
    }

    double val = (double)mantissa;
    if (exponent != 0) {
        if (exponent < 0 && -exponent <= 22) { val /= power_of_ten[-exponent]; } 
        else if (exponent > 0 && exponent <= 22) { val *= power_of_ten[exponent]; } 
        else { 
            val *= pow(10.0, exponent);
        }
    }

    *out_num = val * sign;
    s->curr = p; 
    return true;
}

static bool parse_array(Arena *main, ParseState *s, JsonValue *arr, int depth) {
    if (depth > MAX_JSON_DEPTH) { set_error(s, "Max JSON depth"); return false; }

    advance(s, 1); 
    s->last_comment = NULL;
    s->inline_comment = NULL;
    skip_whitespace(s);
    
    if (s->curr < s->end && *s->curr == ']') {
        advance(s, 1);
        arr->as.list.count = 0;
        arr->as.list.items = NULL;
        if (s->inline_comment || s->last_comment) {
            arr->post_comment = combine_comments(main, s->inline_comment, s->last_comment);
            s->last_comment = NULL;
            s->inline_comment = NULL;
        } else {
            arr->post_comment = NULL;
        }
        return true;
    }

    bool same_arena = (main == s->scratch);
    ArenaTemp temp_mem = {0};
    if (!same_arena) temp_mem = arena_temp_begin(s->scratch);
    
    JsonNode first_items[16];
    NodeBlock head;
    head.capacity = 16; // Start small for micro-payloads directly on the stack
    head.count = 0;
    head.next = NULL;
    head.items = first_items;
    
    NodeBlock *curr_block = &head;
    size_t total_count = 0;
    
    while (1) {
        if (curr_block->count == curr_block->capacity) {
            NodeBlock *next_block = arena_alloc_struct(s->scratch, NodeBlock);
            if (!next_block) return false;
            next_block->capacity = curr_block->capacity * 2;
            if (next_block->capacity > 32768) next_block->capacity = 32768;
            next_block->count = 0;
            next_block->next = NULL;
            next_block->items = arena_alloc_array(s->scratch, JsonNode, next_block->capacity);
            if (!next_block->items) return false;
            curr_block->next = next_block;
            curr_block = next_block;
        }

        JsonNode *node = &curr_block->items[curr_block->count];
        node->key = NULL;
        node->pre_comment = NULL;
        
        if (!parse_element(main, s, &node->value, depth + 1)) return false;
        
        curr_block->count++;
        total_count++;

        s->last_comment = NULL;
        s->inline_comment = NULL;
        skip_whitespace(s);
        if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }
        
        if (s->inline_comment) {
            size_t clen = strlen(s->inline_comment);
            node->value.trailing_comment = arena_alloc_array(main, char, clen + 1);
            strcpy(node->value.trailing_comment, s->inline_comment);
            s->inline_comment = NULL;
        } else {
            node->value.trailing_comment = NULL;
        }

        char c = *s->curr;
        if (c == ',') {
            advance(s, 1);
            if (s->flags & JSON_PARSE_ALLOW_COMMENTS) {
                skip_whitespace(s);
                if (s->inline_comment) {
                    if (node->value.trailing_comment) {
                        size_t ex = strlen(node->value.trailing_comment);
                        size_t add = strlen(s->inline_comment);
                        char *merged = arena_alloc_array(main, char, ex + add + 2);
                        strcpy(merged, node->value.trailing_comment);
                        merged[ex] = ' ';
                        strcpy(merged + ex + 1, s->inline_comment);
                        node->value.trailing_comment = merged;
                    } else {
                        size_t clen = strlen(s->inline_comment);
                        node->value.trailing_comment = arena_alloc_array(main, char, clen + 1);
                        strcpy(node->value.trailing_comment, s->inline_comment);
                    }
                    s->inline_comment = NULL;
                }
                
                if (s->curr < s->end && *s->curr == ']') {
                    advance(s, 1);
                    if (s->inline_comment || s->last_comment) {
                    arr->post_comment = combine_comments(main, s->inline_comment, s->last_comment);
                    s->last_comment = NULL;
                    s->inline_comment = NULL;
                } else {
                    arr->post_comment = NULL;
                }
                    break;
                }
            }
        } 
        else if (c == ']') { 
            advance(s, 1); 
            if (s->inline_comment || s->last_comment) {
            arr->post_comment = combine_comments(main, s->inline_comment, s->last_comment);
            s->last_comment = NULL;
            s->inline_comment = NULL;
        } else {
            arr->post_comment = NULL;
        }
            break; 
        } 
        else { set_error(s, "Expected ',' or ']'"); return false; }
    }

    arr->as.list.count = total_count;
    if (total_count > 0) {
        arr->as.list.items = arena_alloc_array(main, JsonNode, total_count);
        if (!arr->as.list.items) return false;
        
        size_t write_idx = 0;
        const NodeBlock *b = &head;
        while (b) {
            if (b->count > 0) {
                memcpy(&arr->as.list.items[write_idx], b->items, b->count * sizeof(JsonNode));
                write_idx += b->count;
            }
            b = b->next;
        }
    } else {
        arr->as.list.items = NULL;
    }
    
    if (!same_arena) arena_temp_end(temp_mem);
    return true;
}

static bool parse_object(Arena *main, ParseState *s, JsonValue *obj, int depth) {
    if (depth > MAX_JSON_DEPTH) { set_error(s, "Max JSON depth"); return false; }

    advance(s, 1); 
    s->last_comment = NULL;
    s->inline_comment = NULL;
    skip_whitespace(s);

    if (s->curr < s->end && *s->curr == '}') {
        advance(s, 1);
        obj->as.list.count = 0;
        obj->as.list.items = NULL;
        if (s->inline_comment || s->last_comment) {
            obj->post_comment = combine_comments(main, s->inline_comment, s->last_comment);
            s->last_comment = NULL;
            s->inline_comment = NULL;
        } else {
            obj->post_comment = NULL;
        }
        return true;
    }

    bool same_arena = (main == s->scratch);
    ArenaTemp temp_mem = {0};
    if (!same_arena) temp_mem = arena_temp_begin(s->scratch);
    
    JsonNode first_items[16];
    NodeBlock head;
    head.capacity = 16; // Start small for micro-payloads directly on the stack
    head.count = 0;
    head.next = NULL;
    head.items = first_items;
    
    NodeBlock *curr_block = &head;
    size_t total_count = 0;

    while (1) {
        skip_whitespace(s); 
        if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }
        if (*s->curr != '"') { set_error(s, "Expected key"); return false; }

        char *key_comment = NULL;
        if (s->inline_comment || s->last_comment) {
            key_comment = combine_comments(main, s->inline_comment, s->last_comment);
            s->last_comment = NULL;
            s->inline_comment = NULL;
        }

        if (curr_block->count == curr_block->capacity) {
            NodeBlock *next_block = arena_alloc_struct(s->scratch, NodeBlock);
            if (!next_block) return false;
            next_block->capacity = curr_block->capacity * 2;
            if (next_block->capacity > 32768) next_block->capacity = 32768;
            next_block->count = 0;
            next_block->next = NULL;
            next_block->items = arena_alloc_array(s->scratch, JsonNode, next_block->capacity);
            if (!next_block->items) return false;
            curr_block->next = next_block;
            curr_block = next_block;
        }

        JsonNode *node = &curr_block->items[curr_block->count];
        node->pre_comment = NULL;

        if (!parse_string(main, s, &node->key)) return false;

        skip_whitespace(s);
        if (s->curr >= s->end || *s->curr != ':') { set_error(s, "Expected ':'"); return false; }
        advance(s, 1); 

        node->pre_comment = key_comment;

        if (!parse_element(main, s, &node->value, depth + 1)) return false;
        
        curr_block->count++;
        total_count++;

        s->last_comment = NULL;
        s->inline_comment = NULL;
        skip_whitespace(s);
        if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }

        if (s->inline_comment) {
            size_t clen = strlen(s->inline_comment);
            node->value.trailing_comment = arena_alloc_array(main, char, clen + 1);
            strcpy(node->value.trailing_comment, s->inline_comment);
            s->inline_comment = NULL;
        } else {
            node->value.trailing_comment = NULL;
        }

        char c = *s->curr;
        if (c == ',') {
            advance(s, 1);
            if (s->flags & JSON_PARSE_ALLOW_COMMENTS) {
                skip_whitespace(s);
                if (s->inline_comment) {
                    if (node->value.trailing_comment) {
                        size_t ex = strlen(node->value.trailing_comment);
                        size_t add = strlen(s->inline_comment);
                        char *merged = arena_alloc_array(main, char, ex + add + 2);
                        strcpy(merged, node->value.trailing_comment);
                        merged[ex] = ' ';
                        strcpy(merged + ex + 1, s->inline_comment);
                        node->value.trailing_comment = merged;
                    } else {
                        size_t clen = strlen(s->inline_comment);
                        node->value.trailing_comment = arena_alloc_array(main, char, clen + 1);
                        strcpy(node->value.trailing_comment, s->inline_comment);
                    }
                    s->inline_comment = NULL;
                }

                if (s->curr < s->end && *s->curr == '}') {
                    advance(s, 1);
                if (s->inline_comment || s->last_comment) {
                    obj->post_comment = combine_comments(main, s->inline_comment, s->last_comment);
                    s->last_comment = NULL;
                    s->inline_comment = NULL;
                } else {
                    obj->post_comment = NULL;
                }
                    break;
                }
            }
        } 
        else if (c == '}') { 
            advance(s, 1); 
        if (s->inline_comment || s->last_comment) {
            obj->post_comment = combine_comments(main, s->inline_comment, s->last_comment);
            s->last_comment = NULL;
            s->inline_comment = NULL;
        } else {
            obj->post_comment = NULL;
        }
            break; 
        } 
        else { set_error(s, "Expected ',' or '}'"); return false; }
    }

    obj->as.list.count = total_count;
    if (total_count > 0) {
        obj->as.list.items = arena_alloc_array(main, JsonNode, total_count);
        if (!obj->as.list.items) return false;
        
        size_t write_idx = 0;
        const NodeBlock *b = &head;
        while (b) {
            if (b->count > 0) {
                memcpy(&obj->as.list.items[write_idx], b->items, b->count * sizeof(JsonNode));
                write_idx += b->count;
            }
            b = b->next;
        }
    } else {
        obj->as.list.items = NULL;
    }
    
    if (!same_arena) arena_temp_end(temp_mem);
    return true;
}

static inline bool parse_true(ParseState *s, JsonValue *v) {
    if (s->end - s->curr >= 4) {
        uint32_t val;
        memcpy(&val, s->curr, 4);
        if (val == 0x65757274) { // "true"
            v->type = JSON_BOOL; v->as.boolean = true; advance(s, 4); return true;
        }
    }
    set_error(s, "Expected 'true'"); return false;
}
static inline bool parse_false(ParseState *s, JsonValue *v) {
    if (s->end - s->curr >= 5) {
        uint32_t val;
        memcpy(&val, s->curr, 4);
        if (val == 0x736c6166 && s->curr[4] == 'e') { // "fals" + 'e'
            v->type = JSON_BOOL; v->as.boolean = false; advance(s, 5); return true;
        }
    }
    set_error(s, "Expected 'false'"); return false;
}
static inline bool parse_null(ParseState *s, JsonValue *v) {
    if (s->end - s->curr >= 4) {
        uint32_t val;
        memcpy(&val, s->curr, 4);
        if (val == 0x6c6c756e) { // "null"
            v->type = JSON_NULL; advance(s, 4); return true;
        }
    }
    set_error(s, "Expected 'null'"); return false;
}

static bool parse_element(Arena *main, ParseState *s, JsonValue *out_val, int depth) {
    skip_whitespace(s);
    if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }
    out_val->offset = s->curr - s->start;

    if (s->inline_comment || s->last_comment) {
        out_val->pre_comment = combine_comments(main, s->inline_comment, s->last_comment);
        s->last_comment = NULL;
        s->inline_comment = NULL;
    } else {
        out_val->pre_comment = NULL;
    }
    out_val->post_comment = NULL;
    out_val->trailing_comment = NULL;

    unsigned char c = (unsigned char)*s->curr;
    switch (c) {
        case '"': 
            out_val->type = JSON_STRING;
            return parse_string(main, s, &out_val->as.string);
        case '[': 
            out_val->type = JSON_ARRAY;
            return parse_array(main, s, out_val, depth);
        case '{': 
            out_val->type = JSON_OBJECT;
            return parse_object(main, s, out_val, depth);
        case '-': case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            out_val->type = JSON_NUMBER;
            return parse_number(s, &out_val->as.number);
        case 't': return parse_true(s, out_val);
        case 'f': return parse_false(s, out_val);
        case 'n': return parse_null(s, out_val);
        default:
            set_error(s, "Unexpected character '%c'", c);
            return false;
    }
}

JsonValue *json_parse(Arena *main, Arena *scratch, const char *input, size_t len, int flags, JsonError *err) {
    if (!main || !scratch || !input || len == 0) return NULL;  
    if (err) {
        err->msg[0] = '\0';
        err->line = 0;
        err->col = 0;
        err->offset = 0;
    }

    ParseState s = {0};
    s.start = input; s.curr = input; s.end = input + len;
    s.err = err; s.scratch = scratch;
    s.flags = flags;
    s.last_comment = NULL;
    s.inline_comment = NULL;
    
    // Dynamically scale the key cache based on payload size
    if (len > 4096) {
        uint32_t cap = 256;
        while (cap < len / 4 && cap < KEY_CACHE_CAPACITY) cap *= 2;
        s.key_cache = (KeyCacheEntry*)arena_zalloc(scratch, sizeof(KeyCacheEntry) * cap);
        s.key_cache_mask = cap - 1;
    } else {
        s.key_cache = NULL;
    }

    JsonValue *root = arena_alloc_struct(main, JsonValue);
    if (!root) return NULL;

    if (!parse_element(main, &s, root, 0)) return NULL;
    
    s.last_comment = NULL;
    s.inline_comment = NULL;
    skip_whitespace(&s);
    
    if (s.inline_comment || s.last_comment) {
        char *trail = combine_comments(main, s.inline_comment, s.last_comment);
        if (trail) {
            root->trailing_comment = trail;
        }
    }

    if (s.curr != s.end) {
        if (err) set_error(&s, "Garbage after JSON");
        return NULL;
    }
    return root;
}

/* --- Helpers and Builders --- */

JsonValue *json_object_get(JsonValue *obj, const char *key) {
    if (!obj || !key || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) return &obj->as.list.items[i].value;
    }
    return NULL;
}

/* --- New QoL Implementations --- */

double json_object_get_number(JsonValue *obj, const char *key, double fallback) {
    JsonValue *v = json_object_get(obj, key);
    return (v && v->type == JSON_NUMBER) ? v->as.number : fallback;
}

const char* json_object_get_string(JsonValue *obj, const char *key, const char *fallback) {
    JsonValue *v = json_object_get(obj, key);
    return (v && v->type == JSON_STRING) ? v->as.string : fallback;
}

bool json_object_get_bool(JsonValue *obj, const char *key, bool fallback) {
    JsonValue *v = json_object_get(obj, key);
    return (v && v->type == JSON_BOOL) ? v->as.boolean : fallback;
}

static bool arena_json_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

JsonValue *json_object_get_case_insensitive(JsonValue *obj, const char *key) {
    if (!obj || !key || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (arena_json_strcasecmp(obj->as.list.items[i].key, key)) return &obj->as.list.items[i].value;
    }
    return NULL;
}

void json_object_remove(JsonValue *obj, const char *key) {
    if (!obj || !key || obj->type != JSON_OBJECT) return;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) {
            if (i < obj->as.list.count - 1) {
                memmove(&obj->as.list.items[i], &obj->as.list.items[i + 1], 
                        (obj->as.list.count - i - 1) * sizeof(JsonNode));
            }
            obj->as.list.count--;
            return;
        }
    }
}

void json_object_replace(Arena *a, JsonValue *obj, const char *key, const JsonValue *val) {
    if (!obj || !key || !val || obj->type != JSON_OBJECT) return;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) {
            obj->as.list.items[i].value = *val;
            return;
        }
    }
    json_object_add(a, obj, key, val);
}

JsonValue *json_object_detach(Arena *a, JsonValue *obj, const char *key) {
    if (!obj || !key || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) {
            JsonValue *detached = arena_alloc_struct(a, JsonValue);
            if (!detached) return NULL;
            *detached = obj->as.list.items[i].value;
            
            if (i < obj->as.list.count - 1) {
                memmove(&obj->as.list.items[i], &obj->as.list.items[i + 1], 
                        (obj->as.list.count - i - 1) * sizeof(JsonNode));
            }
            obj->as.list.count--;
            return detached;
        }
    }
    return NULL;
}

JsonValue *json_clone(Arena *a, const JsonValue *v) {
    if (!a || !v) return NULL;
    JsonValue *copy = arena_alloc_struct(a, JsonValue);
    if (!copy) return NULL;
    *copy = *v;
    
    if (v->pre_comment) {
        size_t l = strlen(v->pre_comment);
        copy->pre_comment = arena_alloc_array(a, char, l + 1);
        if (copy->pre_comment) strcpy(copy->pre_comment, v->pre_comment);
    }
    if (v->post_comment) {
        size_t l = strlen(v->post_comment);
        copy->post_comment = arena_alloc_array(a, char, l + 1);
        if (copy->post_comment) strcpy(copy->post_comment, v->post_comment);
    }
    if (v->trailing_comment) {
        size_t l = strlen(v->trailing_comment);
        copy->trailing_comment = arena_alloc_array(a, char, l + 1);
        if (copy->trailing_comment) strcpy(copy->trailing_comment, v->trailing_comment);
    }
    
    if (v->type == JSON_STRING && v->as.string) {
        size_t l = strlen(v->as.string);
        copy->as.string = arena_alloc_array(a, char, l + 1);
        if (copy->as.string) strcpy(copy->as.string, v->as.string);
    } else if (v->type == JSON_ARRAY || v->type == JSON_OBJECT) {
        if (v->as.list.count > 0 && v->as.list.items) {
            copy->as.list.items = arena_alloc_array(a, JsonNode, v->as.list.count);
            if (!copy->as.list.items) return copy;
            for (size_t i = 0; i < v->as.list.count; i++) {
                JsonNode *dn = &copy->as.list.items[i];
                JsonNode *sn = &v->as.list.items[i];
                dn->key = sn->key ? strcpy(arena_alloc_array(a, char, strlen(sn->key) + 1), sn->key) : NULL;
                dn->pre_comment = sn->pre_comment ? strcpy(arena_alloc_array(a, char, strlen(sn->pre_comment) + 1), sn->pre_comment) : NULL;
                JsonValue *cv = json_clone(a, &sn->value);
                if (cv) dn->value = *cv;
                else memset(&dn->value, 0, sizeof(JsonValue));
            }
        }
    }
    return copy;
}

/* --- Serialization Builders --- */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} StrBuilder;

static void sb_init(StrBuilder *sb) {
    sb->cap = 8192; 
    sb->len = 0;
    sb->buf = (char *)malloc(sb->cap);
}

static void sb_append(StrBuilder *sb, const char *str, size_t len) {
    if (len > SIZE_MAX - sb->len) return; // Check for total length overflow
    size_t new_len = sb->len + len;
    if (new_len > sb->cap) {
        size_t new_cap = sb->cap;
        if (new_cap == 0) new_cap = 8192;
        while (new_len > new_cap) {
            if (new_cap > SIZE_MAX / 2) {
                new_cap = SIZE_MAX;
                break; // Can't grow further
            }
            new_cap *= 2;
        }
        if (new_len > new_cap) return; // Required size is impossible

        char* new_buf = (char *)realloc(sb->buf, new_cap);
        if (!new_buf) return; // realloc failed
        sb->buf = new_buf;
        sb->cap = new_cap;
    }
    memcpy(sb->buf + sb->len, str, len);
    sb->len = new_len;
}

static void sb_putc(StrBuilder *sb, char c) {
    if (sb->len + 1 > sb->cap) {
        sb->cap *= 2;
        sb->buf = (char *)realloc(sb->buf, sb->cap);
    }
    sb->buf[sb->len++] = c;
}

static void w_escaped_string(StrBuilder *sb, const char *s) {
    sb_putc(sb, '"');
    while (*s) {
        const char *p = s;
        while (*p) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\' || c < 0x20) break;
            p++;
        }
        if (p > s) {
            sb_append(sb, s, p - s);
            s = p;
        }
        if (*s == '\0') break;

        unsigned char c = (unsigned char)*s;
        if (c == '"') sb_append(sb, "\\\"", 2);
        else if (c == '\\') sb_append(sb, "\\\\", 2);
        else if (c == '\b') sb_append(sb, "\\b", 2);
        else if (c == '\f') sb_append(sb, "\\f", 2);
        else if (c == '\n') sb_append(sb, "\\n", 2);
        else if (c == '\r') sb_append(sb, "\\r", 2);
        else if (c == '\t') sb_append(sb, "\\t", 2);
        else {
            char hex[7];
            sprintf(hex, "\\u00%02X", c);
            sb_append(sb, hex, 6);
        }
        s++;
    }
    sb_putc(sb, '"');
}

static bool comment_requires_newline(const char *comment) {
    if (!comment) return false;
    bool in_line = false;
    bool in_block = false;
    for (const char *p = comment; *p; p++) {
        if (!in_line && !in_block) {
            if (p[0] == '/' && p[1] == '/') {
                in_line = true;
                p++;
            } else if (p[0] == '/' && p[1] == '*') {
                in_block = true;
                p++;
            }
        } else if (in_line) {
            if (*p == '\n') in_line = false;
        } else if (in_block) {
            if (p[0] == '*' && p[1] == '/') {
                in_block = false;
                p++;
            }
        }
    }
    return in_line;
}

static void json_write_internal(JsonValue *v, StrBuilder *sb, int depth, bool pretty, bool use_tabs, int indent_step, bool keep_comments) {
    if (!v) return;

    if (keep_comments && v->pre_comment) {
        sb_append(sb, v->pre_comment, strlen(v->pre_comment));
        if (pretty) {
            sb_putc(sb, '\n');
            if (depth > 0) {
                for (int j = 0; j < depth; j++) {
                    if (use_tabs) sb_putc(sb, '\t');
                    else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                }
            }
        } else if (comment_requires_newline(v->pre_comment)) {
            sb_putc(sb, '\n');
        }
    }

    switch (v->type) {
        case JSON_NULL: sb_append(sb, "null", 4); break;
        case JSON_BOOL: 
            if (v->as.boolean) sb_append(sb, "true", 4);
            else sb_append(sb, "false", 5);
            break;
        case JSON_NUMBER: {
            if (!isfinite(v->as.number)) { sb_append(sb, "null", 4); } 
            else {
                double n = v->as.number;
                if (n >= -9007199254740991.0 && n <= 9007199254740991.0 && n == (int64_t)n) { 
                    char num_buf[32];
                    int64_t val = (int64_t)n;
                    char *p = num_buf + 31;
                    *p = '\0';
                    uint64_t uval = val < 0 ? -val : val;
                    do {
                        *--p = '0' + (uval % 10);
                        uval /= 10;
                    } while (uval > 0);
                    if (val < 0) *--p = '-';
                    sb_append(sb, p, (num_buf + 31) - p);
                } else {
                    char num_buf[64];
                    int len = snprintf(num_buf, sizeof(num_buf), "%.15g", n); // 15 digits is standard JSON precision (speeds up parsing slightly)
                    sb_append(sb, num_buf, len);
                }
            }
            break;
        }
        case JSON_STRING: w_escaped_string(sb, v->as.string); break;
        case JSON_ARRAY: {
            sb_putc(sb, '[');
            if (v->as.list.count > 0 || (keep_comments && v->post_comment)) {
                for (size_t i = 0; i < v->as.list.count; i++) {
                    JsonValue *child = &v->as.list.items[i].value;
                    
                    
                    if (pretty) {
                        sb_putc(sb, '\n');
                        for (int j = 0; j < depth + 1; j++) {
                            if (use_tabs) sb_putc(sb, '\t');
                            else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                        }
                    }
                    
                    json_write_internal(child, sb, depth + 1, pretty, use_tabs, indent_step, keep_comments);
                    
                    if (i + 1 < v->as.list.count) {
                        sb_putc(sb, ',');
                    }
                    
                    if (keep_comments && child->trailing_comment) {
                        if (pretty) sb_putc(sb, ' ');
                        sb_append(sb, child->trailing_comment, strlen(child->trailing_comment));
                        if (!pretty && comment_requires_newline(child->trailing_comment)) {
                            sb_putc(sb, '\n');
                        }
                    }
                }
                if (keep_comments && v->post_comment) {
                    if (pretty) {
                        sb_putc(sb, '\n');
                        for (int j = 0; j < depth + 1; j++) {
                            if (use_tabs) sb_putc(sb, '\t');
                            else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                        }
                    }
                    sb_append(sb, v->post_comment, strlen(v->post_comment));
                    if (!pretty && comment_requires_newline(v->post_comment)) {
                        sb_putc(sb, '\n');
                    }
                }
                if (pretty) {
                    sb_putc(sb, '\n');
                    for (int j = 0; j < depth; j++) {
                        if (use_tabs) sb_putc(sb, '\t');
                        else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                    }
                }
            }
            sb_putc(sb, ']');
            break;
        }
        case JSON_OBJECT: {
            sb_putc(sb, '{');
            if (v->as.list.count > 0 || (keep_comments && v->post_comment)) {
                for (size_t i = 0; i < v->as.list.count; i++) {
                    JsonNode *node = &v->as.list.items[i];
                    JsonValue *child = &node->value;
                    
                    if (keep_comments && node->pre_comment) {
                        if (pretty) {
                            sb_putc(sb, '\n');
                            for (int j = 0; j < depth + 1; j++) {
                                if (use_tabs) sb_putc(sb, '\t');
                                else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                            }
                        }
                        sb_append(sb, node->pre_comment, strlen(node->pre_comment));
                        if (!pretty && comment_requires_newline(node->pre_comment)) {
                            sb_putc(sb, '\n');
                        }
                    }
                    
                    if (pretty) {
                        sb_putc(sb, '\n');
                        for (int j = 0; j < depth + 1; j++) {
                            if (use_tabs) sb_putc(sb, '\t');
                            else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                        }
                    }
                    
                    w_escaped_string(sb, node->key);
                    sb_append(sb, pretty ? ": " : ":", pretty ? 2 : 1);
                    
                    json_write_internal(child, sb, depth + 1, pretty, use_tabs, indent_step, keep_comments);
                    
                    if (i + 1 < v->as.list.count) {
                        sb_putc(sb, ',');
                    }

                    if (keep_comments && child->trailing_comment) {
                        if (pretty) sb_putc(sb, ' ');
                        sb_append(sb, child->trailing_comment, strlen(child->trailing_comment));
                        if (!pretty && comment_requires_newline(child->trailing_comment)) {
                            sb_putc(sb, '\n');
                        }
                    }
                }
                if (keep_comments && v->post_comment) {
                    if (pretty) {
                        sb_putc(sb, '\n');
                        for (int j = 0; j < depth + 1; j++) {
                            if (use_tabs) sb_putc(sb, '\t');
                            else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                        }
                    }
                    sb_append(sb, v->post_comment, strlen(v->post_comment));
                    if (!pretty && comment_requires_newline(v->post_comment)) {
                        sb_putc(sb, '\n');
                    }
                }
                if (pretty) {
                    sb_putc(sb, '\n');
                    for (int j = 0; j < depth; j++) {
                        if (use_tabs) sb_putc(sb, '\t');
                        else for (int k = 0; k < indent_step; k++) sb_putc(sb, ' ');
                    }
                }
            }
            sb_putc(sb, '}');
            break;
        }
    }

    if (keep_comments && depth == 0 && v->trailing_comment) {
        if (pretty) sb_putc(sb, ' ');
        sb_append(sb, v->trailing_comment, strlen(v->trailing_comment));
        if (!pretty && comment_requires_newline(v->trailing_comment)) {
            sb_putc(sb, '\n');
        }
    }
}

char *json_serialize(Arena *a, JsonValue *v, bool pretty, bool use_tabs, int indent_step, bool keep_comments) {
    if (!a || !v) return NULL;
    StrBuilder sb;
    sb_init(&sb);
    json_write_internal(v, &sb, 0, pretty, use_tabs, indent_step, keep_comments);
    char *result = arena_alloc_array(a, char, sb.len + 1);
    if (result && sb.buf) {
        memcpy(result, sb.buf, sb.len);
        result[sb.len] = '\0';
    }
    if (sb.buf) free(sb.buf);
    return result;
}

JsonValue *json_create_null(Arena *a) { return make_value(a, JSON_NULL); }
JsonValue *json_create_bool(Arena *a, bool b) { 
    JsonValue *v = make_value(a, JSON_BOOL); 
    if (v) v->as.boolean = b; 
    return v;
}
JsonValue *json_create_number(Arena *a, double num) {
    JsonValue *v = make_value(a, JSON_NUMBER); 
    if (v) v->as.number = num; 
    return v;
}
JsonValue *json_create_string(Arena *a, const char *str) {
    if (!a || !str) return NULL;
    JsonValue *v = make_value(a, JSON_STRING);
    if (!v) return NULL;
    size_t len = strlen(str);
    v->as.string = arena_alloc_array(a, char, len + 1);
    if (v->as.string) memcpy(v->as.string, str, len + 1);
    return v;
}
JsonValue *json_create_array(Arena *a) { return make_value(a, JSON_ARRAY); }
JsonValue *json_create_object(Arena *a) { return make_value(a, JSON_OBJECT); }

static void json_list_append(Arena *a, JsonValue *parent, const char *key, const JsonValue *val) {
    if (!a || !parent || !val) return; 

    size_t old_count = parent->as.list.count;
    JsonNode *new_items = arena_alloc_array(a, JsonNode, old_count + 1);
    if (!new_items) return;

    if (old_count > 0) {
        memcpy(new_items, parent->as.list.items, old_count * sizeof(JsonNode));
    }

    if (key) {
        size_t len = strlen(key);
        new_items[old_count].key = arena_alloc_array(a, char, len + 1);
        if (new_items[old_count].key) memcpy(new_items[old_count].key, key, len + 1);
    } else {
        new_items[old_count].key = NULL;
    }
    
    new_items[old_count].pre_comment = NULL;
    new_items[old_count].value = *val;
    parent->as.list.items = new_items;
    parent->as.list.count = old_count + 1;
}

void json_object_add(Arena *a, JsonValue *obj, const char *key, const JsonValue *val) {
    if (obj && key && val && obj->type == JSON_OBJECT) json_list_append(a, obj, key, val);
}
void json_object_add_string(Arena *a, JsonValue *obj, const char *key, const char *val) {
    if (val) {
        JsonValue str_val; str_val.type = JSON_STRING;
        str_val.pre_comment = NULL; str_val.post_comment = NULL; str_val.offset = 0;
        str_val.trailing_comment = NULL;
        size_t len = strlen(val);
        str_val.as.string = arena_alloc_array(a, char, len + 1);
        if (str_val.as.string) memcpy(str_val.as.string, val, len + 1);
        json_list_append(a, obj, key, &str_val);
    }
}
void json_object_add_number(Arena *a, JsonValue *obj, const char *key, double val) {
    JsonValue num_val; num_val.type = JSON_NUMBER; num_val.as.number = val;
    num_val.pre_comment = NULL; num_val.post_comment = NULL; num_val.offset = 0;
    num_val.trailing_comment = NULL;
    json_list_append(a, obj, key, &num_val);
}
void json_object_add_bool(Arena *a, JsonValue *obj, const char *key, bool val) {
    JsonValue bool_val; bool_val.type = JSON_BOOL; bool_val.as.boolean = val;
    bool_val.pre_comment = NULL; bool_val.post_comment = NULL; bool_val.offset = 0;
    bool_val.trailing_comment = NULL;
    json_list_append(a, obj, key, &bool_val);
}

void json_array_append(Arena *a, JsonValue *arr, const JsonValue *val) {
    if (arr && val && arr->type == JSON_ARRAY) json_list_append(a, arr, NULL, val);
}
void json_array_append_string(Arena *a, JsonValue *arr, const char *val) {
    if (val) {
        JsonValue str_val; str_val.type = JSON_STRING;
        str_val.pre_comment = NULL; str_val.post_comment = NULL; str_val.offset = 0;
        str_val.trailing_comment = NULL;
        size_t len = strlen(val);
        str_val.as.string = arena_alloc_array(a, char, len + 1);
        if (str_val.as.string) memcpy(str_val.as.string, val, len + 1);
        json_list_append(a, arr, NULL, &str_val);
    }
}

void json_array_append_number(Arena *a, JsonValue *arr, double val) {
    JsonValue num_val; num_val.type = JSON_NUMBER; num_val.as.number = val;
    num_val.pre_comment = NULL; num_val.post_comment = NULL; num_val.offset = 0;
    num_val.trailing_comment = NULL;
    json_list_append(a, arr, NULL, &num_val);
}

#endif /* ARENA_JSON_IMPLEMENTATION */
