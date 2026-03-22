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

/* ============================================================================
 * ARENA ALLOCATOR
 * ============================================================================ */

typedef struct ArenaRegion ArenaRegion;

struct ArenaRegion {
    ArenaRegion *next;
    size_t capacity;
    size_t count;
    size_t _padding; 
    uint8_t data[];
};

typedef struct Arena {
    ArenaRegion *begin;
    ArenaRegion *end;
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
void *arena_alloc_zero(Arena *a, size_t size);
void arena_reset(Arena *a);
void arena_free(Arena *a);
void arena_print_stats(const Arena *a);

static inline void *arena_alloc(Arena *a, size_t size) {
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
    JsonType type;
    size_t offset;
    union {
        bool boolean;
        double number;
        char *string;
        struct { 
            JsonNode *items; 
            size_t count;
        } list; 
    } as;
} JsonValue;

struct JsonNode {
    char *key;       
    JsonValue value; 
};

/* --- Core API --- */
#define JSON_PARSE_STRICT 0
#define JSON_PARSE_ALLOW_COMMENTS 1

JsonValue *json_parse(Arena *main, Arena *scratch, const char *input, size_t len, int flags, JsonError *err);
JsonValue *json_get(JsonValue *obj, const char *key);
JsonValue *json_at(JsonValue *arr, int index);
void json_print(JsonValue *v, int indent);
char *json_to_string(Arena *a, JsonValue *v, bool pretty);

/* --- Iteration Macros --- */
#define json_array_foreach(node, arr) \
    for (size_t i = 0; (arr) && i < (arr)->as.list.count && (node = &(arr)->as.list.items[i]); i++)

#define json_object_foreach(node, obj) \
    for (size_t i = 0; (obj) && i < (obj)->as.list.count && (node = &(obj)->as.list.items[i]); i++)

/* --- New QoL & Mutation API --- */
bool        json_is_type(JsonValue *v, JsonType type);
double      json_get_number(JsonValue *obj, const char *key, double fallback);
const char* json_get_string(JsonValue *obj, const char *key, const char *fallback);
bool        json_get_bool(JsonValue *obj, const char *key, bool fallback);
JsonValue* json_get_case(JsonValue *obj, const char *key);
void        json_remove_from_object(JsonValue *obj, const char *key);
void        json_remove_from_array(JsonValue *arr, size_t index);
JsonValue* json_clone(Arena *dest_arena, JsonValue *src);
void       json_replace_in_object(Arena *a, JsonValue *obj, const char *key, JsonValue *new_val);
JsonValue* json_detach_from_object(Arena *a, JsonValue *obj, const char *key);

/* --- Builder API --- */
JsonValue *json_create_null(Arena *a);
JsonValue *json_create_bool(Arena *a, bool b);
JsonValue *json_create_number(Arena *a, double num);
JsonValue *json_create_string(Arena *a, const char *str);
JsonValue *json_create_array(Arena *a);
JsonValue *json_create_object(Arena *a);

void json_add(Arena *a, JsonValue *obj, const char *key, JsonValue *val);
void json_add_string(Arena *a, JsonValue *obj, const char *key, const char *val);
void json_add_number(Arena *a, JsonValue *obj, const char *key, double val);
void json_add_bool(Arena *a, JsonValue *obj, const char *key, bool val);
void json_add_null(Arena *a, JsonValue *obj, const char *key);

void json_append(Arena *a, JsonValue *arr, JsonValue *val);
void json_append_string(Arena *a, JsonValue *arr, const char *val);
void json_append_number(Arena *a, JsonValue *arr, double val);
void json_append_bool(Arena *a, JsonValue *arr, bool val);
void json_append_null(Arena *a, JsonValue *arr);

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
#include <assert.h>
#include <strings.h> /* for strcasecmp */

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#include <tmmintrin.h>
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
    a->begin = NULL;
    a->end = NULL;
}

void *arena_alloc_fallback(Arena *a, size_t size) {
    size_t aligned_size = (size + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1);

    if (a->end == NULL) {
        if (a->begin != NULL) {
            a->end = a->begin;
            a->end->count = 0;
        } else {
            size_t cap = ARENA_DEFAULT_BLOCK_SIZE;
            if (aligned_size > cap) cap = aligned_size;
            a->begin = arena__new_region(cap);
            if (!a->begin) return NULL;
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
    if (aligned_size > new_cap) new_cap = aligned_size;
    if (new_cap < ARENA_DEFAULT_BLOCK_SIZE) new_cap = ARENA_DEFAULT_BLOCK_SIZE;

    ArenaRegion *next = arena__new_region(new_cap);
    if (!next) return NULL;

    a->end->next = next;
    a->end = next;
    a->end->count = aligned_size;
    return (void *)a->end->data;
}

void *arena_alloc_zero(Arena *a, size_t size) {
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
    while (curr) {
        ArenaRegion *next = curr->next;
        free(curr);
        curr = next;
    }
    a->begin = NULL;
    a->end = NULL;
}

void arena_print_stats(const Arena *a) {
    size_t total_cap = 0, used = 0, count = 0;
    ArenaRegion *curr = a->begin;
    while (curr) {
        total_cap += curr->capacity;
        used += curr->count;
        count++;
        curr = curr->next;
    }
    printf("Arena: %zu regions, %zu/%zu bytes used\n", count, used, total_cap);
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
#define BLOCK_CAPACITY 128

typedef struct {
    const char *start;
    const char *curr;
    const char *end;
    JsonError *err;
    Arena *scratch; 
    int flags;
} ParseState;

typedef struct NodeBlock NodeBlock;
struct NodeBlock {
    JsonNode items[BLOCK_CAPACITY];
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

static const unsigned char ws_lut[256] = { [' '] = 1, ['\n'] = 1, ['\r'] = 1, ['\t'] = 1 };

static void skip_whitespace(ParseState *s) {
    while (1) {
        const char *p = s->curr;
        const char *end = s->end;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
        static const char space_chars[16] __attribute__((aligned(16))) = { ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ' };
        static const char lf_chars[16]    __attribute__((aligned(16))) = { '\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n','\n' };
        static const char cr_chars[16]    __attribute__((aligned(16))) = { '\r','\r','\r','\r','\r','\r','\r','\r','\r','\r','\r','\r','\r','\r','\r','\r' };
        static const char tab_chars[16]   __attribute__((aligned(16))) = { '\t','\t','\t','\t','\t','\t','\t','\t','\t','\t','\t','\t','\t','\t','\t','\t' };

        const __m128i v_sp  = _mm_load_si128((const __m128i*)space_chars);
        const __m128i v_lf  = _mm_load_si128((const __m128i*)lf_chars);
        const __m128i v_cr  = _mm_load_si128((const __m128i*)cr_chars);
        const __m128i v_tab = _mm_load_si128((const __m128i*)tab_chars);

        while (p + 16 <= end) {
            __m128i data = _mm_loadu_si128((const __m128i *)p);
            __m128i is_ws = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(data, v_sp), _mm_cmpeq_epi8(data, v_lf)), 
                                         _mm_or_si128(_mm_cmpeq_epi8(data, v_cr), _mm_cmpeq_epi8(data, v_tab)));
            uint32_t mask = (uint32_t)_mm_movemask_epi8(is_ws);
            if (mask != 0xFFFF) {
                p += __builtin_ctz(~mask);
                break;
            }
            p += 16;
        }
#endif
        while (p < end && ws_lut[(unsigned char)*p]) p++;
        s->curr = p;

        if (s->flags & JSON_PARSE_ALLOW_COMMENTS) {
            if (p + 1 < end && p[0] == '/') {
                if (p[1] == '/') {
                    p += 2;
                    while (p < end && *p != '\n') p++;
                    s->curr = p;
                    continue;
                } else if (p[1] == '*') {
                    p += 2;
                    while (p + 1 < end && !(p[0] == '*' && p[1] == '/')) p++;
                    if (p + 1 < end) p += 2;
                    s->curr = p;
                    continue;
                }
            }
        }
        break;
    }
}

static JsonValue *make_value(Arena *a, JsonType type) {
    JsonValue *v = arena_alloc_struct(a, JsonValue);
    if (v) {
        v->type = type;
        v->offset = 0;
        if (type == JSON_ARRAY || type == JSON_OBJECT) {
            v->as.list.items = NULL;
            v->as.list.count = 0;
        }
    }
    return v;
}

static bool parse_element(Arena *main, ParseState *s, JsonValue *out_val, int depth);

static bool parse_string(Arena *a, ParseState *s, char **out_str) {
    advance(s, 1); 
    const char *start_content = s->curr;
    const char *scan = s->curr;
    bool has_escapes = false;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
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
#endif
    while (scan < s->end) {
        unsigned char c = (unsigned char)*scan;
        if (c == '"') break;
        else if (c == '\\') {
            has_escapes = true;
            scan++; 
            if (scan >= s->end) { set_error(s, "Unterminated escape"); return false; }
        } else if (c < 0x20) {
            set_error(s, "Control character in string"); return false;
        }
        scan++;
    }
    if (scan >= s->end) { set_error(s, "Unterminated string"); return false; }

    size_t raw_len = scan - start_content;
    if (!has_escapes) {
        char *str = arena_alloc_array(a, char, raw_len + 1);
        if (!str) return false; 
        memcpy(str, start_content, raw_len);
        str[raw_len] = '\0';
        *out_str = str;
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
    advance(s, (int)(scan - start_content) + 1); 
    return true;
}

static bool parse_number(ParseState *s, double *out_num) {
    const char *p = s->curr;
    int64_t mantissa = 0;
    int exponent = 0;
    int sign = 1;

    if (p < s->end && *p == '-') { sign = -1; p++; }
    if (p >= s->end) return false;

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
        int32_t e_val = 0;
        const char *exp_start = p;
        while (p < s->end && (*p >= '0' && *p <= '9')) {
            e_val = e_val * 10 + (*p - '0');
            p++;
        }
        if (p == exp_start) return false;
        exponent += (e_val * exp_sign);
    }

    double val = (double)mantissa;
    if (exponent != 0) {
        static const double pow10[] = {
            1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10,
            1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
        };
        if (exponent < 0 && -exponent <= 22) { val /= pow10[-exponent]; } 
        else if (exponent > 0 && exponent <= 22) { val *= pow10[exponent]; } 
        else { val *= pow(10.0, (double)exponent); }
    }

    *out_num = val * sign;
    s->curr = p; 
    return true;
}

static bool parse_array(Arena *main, ParseState *s, JsonValue *arr, int depth) {
    if (depth > MAX_JSON_DEPTH) { set_error(s, "Max JSON depth"); return false; }

    advance(s, 1); 
    skip_whitespace(s);
    
    if (s->curr < s->end && *s->curr == ']') {
        advance(s, 1);
        arr->as.list.count = 0;
        arr->as.list.items = NULL;
        return true;
    }

    ArenaTemp temp_mem = arena_temp_begin(s->scratch);
    
    NodeBlock *head = arena_alloc_struct(s->scratch, NodeBlock);
    if (!head) return false;
    head->count = 0;
    head->next = NULL;
    
    NodeBlock *curr_block = head;
    size_t total_count = 0;
    
    while (1) {
        if (curr_block->count == BLOCK_CAPACITY) {
            NodeBlock *next_block = arena_alloc_struct(s->scratch, NodeBlock);
            if (!next_block) return false;
            next_block->count = 0;
            next_block->next = NULL;
            curr_block->next = next_block;
            curr_block = next_block;
        }

        JsonNode *node = &curr_block->items[curr_block->count];
        node->key = NULL;
        
        if (!parse_element(main, s, &node->value, depth + 1)) return false;
        
        curr_block->count++;
        total_count++;

        skip_whitespace(s);
        if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }
        
        char c = *s->curr;
        if (c == ',') {
            advance(s, 1);
            if (s->flags & JSON_PARSE_ALLOW_COMMENTS) {
                skip_whitespace(s);
                if (s->curr < s->end && *s->curr == ']') {
                    advance(s, 1);
                    break;
                }
            }
        } 
        else if (c == ']') { advance(s, 1); break; } 
        else { set_error(s, "Expected ',' or ']'"); return false; }
    }

    arr->as.list.count = total_count;
    arr->as.list.items = arena_alloc_array(main, JsonNode, total_count);
    if (!arr->as.list.items) return false;
    
    size_t write_idx = 0;
    NodeBlock *b = head;
    while (b) {
        memcpy(&arr->as.list.items[write_idx], b->items, b->count * sizeof(JsonNode));
        write_idx += b->count;
        b = b->next;
    }
    
    arena_temp_end(temp_mem);
    return true;
}

static bool parse_object(Arena *main, ParseState *s, JsonValue *obj, int depth) {
    if (depth > MAX_JSON_DEPTH) { set_error(s, "Max JSON depth"); return false; }

    advance(s, 1); 
    skip_whitespace(s);

    if (s->curr < s->end && *s->curr == '}') {
        advance(s, 1);
        obj->as.list.count = 0;
        obj->as.list.items = NULL;
        return true;
    }

    ArenaTemp temp_mem = arena_temp_begin(s->scratch);
    
    NodeBlock *head = arena_alloc_struct(s->scratch, NodeBlock);
    if (!head) return false;
    head->count = 0;
    head->next = NULL;
    
    NodeBlock *curr_block = head;
    size_t total_count = 0;

    while (1) {
        skip_whitespace(s); 
        if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }
        if (*s->curr != '"') { set_error(s, "Expected key"); return false; }

        if (curr_block->count == BLOCK_CAPACITY) {
            NodeBlock *next_block = arena_alloc_struct(s->scratch, NodeBlock);
            if (!next_block) return false;
            next_block->count = 0;
            next_block->next = NULL;
            curr_block->next = next_block;
            curr_block = next_block;
        }

        JsonNode *node = &curr_block->items[curr_block->count];

        if (!parse_string(main, s, &node->key)) return false;

        skip_whitespace(s);
        if (s->curr >= s->end || *s->curr != ':') { set_error(s, "Expected ':'"); return false; }
        advance(s, 1); 

        if (!parse_element(main, s, &node->value, depth + 1)) return false;
        
        curr_block->count++;
        total_count++;

        skip_whitespace(s);
        if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }

        char c = *s->curr;
        if (c == ',') {
            advance(s, 1);
            if (s->flags & JSON_PARSE_ALLOW_COMMENTS) {
                skip_whitespace(s);
                if (s->curr < s->end && *s->curr == '}') {
                    advance(s, 1);
                    break;
                }
            }
        } 
        else if (c == '}') { advance(s, 1); break; } 
        else { set_error(s, "Expected ',' or '}'"); return false; }
    }

    obj->as.list.count = total_count;
    obj->as.list.items = arena_alloc_array(main, JsonNode, total_count);
    if (!obj->as.list.items) return false;
    
    size_t write_idx = 0;
    NodeBlock *b = head;
    while (b) {
        memcpy(&obj->as.list.items[write_idx], b->items, b->count * sizeof(JsonNode));
        write_idx += b->count;
        b = b->next;
    }
    
    arena_temp_end(temp_mem);
    return true;
}

typedef bool (*parse_func)(Arena *, ParseState *, JsonValue *, int);

static bool parse_string_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    (void)depth;
    v->type = JSON_STRING;
    return parse_string(main, s, &v->as.string);
}
static bool parse_array_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    v->type = JSON_ARRAY; return parse_array(main, s, v, depth);
}
static bool parse_object_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    v->type = JSON_OBJECT; return parse_object(main, s, v, depth);
}
static bool parse_number_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    (void)main; (void)depth;
    v->type = JSON_NUMBER; return parse_number(s, &v->as.number);
}
static bool parse_true_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    (void)main; (void)depth;
    if (s->end - s->curr >= 4 && s->curr[1] == 'r' && s->curr[2] == 'u' && s->curr[3] == 'e') {
        v->type = JSON_BOOL; v->as.boolean = true; advance(s, 4); return true;
    }
    set_error(s, "Expected 'true'"); return false;
}
static bool parse_false_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    (void)main; (void)depth;
    if (s->end - s->curr >= 5 && s->curr[1] == 'a' && s->curr[2] == 'l' && s->curr[3] == 's' && s->curr[4] == 'e') {
        v->type = JSON_BOOL; v->as.boolean = false; advance(s, 5); return true;
    }
    set_error(s, "Expected 'false'"); return false;
}
static bool parse_null_dispatch(Arena *main, ParseState *s, JsonValue *v, int depth) {
    (void)main; (void)depth;
    if (s->end - s->curr >= 4 && s->curr[1] == 'u' && s->curr[2] == 'l' && s->curr[3] == 'l') {
        v->type = JSON_NULL; advance(s, 4); return true;
    }
    set_error(s, "Expected 'null'"); return false;
}

static const parse_func dispatch_table[256] = {
    ['"'] = parse_string_dispatch, ['['] = parse_array_dispatch, ['{'] = parse_object_dispatch,
    ['-'] = parse_number_dispatch, ['0'] = parse_number_dispatch, ['1'] = parse_number_dispatch,
    ['2'] = parse_number_dispatch, ['3'] = parse_number_dispatch, ['4'] = parse_number_dispatch,
    ['5'] = parse_number_dispatch, ['6'] = parse_number_dispatch, ['7'] = parse_number_dispatch,
    ['8'] = parse_number_dispatch, ['9'] = parse_number_dispatch, ['t'] = parse_true_dispatch,
    ['f'] = parse_false_dispatch,  ['n'] = parse_null_dispatch,
};

static bool parse_element(Arena *main, ParseState *s, JsonValue *out_val, int depth) {
    skip_whitespace(s);
    if (s->curr >= s->end) { set_error(s, "Unexpected end"); return false; }
    out_val->offset = s->curr - s->start;
    unsigned char c = (unsigned char)*s->curr;
    parse_func func = dispatch_table[c];
    if (func) return func(main, s, out_val, depth);
    set_error(s, "Unexpected character '%c'", c);
    return false;
}

JsonValue *json_parse(Arena *main, Arena *scratch, const char *input, size_t len, int flags, JsonError *err) {
    if (!main || !scratch || !input || len == 0) return NULL;  
    if (err) memset(err, 0, sizeof(JsonError));

    ParseState s = {0};
    s.start = input; s.curr = input; s.end = input + len;
    s.err = err; s.scratch = scratch;
    s.flags = flags;

    JsonValue *root = arena_alloc_struct(main, JsonValue);
    if (!root) return NULL;

    if (!parse_element(main, &s, root, 0)) return NULL;
    
    skip_whitespace(&s);
    if (s.curr != s.end) {
        if (err) set_error(&s, "Garbage after JSON");
        return NULL;
    }
    return root;
}

/* --- Helpers and Builders --- */

JsonValue *json_get(JsonValue *obj, const char *key) {
    if (!obj || !key || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) return &obj->as.list.items[i].value;
    }
    return NULL;
}

JsonValue *json_at(JsonValue *arr, int index) {
    if (!arr || arr->type != JSON_ARRAY || index < 0) return NULL;
    if ((size_t)index >= arr->as.list.count) return NULL;
    return &arr->as.list.items[index].value;
}

void json_print(JsonValue *v, int indent) {
    if (!v) return;
    for (int i=0; i<indent; i++) printf("  ");
    switch (v->type) {
        case JSON_NULL:   printf("null\n"); break;
        case JSON_BOOL:   printf("%s\n", v->as.boolean ? "true" : "false"); break;
        case JSON_NUMBER: printf("%g\n", v->as.number); break;
        case JSON_STRING: printf("\"%s\"\n", v->as.string); break;
        case JSON_ARRAY:
            printf("[\n");
            for (size_t i = 0; i < v->as.list.count; i++) json_print(&v->as.list.items[i].value, indent + 1);
            for (int i=0; i<indent; i++) printf("  ");
            printf("]\n");
            break;
        case JSON_OBJECT:
            printf("{\n");
            for (size_t i = 0; i < v->as.list.count; i++) {
                for (int j=0; j<indent+1; j++) printf("  ");
                printf("\"%s\":\n", v->as.list.items[i].key);
                json_print(&v->as.list.items[i].value, indent + 2);
            }
            for (int i=0; i<indent; i++) printf("  ");
            printf("}\n");
            break;
    }
}

/* --- New QoL Implementations --- */

bool json_is_type(JsonValue *v, JsonType type) {
    return (v && v->type == type);
}

double json_get_number(JsonValue *obj, const char *key, double fallback) {
    JsonValue *v = json_get(obj, key);
    return (v && v->type == JSON_NUMBER) ? v->as.number : fallback;
}

const char* json_get_string(JsonValue *obj, const char *key, const char *fallback) {
    JsonValue *v = json_get(obj, key);
    return (v && v->type == JSON_STRING) ? v->as.string : fallback;
}

bool json_get_bool(JsonValue *obj, const char *key, bool fallback) {
    JsonValue *v = json_get(obj, key);
    return (v && v->type == JSON_BOOL) ? v->as.boolean : fallback;
}

JsonValue* json_get_case(JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcasecmp(obj->as.list.items[i].key, key) == 0) {
            return &obj->as.list.items[i].value;
        }
    }
    return NULL;
}

void json_remove_from_object(JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT) return;
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) {
            size_t items_to_move = obj->as.list.count - i - 1;
            if (items_to_move > 0) {
                memmove(&obj->as.list.items[i], &obj->as.list.items[i+1], items_to_move * sizeof(JsonNode));
            }
            obj->as.list.count--;
            return;
        }
    }
}

void json_remove_from_array(JsonValue *arr, size_t index) {
    if (!arr || arr->type != JSON_ARRAY || index >= arr->as.list.count) return;
    size_t items_to_move = arr->as.list.count - index - 1;
    if (items_to_move > 0) {
        memmove(&arr->as.list.items[index], &arr->as.list.items[index+1], items_to_move * sizeof(JsonNode));
    }
    arr->as.list.count--;
}


/* Detach: Removes the node and returns a pointer to a copy of the value in the Arena. */
JsonValue* json_detach_from_object(Arena *a, JsonValue *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key || !a) return NULL;
    
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) {
            // Allocate a new container in the arena to hold the detached data
            JsonValue *detached = (JsonValue *)arena_alloc(a, sizeof(JsonValue));
            if (detached) *detached = obj->as.list.items[i].value;
            
            size_t items_to_move = obj->as.list.count - i - 1;
            if (items_to_move > 0) {
                memmove(&obj->as.list.items[i], &obj->as.list.items[i+1], items_to_move * sizeof(JsonNode));
            }
            obj->as.list.count--;
            return detached;
        }
    }
    return NULL;
}

/* Replace: Finds a key and swaps the value. If key doesn't exist, it adds it. */
void json_replace_in_object(Arena *a, JsonValue *obj, const char *key, JsonValue *new_val) {
    if (!obj || obj->type != JSON_OBJECT || !key || !new_val) return;
    
    for (size_t i = 0; i < obj->as.list.count; i++) {
        if (strcmp(obj->as.list.items[i].key, key) == 0) {
            obj->as.list.items[i].value = *new_val;
            return;
        }
    }
    // If not found, just add it
    json_add(a, obj, key, new_val);
}

JsonValue* json_clone(Arena *dest_arena, JsonValue *src) {
    if (!src) return NULL;
    JsonValue *new_val = arena_alloc_struct(dest_arena, JsonValue);
    new_val->type = src->type;

    switch (src->type) {
        case JSON_NUMBER: new_val->as.number = src->as.number; break;
        case JSON_BOOL:   new_val->as.boolean = src->as.boolean; break;
        case JSON_STRING: {
            size_t len = strlen(src->as.string);
            new_val->as.string = arena_alloc_array(dest_arena, char, len + 1);
            memcpy(new_val->as.string, src->as.string, len + 1);
            break;
        }
        case JSON_ARRAY:
        case JSON_OBJECT: {
            new_val->as.list.count = src->as.list.count;
            new_val->as.list.items = arena_alloc_array(dest_arena, JsonNode, src->as.list.count);
            for (size_t i = 0; i < src->as.list.count; i++) {
                if (src->type == JSON_OBJECT) {
                    size_t klen = strlen(src->as.list.items[i].key);
                    new_val->as.list.items[i].key = arena_alloc_array(dest_arena, char, klen + 1);
                    memcpy(new_val->as.list.items[i].key, src->as.list.items[i].key, klen + 1);
                }
                JsonValue *cloned_sub = json_clone(dest_arena, &src->as.list.items[i].value);
                new_val->as.list.items[i].value = *cloned_sub;
            }
            break;
        }
        default: break;
    }
    return new_val;
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
    if (sb->len + len > sb->cap) {
        while (sb->len + len > sb->cap) sb->cap *= 2;
        sb->buf = (char *)realloc(sb->buf, sb->cap);
    }
    memcpy(sb->buf + sb->len, str, len);
    sb->len += len;
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
    const char *start = s;
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\' || c < 0x20) {
            if (s > start) sb_append(sb, start, s - start);
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
            start = s + 1;
        }
        s++;
    }
    if (s > start) sb_append(sb, start, s - start);
    sb_putc(sb, '"');
}

static void json_write_internal(JsonValue *v, StrBuilder *sb, int indent, bool pretty) {
    if (!v) return;
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
                if (n == (int64_t)n) { 
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
                    int len = snprintf(num_buf, sizeof(num_buf), "%.17g", n);
                    sb_append(sb, num_buf, len);
                }
            }
            break;
        }
        case JSON_STRING: w_escaped_string(sb, v->as.string); break;
        case JSON_ARRAY: {
            sb_putc(sb, '[');
            if (v->as.list.count > 0) {
                if (pretty) sb_putc(sb, '\n');
                for (size_t i = 0; i < v->as.list.count; i++) {
                    if (pretty) for (int j = 0; j < indent + 2; j++) sb_putc(sb, ' ');
                    json_write_internal(&v->as.list.items[i].value, sb, indent + (pretty ? 2 : 0), pretty);
                    if (i + 1 < v->as.list.count) {
                        sb_putc(sb, ',');
                        if (pretty) sb_putc(sb, '\n');
                    }
                }
                if (pretty) {
                    sb_putc(sb, '\n');
                    for (int i = 0; i < indent; i++) sb_putc(sb, ' ');
                }
            }
            sb_putc(sb, ']');
            break;
        }
        case JSON_OBJECT: {
            sb_putc(sb, '{');
            if (v->as.list.count > 0) {
                if (pretty) sb_putc(sb, '\n');
                for (size_t i = 0; i < v->as.list.count; i++) {
                    if (pretty) for (int j = 0; j < indent + 2; j++) sb_putc(sb, ' ');
                    w_escaped_string(sb, v->as.list.items[i].key);
                    sb_append(sb, pretty ? ": " : ":", pretty ? 2 : 1);
                    json_write_internal(&v->as.list.items[i].value, sb, indent + (pretty ? 2 : 0), pretty);
                    if (i + 1 < v->as.list.count) {
                        sb_putc(sb, ',');
                        if (pretty) sb_putc(sb, '\n');
                    }
                }
                if (pretty) {
                    sb_putc(sb, '\n');
                    for (int i = 0; i < indent; i++) sb_putc(sb, ' ');
                }
            }
            sb_putc(sb, '}');
            break;
        }
    }
}

char *json_to_string(Arena *a, JsonValue *v, bool pretty) {
    if (!a || !v) return NULL;
    StrBuilder sb;
    sb_init(&sb);
    json_write_internal(v, &sb, 0, pretty);
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

static void json_list_append(Arena *a, JsonValue *parent, const char *key, JsonValue *val) {
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
    
    new_items[old_count].value = *val;
    parent->as.list.items = new_items;
    parent->as.list.count = old_count + 1;
}

void json_add(Arena *a, JsonValue *obj, const char *key, JsonValue *val) {
    if (obj && key && val && obj->type == JSON_OBJECT) json_list_append(a, obj, key, val);
}
void json_add_string(Arena *a, JsonValue *obj, const char *key, const char *val) {
    if (val) {
        JsonValue str_val; str_val.type = JSON_STRING;
        size_t len = strlen(val);
        str_val.as.string = arena_alloc_array(a, char, len + 1);
        if (str_val.as.string) memcpy(str_val.as.string, val, len + 1);
        json_list_append(a, obj, key, &str_val);
    }
}
void json_add_number(Arena *a, JsonValue *obj, const char *key, double val) {
    JsonValue num_val; num_val.type = JSON_NUMBER; num_val.as.number = val;
    json_list_append(a, obj, key, &num_val);
}
void json_add_bool(Arena *a, JsonValue *obj, const char *key, bool val) {
    JsonValue bool_val; bool_val.type = JSON_BOOL; bool_val.as.boolean = val;
    json_list_append(a, obj, key, &bool_val);
}
void json_add_null(Arena *a, JsonValue *obj, const char *key) {
    JsonValue null_val; null_val.type = JSON_NULL;
    json_list_append(a, obj, key, &null_val);
}

void json_append(Arena *a, JsonValue *arr, JsonValue *val) {
    if (arr && val && arr->type == JSON_ARRAY) json_list_append(a, arr, NULL, val);
}
void json_append_string(Arena *a, JsonValue *arr, const char *val) {
    if (val) {
        JsonValue str_val; str_val.type = JSON_STRING;
        size_t len = strlen(val);
        str_val.as.string = arena_alloc_array(a, char, len + 1);
        if (str_val.as.string) memcpy(str_val.as.string, val, len + 1);
        json_list_append(a, arr, NULL, &str_val);
    }
}
void json_append_number(Arena *a, JsonValue *arr, double val) {
    JsonValue num_val; num_val.type = JSON_NUMBER; num_val.as.number = val;
    json_list_append(a, arr, NULL, &num_val);
}
void json_append_bool(Arena *a, JsonValue *arr, bool val) {
    JsonValue bool_val; bool_val.type = JSON_BOOL; bool_val.as.boolean = val;
    json_list_append(a, arr, NULL, &bool_val);
}
void json_append_null(Arena *a, JsonValue *arr) {
    JsonValue null_val; null_val.type = JSON_NULL;
    json_list_append(a, arr, NULL, &null_val);
}

#endif /* ARENA_JSON_IMPLEMENTATION */