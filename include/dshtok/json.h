#ifndef DSHTOK_JSON_H
#define DSHTOK_JSON_H

#include <stddef.h>

/* A small, allocation-frugal JSON reader.
 *
 * The ledger is a few megabytes of dense JSON, so the parser keeps one arena
 * and bumps pointers out of it: freeing the whole document is a single free(),
 * and there is no per-node malloc traffic. Strings are stored UTF-8 encoded and
 * NUL terminated, so they can be handed to printf directly.
 *
 * The parser is strict RFC 8259: no comments, no trailing commas, no NaN, no
 * single quotes, and duplicate object keys are accepted with last-one-wins
 * (which matches JSON.parse in the host that writes the ledger). */

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

typedef struct {
    char *key;          /* NUL terminated, UTF-8, never NULL */
    JsonValue *value;   /* never NULL */
} JsonMember;

struct JsonValue {
    JsonType type;

    /* JSON_BOOL */
    int boolean;

    /* JSON_NUMBER */
    double number;

    /* JSON_STRING: NUL terminated UTF-8, may contain embedded NULs in theory,
     * in which case string_len is longer than strlen(string). */
    char *string;
    size_t string_len;

    /* JSON_ARRAY */
    JsonValue **items;
    size_t item_count;

    /* JSON_OBJECT: members sorted? no — source order, see json_get() */
    JsonMember *members;
    size_t member_count;
};

/* Parse text[0..len) as one JSON document. On success returns the root value,
 * which stays valid until json_free(). On failure returns NULL and, when err is
 * not NULL, writes a message such as
 *   "line 3 column 17: expected ':' after object key"
 * into err. The message always names a line and a column. */
JsonValue *json_parse(const char *text, size_t len, char *err, size_t errsz);

/* Release everything json_parse() allocated for root. NULL is a no-op. */
void json_free(JsonValue *root);

/* --- accessors: all of them accept NULL and behave as "absent" --- */

/* Object member lookup. Returns NULL when obj is not an object or the key is
 * missing. When the same key appears twice the last one wins. */
const JsonValue *json_get(const JsonValue *obj, const char *key);

/* Array element. Returns NULL when arr is not an array or the index is out of
 * range. */
const JsonValue *json_at(const JsonValue *arr, size_t index);

/* Convenience numeric read. Returns def when v is not a number (a JSON string
 * holding a number is not accepted — the ledger always emits numbers). */
double json_number(const JsonValue *v, double def);

/* Convenience integer read: rounds the number, saturating on overflow. */
long long json_integer(const JsonValue *v, long long def);

/* Convenience boolean read: JSON true -> 1, false -> 0, anything else -> def. */
int json_bool(const JsonValue *v, int def);

/* Convenience string read. Returns def when v is not a string. */
const char *json_string(const JsonValue *v, const char *def);

/* True when v is non-NULL and of the given type. */
int json_is(const JsonValue *v, JsonType type);

#endif /* DSHTOK_JSON_H */
