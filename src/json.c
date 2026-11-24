/* json.c — a small, allocation-frugal, strict RFC 8259 JSON reader.
 *
 * Memory model
 * ------------
 * Everything a parse produces lives in a chain of fixed size blocks owned by
 * the document root: JsonValue cells, JsonMember arrays, element pointer
 * arrays and decoded strings are all carved out of those blocks by bumping a
 * pointer. json_free() therefore walks the block chain once and never visits
 * individual values, so its cost is O(number of blocks); a document that fits
 * in a single block is released with one free() and a single malloc() paid for
 * the whole parse.
 *
 * The block chain has to be reachable from the JsonValue * that json_parse()
 * returns (the frozen header has no field to hang it from), so the root value
 * is always the very first allocation out of the very first block. Its address
 * is exactly (char *)block + sizeof(ArenaBlock) and json_free() recovers the
 * block with a single subtraction. Blocks allocated after the first one are
 * linked behind it, in allocation order.
 *
 * Arrays and objects grow geometrically (4, 8, 16, ...), so a 100k member
 * document costs O(n) work instead of O(n^2); the capacity of each vector is
 * stored in a small header in front of its payload, because JsonValue has no
 * room for one.
 *
 * Parsing
 * -------
 * Recursive descent over the text with a hard nesting cap, so hostile input
 * such as "[[[[[[..." is rejected with an error instead of overflowing the C
 * stack. Every failure path goes through parser_error(), which records the
 * first error with a 1-based line and column; later errors are ignored so the
 * message the caller sees is the one that actually stopped the parse.
 *
 * Strings
 * -------
 * A string is copied out byte for byte, so UTF-8 already in the document is
 * passed through untouched and is never re-encoded. Bytes at or above 0x80 are
 * not validated as UTF-8: RFC 8259 only requires the JSON grammar to hold, and
 * checking the encoding would both cost a pass over every string and reject
 * documents this program has no opinion about. What is validated is what the
 * grammar demands: no raw control byte below 0x20, and no lone surrogate in a
 * \u escape, because a surrogate half has no legal UTF-8 encoding. See the
 * comment on read_escape() for why that differs from JSON.parse.
 */

#include "dshtok/json.h"
#include "dshtok/util.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Nesting deeper than this is rejected. A ledger is nowhere near that deep;
 * 256 frames of recursive descent is a few tens of kilobytes of stack. */
#define JSON_MAX_DEPTH 256

/* Payload size of a fresh arena block. Blocks are only ever appended, so this
 * trades a little slack in the last block for far fewer mallocs. */
#define ARENA_BLOCK_SIZE ((size_t)64 * 1024)

/* An allocation-alignment probe: a union of the widest scalar types the arena
 * hands out. sizeof(ArenaProbe) is both the arena alignment and a size that is
 * a multiple of it, which keeps every payload address suitably aligned. */
typedef union ArenaProbe {
    void *pointer;
    double real;
    long long whole;
} ArenaProbe;

#define ARENA_ALIGN ((size_t)sizeof(ArenaProbe))

/* One block of bump-allocated memory. `next` chains the blocks of a document
 * in allocation order; json_free() follows it from the block that holds the
 * root. The probe member forces the payload (which starts at
 * sizeof(ArenaBlock)) to be aligned for anything the arena stores. */
typedef struct ArenaBlock ArenaBlock;

struct ArenaBlock {
    ArenaBlock *next;
    size_t capacity;
    size_t used;
    ArenaProbe probe;
};

typedef struct {
    const char *text;
    size_t len;
    size_t pos;
    int depth;
    int failed;
    char *err;
    size_t errsz;
    ArenaBlock *head;
    ArenaBlock *tail;
    /* Reused scratch buffer for decoding strings; the decoded bytes are copied
     * into the arena once their final length is known. */
    char *scratch;
    size_t scratch_len;
    size_t scratch_cap;
} Parser;

/* Capacity header that sits immediately in front of a vector payload. */
typedef struct {
    ArenaProbe probe;
    size_t capacity;
} VecHeader;

/* --- diagnostics ---------------------------------------------------------- */

/* Record the first failure: "line L column C: message". line and column are
 * 1-based and count bytes, which is what editors show for ASCII JSON. */
static void parser_error(Parser *p, size_t pos, const char *message)
{
    size_t i;
    size_t line = 1;
    size_t column = 1;

    if (p->failed) {
        return;
    }
    p->failed = 1;
    if (pos > p->len) {
        pos = p->len;
    }
    for (i = 0; i < pos; ++i) {
        if (p->text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    if (p->err != NULL && p->errsz > 0) {
        snprintf(p->err, p->errsz, "line %llu column %llu: %s",
                 (unsigned long long)line, (unsigned long long)column, message);
    }
}

/* "unexpected character 'x'" for the byte at pos, or a plain end-of-input
 * message when the parse ran off the end of the buffer. */
static void error_at_byte(Parser *p, size_t pos, const char *eof_message)
{
    char message[64];
    unsigned char c;

    if (pos >= p->len) {
        parser_error(p, pos, eof_message);
        return;
    }
    c = (unsigned char)p->text[pos];
    if (c >= 0x20 && c < 0x7F) {
        snprintf(message, sizeof message, "unexpected character '%c'", (char)c);
    } else {
        snprintf(message, sizeof message, "unexpected byte 0x%02X", (unsigned)c);
    }
    parser_error(p, pos, message);
}

/* --- arena ---------------------------------------------------------------- */

static ArenaBlock *block_new(size_t capacity)
{
    ArenaBlock *block;

    block = (ArenaBlock *)xmalloc(sizeof(ArenaBlock) + capacity);
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0;
    return block;
}

/* Bump-allocate n bytes, padded to ARENA_ALIGN. Never returns memory that was
 * already handed out. Returns NULL only when the size arithmetic would
 * overflow, which cannot happen for a document that fits in memory but keeps
 * the multiplication and addition below honest. */
static void *arena_alloc(Parser *p, size_t n)
{
    size_t need;
    ArenaBlock *block;
    char *base;

    if (n == 0) {
        n = 1;
    }
    if (n > (size_t)-1 - ARENA_ALIGN - sizeof(ArenaBlock)) {
        parser_error(p, p->pos, "document too large");
        return NULL;
    }
    need = ((n + ARENA_ALIGN - 1) / ARENA_ALIGN) * ARENA_ALIGN;

    block = p->tail;
    if (block == NULL || block->capacity - block->used < need) {
        size_t capacity = ARENA_BLOCK_SIZE;
        if (capacity < need) {
            capacity = need;
        }
        block = block_new(capacity);
        if (p->tail == NULL) {
            p->head = block;
        } else {
            p->tail->next = block;
        }
        p->tail = block;
    }
    base = (char *)block + sizeof(ArenaBlock);
    block->used += need;
    return base + (block->used - need);
}

static void arena_release(Parser *p)
{
    ArenaBlock *block = p->head;

    while (block != NULL) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
    p->head = NULL;
    p->tail = NULL;
}

/* --- geometric vectors ---------------------------------------------------- */

/* Append one element to a vector stored in the arena. `payload` is the current
 * payload (NULL for an empty vector), `count` is the number of elements
 * already in it. Returns the payload to use from now on, or NULL after
 * recording an error. Amortised O(1) per element. */
static void *vec_push(Parser *p, void *payload, size_t count, size_t elem,
                      const void *item)
{
    size_t capacity = 0;
    size_t fresh_capacity;
    size_t bytes;
    VecHeader *header;

    if (payload != NULL) {
        header = ((VecHeader *)(void *)payload) - 1;
        capacity = header->capacity;
    }
    if (count >= capacity) {
        char *fresh_payload;
        fresh_capacity = capacity == 0 ? 4 : capacity * 2;
        if (fresh_capacity < capacity ||
            fresh_capacity > ((size_t)-1 - sizeof(VecHeader)) / elem) {
            parser_error(p, p->pos, "document too large");
            return NULL;
        }
        bytes = sizeof(VecHeader) + fresh_capacity * elem;
        header = (VecHeader *)arena_alloc(p, bytes);
        if (header == NULL) {
            return NULL;
        }
        header->capacity = fresh_capacity;
        fresh_payload = (char *)header + sizeof(VecHeader);
        if (payload != NULL && count > 0) {
            memcpy(fresh_payload, payload, count * elem);
        }
        payload = fresh_payload;
    }
    memcpy((char *)payload + count * elem, item, elem);
    return payload;
}

/* --- string scratch ------------------------------------------------------- */

static int scratch_byte(Parser *p, unsigned char byte)
{
    if (p->scratch_len >= p->scratch_cap) {
        size_t capacity = p->scratch_cap == 0 ? 64 : p->scratch_cap * 2;
        if (capacity < p->scratch_cap) {
            parser_error(p, p->pos, "string too large");
            return -1;
        }
        p->scratch = (char *)xrealloc(p->scratch, capacity);
        p->scratch_cap = capacity;
    }
    p->scratch[p->scratch_len++] = (char)byte;
    return 0;
}

/* --- numbers -------------------------------------------------------------- */

/* Powers of ten that a double holds exactly. Beyond 1e22 the value needs more
 * mantissa bits than a double has, so scaling in steps of 1e22 keeps the
 * rounding error around one unit in the last place. */
static const double kPowersOfTen[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* Convert the already validated number token s[0..n) to a double.
 *
 * strtod() would be the obvious tool, but it obeys LC_NUMERIC: in a locale
 * whose decimal separator is ',' it would read "1.5" as 1. This is a locale
 * independent conversion that is exact whenever the value is an integer of at
 * most 19 digits (the ledger case) and correctly rounded to within an ulp or
 * two otherwise. Overflow becomes +/-infinity and underflow becomes zero,
 * which is what a JSON reader should do for 1e400 and 1e-400. */
static double number_convert(const char *s, size_t n)
{
    size_t i = 0;
    int negative = 0;
    int kept = 0;
    int seen_significant = 0;
    unsigned long long mantissa = 0;
    long exponent = 0;
    double value;

    if (i < n && s[i] == '-') {
        negative = 1;
        ++i;
    }
    while (i < n && s[i] >= '0' && s[i] <= '9') {
        unsigned digit = (unsigned)(s[i] - '0');
        if (kept == 0 && !seen_significant && digit == 0) {
            /* Leading zeros carry no information. */
        } else if (kept < 19) {
            mantissa = mantissa * 10u + digit;
            ++kept;
            seen_significant = 1;
        } else {
            /* More significant integer digits than a double can hold. */
            ++exponent;
        }
        ++i;
    }
    if (i < n && s[i] == '.') {
        ++i;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            unsigned digit = (unsigned)(s[i] - '0');
            if (kept < 19) {
                mantissa = mantissa * 10u + digit;
                ++kept;
                --exponent;
            }
            /* Digits below the mantissa's precision only matter for correct
             * rounding, which a token of this shape never needs in practice. */
            ++i;
        }
    }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        int exponent_negative = 0;
        long scale = 0;
        ++i;
        if (i < n && (s[i] == '+' || s[i] == '-')) {
            exponent_negative = s[i] == '-';
            ++i;
        }
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (scale < 1000000L) {
                scale = scale * 10 + (long)(s[i] - '0');
            }
            ++i;
        }
        exponent += exponent_negative ? -scale : scale;
    }

    /* Clamp before scaling: anything past these bounds is already infinite or
     * zero, and the clamp keeps the scaling loops short. */
    if (exponent > 400) {
        exponent = 400;
    }
    if (exponent < -400) {
        exponent = -400;
    }

    value = (double)mantissa;
    while (exponent > 22) {
        value *= kPowersOfTen[22];
        exponent -= 22;
    }
    while (exponent < -22) {
        value /= kPowersOfTen[22];
        exponent += 22;
    }
    if (exponent > 0) {
        value *= kPowersOfTen[exponent];
    } else if (exponent < 0) {
        value /= kPowersOfTen[-exponent];
    }
    return negative ? -value : value;
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int is_whitespace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static void skip_whitespace(Parser *p)
{
    while (p->pos < p->len && is_whitespace(p->text[p->pos])) {
        ++p->pos;
    }
}

/* --- string escapes ------------------------------------------------------- */

static int hex_digit(unsigned char c)
{
    if (c >= '0' && c <= '9') {
        return (int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (int)(c - 'a') + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return (int)(c - 'A') + 10;
    }
    return -1;
}

/* Read exactly four hexadecimal digits at p->pos, advancing past them. */
static int read_hex4(Parser *p, unsigned *out)
{
    unsigned value = 0;
    int i;

    if (p->len - p->pos < 4) {
        parser_error(p, p->pos, "unexpected end of input in \\u escape");
        return -1;
    }
    for (i = 0; i < 4; ++i) {
        int digit = hex_digit((unsigned char)p->text[p->pos + (size_t)i]);
        if (digit < 0) {
            parser_error(p, p->pos + (size_t)i,
                         "invalid hexadecimal digit in \\u escape");
            return -1;
        }
        value = (value << 4) | (unsigned)digit;
    }
    p->pos += 4;
    *out = value;
    return 0;
}

/* Encode one Unicode scalar value as UTF-8, returning the byte count. */
static int utf8_encode(unsigned codepoint, char out[4])
{
    if (codepoint <= 0x7F) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FF) {
        out[0] = (char)(unsigned char)(0xC0u | (codepoint >> 6));
        out[1] = (char)(unsigned char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint <= 0xFFFF) {
        out[0] = (char)(unsigned char)(0xE0u | (codepoint >> 12));
        out[1] = (char)(unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (char)(unsigned char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
    out[0] = (char)(unsigned char)(0xF0u | (codepoint >> 18));
    out[1] = (char)(unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out[2] = (char)(unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[3] = (char)(unsigned char)(0x80u | (codepoint & 0x3Fu));
    return 4;
}

/* Decode one escape sequence, starting at the backslash, into the scratch
 * buffer. Surrogate pairs are combined into a single 4 byte UTF-8 sequence;
 * a lone surrogate is an error, matching the Unicode well-formedness rules
 * that RFC 8259 refers to. */
static int read_escape(Parser *p)
{
    unsigned char escape;
    unsigned codepoint;
    char utf8[4];
    int count;
    int i;

    ++p->pos; /* the backslash */
    if (p->pos >= p->len) {
        parser_error(p, p->pos, "unexpected end of input in escape sequence");
        return -1;
    }
    escape = (unsigned char)p->text[p->pos];
    switch (escape) {
    case '"':
        ++p->pos;
        return scratch_byte(p, '"');
    case '\\':
        ++p->pos;
        return scratch_byte(p, '\\');
    case '/':
        ++p->pos;
        return scratch_byte(p, '/');
    case 'b':
        ++p->pos;
        return scratch_byte(p, '\b');
    case 'f':
        ++p->pos;
        return scratch_byte(p, '\f');
    case 'n':
        ++p->pos;
        return scratch_byte(p, '\n');
    case 'r':
        ++p->pos;
        return scratch_byte(p, '\r');
    case 't':
        ++p->pos;
        return scratch_byte(p, '\t');
    case 'u':
        ++p->pos;
        if (read_hex4(p, &codepoint) != 0) {
            return -1;
        }
        if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            unsigned low;
            if (p->len - p->pos < 2 || p->text[p->pos] != '\\' ||
                p->text[p->pos + 1] != 'u') {
                parser_error(p, p->pos,
                             "unpaired high surrogate in \\u escape");
                return -1;
            }
            p->pos += 2;
            if (read_hex4(p, &low) != 0) {
                return -1;
            }
            if (low < 0xDC00u || low > 0xDFFFu) {
                parser_error(p, p->pos, "invalid low surrogate in \\u escape");
                return -1;
            }
            codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) +
                        (low - 0xDC00u);
        } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            parser_error(p, p->pos, "unpaired low surrogate in \\u escape");
            return -1;
        }
        count = utf8_encode(codepoint, utf8);
        for (i = 0; i < count; ++i) {
            if (scratch_byte(p, (unsigned char)utf8[i]) != 0) {
                return -1;
            }
        }
        return 0;
    default: {
        char message[64];
        if (p->pos >= p->len) {
            parser_error(p, p->pos,
                         "unexpected end of input in escape sequence");
            return -1;
        }
        if (escape >= 0x20 && escape < 0x7F) {
            snprintf(message, sizeof message, "invalid escape sequence '\\%c'",
                     (char)escape);
        } else {
            snprintf(message, sizeof message,
                     "invalid escape sequence, byte 0x%02X after the backslash",
                     (unsigned)escape);
        }
        /* Point at the backslash, which is what a reader will go looking for. */
        parser_error(p, p->pos - 1, message);
        return -1;
    }
    }
}

/* Parse a string literal into an arena allocated, NUL terminated buffer.
 * *out_len receives the decoded byte count, which is larger than strlen() when
 * the string contains a \u0000. Bytes >= 0x20 are copied through unchanged, so
 * UTF-8 text is preserved exactly and does not need to be decoded here. */
static char *read_string(Parser *p, size_t *out_len)
{
    char *result;
    size_t len;

    ++p->pos; /* opening quote; the caller checked it */
    p->scratch_len = 0;
    for (;;) {
        unsigned char c;
        if (p->pos >= p->len) {
            parser_error(p, p->pos, "unexpected end of input in string");
            return NULL;
        }
        c = (unsigned char)p->text[p->pos];
        if (c == '"') {
            ++p->pos;
            break;
        }
        if (c == '\\') {
            if (read_escape(p) != 0) {
                return NULL;
            }
            continue;
        }
        if (c < 0x20) {
            parser_error(p, p->pos, "bare control character in string");
            return NULL;
        }
        if (scratch_byte(p, c) != 0) {
            return NULL;
        }
        ++p->pos;
    }

    len = p->scratch_len;
    if (len > (size_t)-1 - 1) {
        parser_error(p, p->pos, "string too large");
        return NULL;
    }
    result = (char *)arena_alloc(p, len + 1);
    if (result == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(result, p->scratch, len);
    }
    result[len] = '\0';
    if (out_len != NULL) {
        *out_len = len;
    }
    return result;
}

/* --- values --------------------------------------------------------------- */

static JsonValue *read_value(Parser *p);

/* Match one of the three bare literals at p->pos. */
static int expect_literal(Parser *p, const char *literal, const char *message)
{
    size_t n = strlen(literal);

    if (p->len - p->pos < n || memcmp(p->text + p->pos, literal, n) != 0) {
        parser_error(p, p->pos, message);
        return -1;
    }
    p->pos += n;
    return 0;
}

/* Validate a number against the RFC 8259 grammar and convert it. The grammar
 * is checked by hand rather than delegated to strtod() so that "01", "1." and
 * "1e" are errors instead of being quietly half-read. */
static JsonValue *read_number(Parser *p, JsonValue *value)
{
    size_t start = p->pos;
    size_t i = p->pos;

    if (p->text[i] == '-') {
        ++i;
    }
    if (i >= p->len) {
        parser_error(p, i, "unexpected end of input in number");
        return NULL;
    }
    if (!is_digit(p->text[i])) {
        parser_error(p, i, "invalid number: expected a digit");
        return NULL;
    }
    if (p->text[i] == '0') {
        ++i;
        if (i < p->len && is_digit(p->text[i])) {
            parser_error(p, i, "invalid number: leading zero");
            return NULL;
        }
    } else {
        while (i < p->len && is_digit(p->text[i])) {
            ++i;
        }
    }
    if (i < p->len && p->text[i] == '.') {
        ++i;
        if (i >= p->len || !is_digit(p->text[i])) {
            parser_error(p, i, "invalid number: expected a digit after '.'");
            return NULL;
        }
        while (i < p->len && is_digit(p->text[i])) {
            ++i;
        }
    }
    if (i < p->len && (p->text[i] == 'e' || p->text[i] == 'E')) {
        ++i;
        if (i < p->len && (p->text[i] == '+' || p->text[i] == '-')) {
            ++i;
        }
        if (i >= p->len || !is_digit(p->text[i])) {
            parser_error(p, i, "invalid number: expected a digit in the exponent");
            return NULL;
        }
        while (i < p->len && is_digit(p->text[i])) {
            ++i;
        }
    }

    value->type = JSON_NUMBER;
    value->number = number_convert(p->text + start, i - start);
    p->pos = i;
    return value;
}

static JsonValue *read_object(Parser *p, JsonValue *value)
{
    value->type = JSON_OBJECT;
    if (p->depth >= JSON_MAX_DEPTH) {
        parser_error(p, p->pos, "nesting too deep");
        return NULL;
    }
    ++p->depth;
    ++p->pos; /* '{' */
    skip_whitespace(p);
    if (p->pos < p->len && p->text[p->pos] == '}') {
        ++p->pos;
        --p->depth;
        return value;
    }
    for (;;) {
        JsonMember member;
        JsonMember *grown;

        skip_whitespace(p);
        if (p->pos >= p->len) {
            parser_error(p, p->pos,
                         "unexpected end of input: expected an object key");
            return NULL;
        }
        if (p->text[p->pos] != '"') {
            parser_error(p, p->pos, "expected a \"...\" object key");
            return NULL;
        }
        member.key = read_string(p, NULL);
        if (member.key == NULL) {
            return NULL;
        }
        skip_whitespace(p);
        if (p->pos >= p->len) {
            parser_error(p, p->pos,
                         "unexpected end of input: expected ':' after object key");
            return NULL;
        }
        if (p->text[p->pos] != ':') {
            parser_error(p, p->pos, "expected ':' after object key");
            return NULL;
        }
        ++p->pos;
        member.value = read_value(p);
        if (member.value == NULL) {
            return NULL;
        }
        grown = (JsonMember *)vec_push(p, value->members, value->member_count,
                                       sizeof(JsonMember), &member);
        if (grown == NULL) {
            return NULL;
        }
        value->members = grown;
        ++value->member_count;

        skip_whitespace(p);
        if (p->pos >= p->len) {
            parser_error(p, p->pos,
                         "unexpected end of input: expected ',' or '}'");
            return NULL;
        }
        if (p->text[p->pos] == ',') {
            ++p->pos;
            skip_whitespace(p);
            if (p->pos < p->len && p->text[p->pos] == '}') {
                parser_error(p, p->pos, "trailing comma in object");
                return NULL;
            }
            continue;
        }
        if (p->text[p->pos] == '}') {
            ++p->pos;
            break;
        }
        parser_error(p, p->pos, "expected ',' or '}' in object");
        return NULL;
    }
    --p->depth;
    return value;
}

static JsonValue *read_array(Parser *p, JsonValue *value)
{
    value->type = JSON_ARRAY;
    if (p->depth >= JSON_MAX_DEPTH) {
        parser_error(p, p->pos, "nesting too deep");
        return NULL;
    }
    ++p->depth;
    ++p->pos; /* '[' */
    skip_whitespace(p);
    if (p->pos < p->len && p->text[p->pos] == ']') {
        ++p->pos;
        --p->depth;
        return value;
    }
    for (;;) {
        JsonValue *element;
        JsonValue **grown;

        skip_whitespace(p);
        element = read_value(p);
        if (element == NULL) {
            return NULL;
        }
        grown = (JsonValue **)vec_push(p, value->items, value->item_count,
                                       sizeof(JsonValue *), &element);
        if (grown == NULL) {
            return NULL;
        }
        value->items = grown;
        ++value->item_count;

        skip_whitespace(p);
        if (p->pos >= p->len) {
            parser_error(p, p->pos,
                         "unexpected end of input: expected ',' or ']'");
            return NULL;
        }
        if (p->text[p->pos] == ',') {
            ++p->pos;
            skip_whitespace(p);
            if (p->pos < p->len && p->text[p->pos] == ']') {
                parser_error(p, p->pos, "trailing comma in array");
                return NULL;
            }
            continue;
        }
        if (p->text[p->pos] == ']') {
            ++p->pos;
            break;
        }
        parser_error(p, p->pos, "expected ',' or ']' in array");
        return NULL;
    }
    --p->depth;
    return value;
}

/* Parse one value. The JsonValue cell for the value is allocated before any of
 * its children, which is what guarantees that the document root is the first
 * allocation out of the first block (see the file comment). */
static JsonValue *read_value(Parser *p)
{
    JsonValue *value;
    unsigned char c;

    skip_whitespace(p);
    if (p->pos >= p->len) {
        parser_error(p, p->pos, "unexpected end of input");
        return NULL;
    }
    value = (JsonValue *)arena_alloc(p, sizeof(JsonValue));
    if (value == NULL) {
        return NULL;
    }
    memset(value, 0, sizeof *value);

    c = (unsigned char)p->text[p->pos];
    switch (c) {
    case '{':
        return read_object(p, value);
    case '[':
        return read_array(p, value);
    case '"': {
        char *text;
        size_t len = 0;
        text = read_string(p, &len);
        if (text == NULL) {
            return NULL;
        }
        value->type = JSON_STRING;
        value->string = text;
        value->string_len = len;
        return value;
    }
    case 't':
        if (expect_literal(p, "true", "invalid literal: expected 'true'") != 0) {
            return NULL;
        }
        value->type = JSON_BOOL;
        value->boolean = 1;
        return value;
    case 'f':
        if (expect_literal(p, "false", "invalid literal: expected 'false'") != 0) {
            return NULL;
        }
        value->type = JSON_BOOL;
        value->boolean = 0;
        return value;
    case 'n':
        if (expect_literal(p, "null", "invalid literal: expected 'null'") != 0) {
            return NULL;
        }
        value->type = JSON_NULL;
        return value;
    default:
        if (c == '-' || is_digit((char)c)) {
            return read_number(p, value);
        }
        error_at_byte(p, p->pos, "unexpected end of input");
        return NULL;
    }
}

/* --- public API ----------------------------------------------------------- */

JsonValue *json_parse(const char *text, size_t len, char *err, size_t errsz)
{
    Parser p;
    JsonValue *root;

    if (err != NULL && errsz > 0) {
        err[0] = '\0';
    }
    if (text == NULL) {
        len = 0;
    }
    p.text = text;
    p.len = len;
    p.pos = 0;
    p.depth = 0;
    p.failed = 0;
    p.err = err;
    p.errsz = errsz;
    p.head = NULL;
    p.tail = NULL;
    p.scratch = NULL;
    p.scratch_len = 0;
    p.scratch_cap = 0;

    /* The first block is created up front so that the root value allocated by
     * read_value() lands at the very start of its payload. */
    p.head = block_new(ARENA_BLOCK_SIZE);
    p.tail = p.head;

    /* RFC 8259 section 8.1 allows a parser to ignore a byte order mark, and
     * ledgers edited on Windows sometimes carry one. Skip it, and nothing
     * else: any other byte outside the JSON grammar is still an error. */
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        p.pos = 3;
    }

    root = read_value(&p);
    if (root != NULL) {
        skip_whitespace(&p);
        if (p.pos != p.len) {
            parser_error(&p, p.pos, "trailing garbage after the root value");
            root = NULL;
        }
    }
    free(p.scratch);
    p.scratch = NULL;

    if (root == NULL) {
        arena_release(&p);
        if (err != NULL && errsz > 0 && err[0] == '\0') {
            str_copy(err, errsz, "line 1 column 1: invalid JSON");
        }
        return NULL;
    }
    return root;
}

void json_free(JsonValue *root)
{
    ArenaBlock *block;

    if (root == NULL) {
        return;
    }
    /* The root is the first allocation out of the first block, so the block
     * header sits immediately in front of it. Walking the chain from there
     * releases the whole document: O(number of blocks), never per value. */
    block = (ArenaBlock *)(void *)((char *)root - sizeof(ArenaBlock));
    while (block != NULL) {
        ArenaBlock *next = block->next;
        free(block);
        block = next;
    }
}

const JsonValue *json_get(const JsonValue *obj, const char *key)
{
    size_t i;

    if (obj == NULL || key == NULL || obj->type != JSON_OBJECT) {
        return NULL;
    }
    /* Reverse order: a key that appears twice resolves to the last member,
     * matching JSON.parse() in the JavaScript host that writes the ledger. */
    for (i = obj->member_count; i > 0; --i) {
        const JsonMember *member = &obj->members[i - 1];
        if (member->key != NULL && member->value != NULL &&
            strcmp(member->key, key) == 0) {
            return member->value;
        }
    }
    return NULL;
}

const JsonValue *json_at(const JsonValue *arr, size_t index)
{
    if (arr == NULL || arr->type != JSON_ARRAY || index >= arr->item_count) {
        return NULL;
    }
    return arr->items[index];
}

double json_number(const JsonValue *v, double def)
{
    if (v == NULL || v->type != JSON_NUMBER) {
        return def;
    }
    return v->number;
}

long long json_integer(const JsonValue *v, long long def)
{
    double d;

    if (v == NULL || v->type != JSON_NUMBER) {
        return def;
    }
    d = v->number;
    if (!(d == d)) { /* NaN: nothing sensible to return */
        return def;
    }
    /* 2^63 has an exact double representation, so these two comparisons are
     * the safe way to detect the saturating cases before casting. */
    if (d >= 9223372036854775808.0) {
        return LLONG_MAX;
    }
    if (d < -9223372036854775808.0) {
        return LLONG_MIN;
    }
    if (d >= 0.0) {
        return (long long)floor(d + 0.5);
    }
    return (long long)ceil(d - 0.5);
}

int json_bool(const JsonValue *v, int def)
{
    if (v == NULL || v->type != JSON_BOOL) {
        return def;
    }
    return v->boolean;
}

const char *json_string(const JsonValue *v, const char *def)
{
    if (v == NULL || v->type != JSON_STRING) {
        return def;
    }
    return v->string;
}

int json_is(const JsonValue *v, JsonType type)
{
    return v != NULL && v->type == type;
}
