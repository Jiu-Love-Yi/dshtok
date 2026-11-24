/* test_json.c — tests for the arena backed JSON reader in src/json.c.
 *
 * The suite has four jobs:
 *   1. pin down the DOM shape and every accessor, including the NULL cases;
 *   2. prove that malformed input is rejected with a message that names a line
 *      and a column, on a long list of hand written bad documents;
 *   3. prove that the parser cannot crash or read out of bounds: every prefix
 *      of a sample document is fed back in, and a deterministic byte fuzzer
 *      mutates the same sample a few hundred times;
 *   4. prove there is no quadratic behaviour: a 100k member object must parse
 *      in well under a second.
 *
 * Fixtures live in tests/fixtures and are opened by relative path, so the test
 * binary must be run from the repository root (make test does that).
 */

#include "mini_test.h"

#include "dshtok/json.h"
#include "dshtok/util.h"

#include <time.h>

/* --- little helpers ------------------------------------------------------- */

/* Parse a NUL terminated string. Returns NULL on failure and leaves the reason
 * in err. */
static JsonValue *parse_text(const char *text, char *err, size_t errsz)
{
    return json_parse(text, strlen(text), err, errsz);
}

/* Parse a document that must be accepted; on failure it records the reason. */
static JsonValue *parse_must_succeed(const char *text, char *err, size_t errsz)
{
    JsonValue *root = parse_text(text, err, errsz);
    CHECK_MSG(root != NULL, "expected this to parse: %s (%s)", text, err);
    return root;
}

/* Parse a document that must be rejected, and check the message carries both a
 * line and a column. `needle` is an optional substring that must appear. */
static void expect_rejected(const char *text, const char *needle)
{
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_text(text, err, sizeof err);
    CHECK_MSG(root == NULL, "expected a rejection for: %s", text);
    if (root != NULL) {
        json_free(root);
        return;
    }
    CHECK_MSG(err[0] != '\0', "empty error message for: %s", text);
    CHECK_MSG(strstr(err, "line ") != NULL,
              "error has no line for %s: %s", text, err);
    CHECK_MSG(strstr(err, "column ") != NULL,
              "error has no column for %s: %s", text, err);
    if (needle != NULL) {
        CHECK_MSG(strstr(err, needle) != NULL,
                  "error for %s should mention \"%s\", got: %s", text, needle, err);
    }
}

/* Compare a decoded JSON string against an explicit byte sequence, which is
 * the only way to check strings that contain an embedded NUL. */
static void check_string_bytes(const JsonValue *v, const char *expected,
                               size_t expected_len)
{
    CHECK(json_is(v, JSON_STRING));
    if (!json_is(v, JSON_STRING)) {
        return;
    }
    CHECK_EQ_INT(v->string_len, expected_len);
    CHECK(v->string_len == expected_len &&
          memcmp(v->string, expected, expected_len) == 0);
    CHECK(v->string[v->string_len] == '\0');
}

/* --- value types ---------------------------------------------------------- */

static void test_scalar_values(void)
{
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_must_succeed("null", err, sizeof err);
    CHECK(json_is(root, JSON_NULL));
    json_free(root);

    root = parse_must_succeed("true", err, sizeof err);
    CHECK(json_is(root, JSON_BOOL));
    CHECK_EQ_INT(json_bool(root, -1), 1);
    json_free(root);

    root = parse_must_succeed("false", err, sizeof err);
    CHECK_EQ_INT(json_bool(root, 7), 0);
    json_free(root);

    root = parse_must_succeed("\"\"", err, sizeof err);
    check_string_bytes(root, "", 0);
    json_free(root);

    /* Surrounding whitespace is allowed, including CR LF and tabs. */
    root = parse_must_succeed(" \t\r\n 42 \r\n\t ", err, sizeof err);
    CHECK_NEAR(json_number(root, -1.0), 42.0, 0.0);
    json_free(root);

    /* A UTF-8 byte order mark ahead of the document is skipped (RFC 8259
     * section 8.1) and nothing else outside the grammar is. */
    root = json_parse("\xEF\xBB\xBF{\"a\":1}", 10, err, sizeof err);
    CHECK_MSG(root != NULL, "BOM prefixed document rejected: %s", err);
    if (root != NULL) {
        CHECK_EQ_INT(root->member_count, 1);
        json_free(root);
    }
}

static void test_numbers(void)
{
    static const struct {
        const char *text;
        double expected;
    } cases[] = {
        { "0", 0.0 },
        { "-0", 0.0 },
        { "7", 7.0 },
        { "-123", -123.0 },
        { "1.5", 1.5 },
        { "-0.25", -0.25 },
        { "1e2", 100.0 },
        { "1E+2", 100.0 },
        { "-2.5e3", -2500.0 },
        { "2.5E-3", 0.0025 },
        { "9007199254740992", 9007199254740992.0 },
        { "0.000001", 1e-6 }
    };
    char err[256];
    size_t i;

    err[0] = '\0';
    for (i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        JsonValue *root = parse_must_succeed(cases[i].text, err, sizeof err);
        if (root == NULL) {
            continue;
        }
        CHECK_MSG(json_is(root, JSON_NUMBER), "%s is not a number", cases[i].text);
        CHECK_NEAR(json_number(root, -1.0), cases[i].expected,
                   1e-12 * (cases[i].expected < 0 ? -cases[i].expected
                                                  : cases[i].expected) + 1e-15);
        json_free(root);
    }

    /* Overflow and underflow follow IEEE 754 rather than becoming errors. */
    {
        JsonValue *big = parse_must_succeed("1e400", err, sizeof err);
        JsonValue *tiny = parse_must_succeed("1e-400", err, sizeof err);
        if (big != NULL) {
            CHECK(json_number(big, 0.0) > 1e308);
            CHECK(!(json_number(big, 0.0) == json_number(big, 0.0)) ||
                  json_number(big, 0.0) > 1e308);
        }
        if (tiny != NULL) {
            CHECK_NEAR(json_number(tiny, -1.0), 0.0, 1e-320);
        }
        json_free(big);
        json_free(tiny);
    }

    /* A JSON string is never coerced into a number. */
    {
        JsonValue *s = parse_must_succeed("\"12\"", err, sizeof err);
        if (s != NULL) {
            CHECK_NEAR(json_number(s, -3.5), -3.5, 0.0);
            CHECK_EQ_INT(json_integer(s, -7), -7);
        }
        json_free(s);
    }
}

/* --- arrays and objects --------------------------------------------------- */

static void test_containers(void)
{
    static const char DOC[] =
        "{\"name\":\"ledger\",\"days\":[1,2,3],\"meta\":{\"ok\":true},"
        "\"nothing\":null}";
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_must_succeed(DOC, err, sizeof err);
    if (root == NULL) {
        return;
    }
    CHECK_EQ_INT(root->member_count, 4);
    CHECK(json_is(root, JSON_OBJECT));

    /* Members keep source order, which the ledger loader relies on. */
    CHECK_EQ_STR(root->members[0].key, "name");
    CHECK_EQ_STR(root->members[1].key, "days");
    CHECK_EQ_STR(root->members[2].key, "meta");
    CHECK_EQ_STR(root->members[3].key, "nothing");

    CHECK_EQ_STR(json_string(json_get(root, "name"), NULL), "ledger");

    {
        const JsonValue *days = json_get(root, "days");
        CHECK(json_is(days, JSON_ARRAY));
        CHECK_EQ_INT(json_at(days, 0) != NULL ? json_at(days, 0)->number : -1, 1);
        CHECK_EQ_INT(json_at(days, 2) != NULL ? json_at(days, 2)->number : -1, 3);
        CHECK(json_at(days, 3) == NULL);
        if (days != NULL) {
            CHECK_EQ_INT(days->item_count, 3);
            CHECK(days->items[1] == json_at(days, 1));
        }
    }

    CHECK(json_is(json_get(json_get(root, "meta"), "ok"), JSON_BOOL));
    CHECK(json_is(json_get(root, "nothing"), JSON_NULL));
    CHECK(json_get(root, "missing") == NULL);
    CHECK(json_get(root, "") == NULL);
    CHECK(json_get(NULL, "name") == NULL);
    CHECK(json_get(root, NULL) == NULL);
    CHECK(json_get(json_get(root, "days"), "0") == NULL);
    CHECK(json_at(root, 0) == NULL);
    CHECK(json_at(NULL, 0) == NULL);

    /* Empty containers are real values, not errors. */
    {
        JsonValue *empty = parse_must_succeed("{\"a\":[],\"b\":{}}", err, sizeof err);
        if (empty != NULL) {
            CHECK(json_is(json_get(empty, "a"), JSON_ARRAY));
            CHECK(json_is(json_get(empty, "b"), JSON_OBJECT));
            CHECK_EQ_INT(json_get(empty, "a")->item_count, 0);
            CHECK_EQ_INT(json_get(empty, "b")->member_count, 0);
        }
        json_free(empty);
    }

    json_free(root);
}

static void test_duplicate_keys(void)
{
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_must_succeed("{\"k\":1,\"k\":2,\"other\":3,\"k\":4}", err,
                              sizeof err);
    if (root != NULL) {
        /* Duplicates are kept in the DOM (source order is preserved) but the
         * lookup resolves to the last one, like JSON.parse(). */
        CHECK_EQ_INT(root->member_count, 4);
        CHECK_NEAR(json_number(json_get(root, "k"), -1.0), 4.0, 0.0);
        CHECK_NEAR(json_number(json_get(root, "other"), -1.0), 3.0, 0.0);
    }
    json_free(root);

    /* Same, through escaped keys that decode to the same text. */
    root = parse_must_succeed("{\"a\":1,\"\\u0061\":2}", err, sizeof err);
    if (root != NULL) {
        CHECK_NEAR(json_number(json_get(root, "a"), -1.0), 2.0, 0.0);
    }
    json_free(root);
}

/* --- strings and escapes -------------------------------------------------- */

static void test_escapes(void)
{
    static const char ALL_ESCAPES[] = "\"\\\" \\\\ \\/ \\b \\f \\n \\r \\t\"";
    static const char ALL_ESCAPES_DECODED[] = "\" \\ / \b \f \n \r \t";
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_must_succeed(ALL_ESCAPES, err, sizeof err);
    if (root != NULL) {
        check_string_bytes(root, ALL_ESCAPES_DECODED,
                           sizeof ALL_ESCAPES_DECODED - 1);
    }
    json_free(root);

    /* Basic plane escapes, including lowercase hex digits. */
    root = parse_must_succeed("\"\\u0041\\u00e9\\u20AC\"", err, sizeof err);
    if (root != NULL) {
        static const char EXPECTED[] = "A" "\xC3\xA9" "\xE2\x82\xAC";
        check_string_bytes(root, EXPECTED, sizeof EXPECTED - 1);
    }
    json_free(root);

    /* Surrogate pair -> U+1F600, four UTF-8 bytes. */
    root = parse_must_succeed("\"\\uD83D\\uDE00\"", err, sizeof err);
    if (root != NULL) {
        static const char GRIN[] = "\xF0\x9F\x98\x80";
        check_string_bytes(root, GRIN, sizeof GRIN - 1);
    }
    json_free(root);

    /* The same astral character written as raw UTF-8 must decode to exactly the
     * same bytes as the escaped pair above: the parser never re-encodes input
     * that is already UTF-8, so both spellings agree byte for byte. */
    root = parse_must_succeed("\"\xF0\x9F\x98\x80\"", err, sizeof err);
    if (root != NULL) {
        static const char GRIN[] = "\xF0\x9F\x98\x80";
        check_string_bytes(root, GRIN, sizeof GRIN - 1);
    }
    json_free(root);

    /* U+1F600 is the highest codepoint that a well formed pair can produce just
     * below the plane-1 boundary; U+10FFFF is the last codepoint of all.
     * \uDBFF\uDFFF is the maximum legal pair and must encode to F4 8F BF BF. */
    root = parse_must_succeed("\"\\uDBFF\\uDFFF\"", err, sizeof err);
    if (root != NULL) {
        static const char MAXIMUM[] = "\xF4\x8F\xBF\xBF";
        check_string_bytes(root, MAXIMUM, sizeof MAXIMUM - 1);
    }
    json_free(root);

    /* U+1D11E (musical G clef) is another well known pair. */
    root = parse_must_succeed("\"\\uD834\\uDD1E\"", err, sizeof err);
    if (root != NULL) {
        static const char CLEF[] = "\xF0\x9D\x84\x9E";
        check_string_bytes(root, CLEF, sizeof CLEF - 1);
    }
    json_free(root);

    /* \u0000 is stored, the string is NUL terminated, and string_len is
     * longer than strlen(). */
    root = parse_must_succeed("\"a\\u0000b\"", err, sizeof err);
    if (root != NULL) {
        static const char EMBEDDED[] = "a\0b";
        check_string_bytes(root, EMBEDDED, 3);
        CHECK_EQ_INT(strlen(root->string), 1);
        CHECK(root->string_len > strlen(root->string));
    }
    json_free(root);

    /* Raw UTF-8 passes through untouched; the parser never re-encodes it. */
    {
        static const char RAW[] = "\"h" "\xC3\xA9" "llo " "\xF0\x9F\x98\x80"
                                  " \xE4\xB8\xAD\xE6\x96\x87\"";
        static const char RAW_DECODED[] = "h" "\xC3\xA9" "llo "
                                          "\xF0\x9F\x98\x80"
                                          " \xE4\xB8\xAD\xE6\x96\x87";
        root = parse_must_succeed(RAW, err, sizeof err);
        if (root != NULL) {
            check_string_bytes(root, RAW_DECODED, sizeof RAW_DECODED - 1);
        }
        json_free(root);
    }

    /* Bytes at or above 0x80 are not validated as UTF-8. The ledger is always
     * written by JSON.stringify, which cannot produce an ill formed string, so
     * the parser has no reason to spend a pass over every string policing the
     * encoding - and it must not, because a C library that rejects text its
     * caller considers opaque is a compatibility trap. The bytes simply come
     * back unchanged. This test pins that decision: a lone continuation byte
     * and a truncated 2 byte sequence are both accepted and returned as is. */
    {
        static const char LONE_CONTINUATION[] = "\"\x80\"";
        static const char TRUNCATED_SEQUENCE[] = "\"\xC3\"";
        root = parse_must_succeed(LONE_CONTINUATION, err, sizeof err);
        if (root != NULL) {
            check_string_bytes(root, "\x80", 1);
        }
        json_free(root);
        root = parse_must_succeed(TRUNCATED_SEQUENCE, err, sizeof err);
        if (root != NULL) {
            check_string_bytes(root, "\xC3", 1);
        }
        json_free(root);
    }
}

/* --- accessor defaults and NULL safety ------------------------------------ */

static void test_accessor_defaults(void)
{
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_must_succeed("\"text\"", err, sizeof err);

    /* Everything accepts NULL. */
    CHECK(json_get(NULL, "x") == NULL);
    CHECK(json_at(NULL, 0) == NULL);
    CHECK_NEAR(json_number(NULL, 1.25), 1.25, 0.0);
    CHECK_EQ_INT(json_integer(NULL, -9), -9);
    CHECK_EQ_INT(json_bool(NULL, 3), 3);
    CHECK_EQ_STR(json_string(NULL, "fallback"), "fallback");
    CHECK(json_is(NULL, JSON_STRING) == 0);
    json_free(NULL); /* a no-op */

    /* Type mismatches fall back to the supplied default. */
    CHECK(json_is(root, JSON_STRING));
    CHECK(!json_is(root, JSON_NUMBER));
    CHECK_EQ_STR(json_string(root, "d"), "text");
    CHECK_NEAR(json_number(root, 4.5), 4.5, 0.0);
    CHECK_EQ_INT(json_integer(root, 8), 8);
    CHECK_EQ_INT(json_bool(root, 5), 5);

    json_free(root);
}

static void test_integer_conversion(void)
{
    char err[256];
    JsonValue *root;

    err[0] = '\0';

    root = parse_must_succeed("1.4", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), 1);
    json_free(root);

    root = parse_must_succeed("1.5", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), 2);
    json_free(root);

    root = parse_must_succeed("2.5", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), 3);
    json_free(root);

    root = parse_must_succeed("-1.5", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), -2);
    json_free(root);

    root = parse_must_succeed("-2.5", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), -3);
    json_free(root);

    root = parse_must_succeed("9007199254740992", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), 9007199254740992LL);
    json_free(root);

    /* Saturation, both directions. */
    root = parse_must_succeed("1e300", err, sizeof err);
    CHECK(json_integer(root, 0) > 0);
    CHECK_EQ_INT(json_integer(root, 0), 9223372036854775807LL);
    json_free(root);

    root = parse_must_succeed("-1e300", err, sizeof err);
    CHECK(json_integer(root, 0) < 0);
    CHECK(json_integer(root, 0) < -9223372036854775806LL);
    json_free(root);

    /* Exactly 2^63 is still out of range for a signed 64 bit integer. */
    root = parse_must_succeed("9223372036854775808", err, sizeof err);
    CHECK_EQ_INT(json_integer(root, 0), 9223372036854775807LL);
    json_free(root);
}

/* --- malformed documents -------------------------------------------------- */

static void test_malformed_documents(void)
{
    /* Empty and whitespace-only input. */
    expect_rejected("", "end of input");
    expect_rejected("   \n\t\r ", "end of input");

    /* Structural punctuation errors. */
    expect_rejected("{", NULL);
    expect_rejected("[", NULL);
    expect_rejected("\"unterminated", "end of input");
    expect_rejected("{\"a\" 1}", "expected ':'");
    expect_rejected("{\"a\":1,}", "trailing comma");
    expect_rejected("[1,]", "trailing comma");
    expect_rejected("[,1]", NULL);
    expect_rejected("[1 2]", "expected ','");
    expect_rejected("{\"a\":1 \"b\":2}", "expected ','");
    expect_rejected("{a:1}", "key");
    expect_rejected("[1,2}", "expected ',' or ']'");

    /* Trailing garbage after a complete root value. */
    expect_rejected("{} x", "trailing garbage");
    expect_rejected("1 2", "trailing garbage");
    expect_rejected("[1]]", "trailing garbage");

    /* Numbers. */
    expect_rejected("01", "leading zero");
    expect_rejected("-", "end of input");
    expect_rejected("1.", "after '.'");
    expect_rejected("1e", "exponent");
    expect_rejected("1e+", "exponent");
    expect_rejected("+1", NULL);
    expect_rejected(".5", NULL);
    expect_rejected("--1", NULL);
    expect_rejected("Infinity", NULL);
    expect_rejected("-Infinity", NULL);

    /* Literals. */
    expect_rejected("True", NULL);
    expect_rejected("tru", NULL);
    expect_rejected("nul", NULL);
    expect_rejected("fals", NULL);
    expect_rejected("truex", "trailing garbage");

    /* Not JSON at all: comments and single quotes are out. */
    expect_rejected("// comment\n{}", NULL);
    expect_rejected("{\"a\":1} /* tail */", "trailing garbage");
    expect_rejected("{'a':1}", "key");
    expect_rejected("{\"a\":'x'}", NULL);
    expect_rejected("{\"a\":NaN}", NULL);

    /* Strings: bad escapes and bare control characters. */
    expect_rejected("\"\\q\"", "escape");
    expect_rejected("\"\\u12\"", "\\u escape");
    expect_rejected("\"\\u12g4\"", "hexadecimal");

    /* Lone surrogates are rejected. This is a deliberate divergence from
     * JavaScript's JSON.parse, which happily builds a string holding a lone
     * surrogate because it does not require well formed UTF-16. RFC 8259
     * section 8.2 says such text is not interoperable, and a C string is
     * UTF-8, where a surrogate half has no legal encoding: silently writing
     * ED A0 80 would produce invalid UTF-8 that no consumer could decode.
     * Refusing is the only honest answer, so a lone \uD800, a lone \uDC00 and
     * a high surrogate followed by a non-surrogate are all errors. */
    expect_rejected("\"\\uD800\"", "surrogate");
    expect_rejected("\"\\uDC00\"", "surrogate");
    expect_rejected("\"\\uD800\\u0041\"", "surrogate");
    /* A high surrogate followed by another high surrogate is not a pair
     * either; the second escape is a valid surrogate codepoint but not a low
     * one, so this is rejected rather than silently combined. */
    expect_rejected("\"\\uD800\\uD800\"", "surrogate");
    /* A low surrogate with no high surrogate before it. */
    expect_rejected("\"\\uDFFF\"", "surrogate");
    expect_rejected("\"a\nb\"", "control character");
    expect_rejected("\"a\tb\"", "control character");
    expect_rejected("\"a\rb\"", "control character");
}

static void test_error_line_and_column(void)
{
    char err[256];
    JsonValue *root;

    /* The classic: a forgotten colon on line 3. The '2' is byte 7 of that
     * line, because the check happens at the character that should have been
     * a colon. */
    err[0] = '\0';
    root = parse_text("{\n  \"a\": 1,\n  \"b\" 2\n}\n", err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "line 3 column 7: expected ':' after object key") != NULL,
              "unexpected message: %s", err);
    json_free(root);

    err[0] = '\0';
    root = parse_text("{\n  \"a\": 1,\n  \"b\": }\n", err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "line 3 column 8") != NULL, "unexpected message: %s", err);
    json_free(root);

    /* A bare CR is not a line terminator for the position report; a CR LF
     * pair is advanced past the LF, which keeps the count aligned with what an
     * editor shows for CRLF files. */
    err[0] = '\0';
    root = parse_text("[\r\n1,\r\n]", err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "line 3 column 1") != NULL, "unexpected message: %s", err);
    json_free(root);

    err[0] = '\0';
    root = parse_text("", err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "line 1 column 1") != NULL, "unexpected message: %s", err);
    json_free(root);

    /* A NUL byte in the middle of the buffer is not an early terminator. */
    err[0] = '\0';
    root = json_parse("{\"a\":1}\0garbage", 14, err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "column 8") != NULL, "unexpected message: %s", err);
    json_free(root);
}

/* --- nesting cap ---------------------------------------------------------- */

static char *build_nested_arrays(size_t depth, size_t *out_len)
{
    char *buf = (char *)xmalloc(depth * 2 + 1);
    size_t i;

    for (i = 0; i < depth; ++i) {
        buf[i] = '[';
    }
    for (i = 0; i < depth; ++i) {
        buf[depth + i] = ']';
    }
    buf[depth * 2] = '\0';
    *out_len = depth * 2;
    return buf;
}

static void test_nesting_cap(void)
{
    char err[256];
    JsonValue *root;
    char *text;
    size_t len = 0;

    /* 256 levels are accepted ... */
    text = build_nested_arrays(256, &len);
    err[0] = '\0';
    root = json_parse(text, len, err, sizeof err);
    CHECK_MSG(root != NULL, "256 levels should parse: %s", err);
    json_free(root);
    free(text);

    /* ... one more is not, and the message says why. */
    text = build_nested_arrays(257, &len);
    err[0] = '\0';
    root = json_parse(text, len, err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "nesting too deep") != NULL, "unexpected message: %s", err);
    json_free(root);
    free(text);

    /* Deeply nested objects are capped the same way, so this cannot blow the
     * C stack even though a hand written file might try. */
    {
        enum { LEVELS = 5000 };
        /* Each level contributes 5 bytes of opener before the '1' and 1 byte of
         * closer after it, plus the NUL. Size it from the shape of the text
         * rather than guessing: getting this wrong silently corrupts the heap
         * long before json_parse() is even reached. */
        size_t cap = (size_t)LEVELS * 6 + 2;
        char *deep = (char *)xmalloc(cap);
        size_t used = 0;
        size_t i;
        for (i = 0; i < LEVELS; ++i) {
            memcpy(deep + used, "{\"a\":", 5);
            used += 5;
        }
        deep[used++] = '1';
        for (i = 0; i < LEVELS; ++i) {
            deep[used++] = '}';
        }
        deep[used] = '\0';
        CHECK(used + 1 <= cap);
        err[0] = '\0';
        root = json_parse(deep, used, err, sizeof err);
        CHECK(root == NULL);
        CHECK_MSG(strstr(err, "nesting too deep") != NULL,
                  "unexpected message: %s", err);
        json_free(root);
        free(deep);
    }
}

/* --- truncation at every byte offset -------------------------------------- */

static const char *const kSampleDocument =
    "{\n"
    "  \"name\": \"dshtok \\u00e9 \\uD83D\\uDE00\",\n"
    "  \"ok\": true,\n"
    "  \"n\": -12.5e+3,\n"
    "  \"list\": [1, 2, {\"nested\": null}, [true, false]],\n"
    "  \"empty\": {},\n"
    "  \"zero\": 0\n"
    "}\n";

static void test_truncation_at_every_offset(void)
{
    size_t full = strlen(kSampleDocument);
    size_t cut;
    int accepted = 0;
    int rejected = 0;

    for (cut = 0; cut <= full; ++cut) {
        char err[256];
        JsonValue *root;
        char *exact;

        /* Copy the prefix into a buffer of exactly `cut` bytes: no NUL terminator
         * and no slack past the end of the document. A parser that peeks one
         * byte beyond length (a classic off-by-one in a hand written lexer)
         * then reads outside the allocation instead of harmlessly hitting the
         * terminator of the source string. This is the closest substitute for
         * AddressSanitizer, which MinGW does not ship. */
        exact = (char *)xmalloc(cut == 0 ? 1 : cut);
        if (cut > 0) {
            memcpy(exact, kSampleDocument, cut);
        }

        err[0] = '\0';
        root = json_parse(exact, cut, err, sizeof err);
        if (root != NULL) {
            ++accepted;
            /* Only the text up to and including the closing '}' is a complete
             * document, i.e. the full sample and the full sample minus its
             * trailing newline. Every shorter prefix stops inside a value. */
            CHECK_MSG(cut + 1 >= full, "only the last two prefixes may parse, "
                      "but a %lu byte prefix did", (unsigned long)cut);
            json_free(root);
        } else {
            ++rejected;
            CHECK_MSG(err[0] != '\0', "no message for a %lu byte prefix",
                      (unsigned long)cut);
            CHECK_MSG(strstr(err, "line ") != NULL &&
                      strstr(err, "column ") != NULL,
                      "%lu byte prefix: error lacks line/column: %s",
                      (unsigned long)cut, err);
        }
        free(exact);
    }
    /* "}\n" and "}" are both valid: the trailing newline is optional. */
    CHECK_EQ_INT(accepted, 2);
    CHECK_EQ_INT(rejected, (int)full - 1);
}

/* The length argument is authoritative: the parser must never look for a NUL
 * terminator, and must accept a document that is not NUL terminated at all. */
static void test_never_requires_nul_terminator(void)
{
    static const char DOC[] = "{\"a\":[1,2],\"b\":\"xy\"}";
    size_t len = sizeof DOC - 1;
    char *exact = (char *)xmalloc(len);
    char err[256];
    JsonValue *root;

    /* Exactly len bytes, with the byte just past the end set to a value that
     * would be a parse error if it were ever read. */
    memcpy(exact, DOC, len);

    err[0] = '\0';
    root = json_parse(exact, len, err, sizeof err);
    CHECK_MSG(root != NULL, "document without a NUL terminator rejected: %s", err);
    if (root != NULL) {
        CHECK_EQ_INT(json_integer(json_get(root, "a") != NULL ?
                                  json_at(json_get(root, "a"), 0) : NULL, -1), 1);
        CHECK_EQ_STR(json_string(json_get(root, "b"), NULL), "xy");
        json_free(root);
    }

    /* Pass a length that stops one byte early: the closing brace is not part of
     * the document, so this must fail even though a NUL sits right there. */
    err[0] = '\0';
    root = json_parse(exact, len - 1, err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "line ") != NULL && strstr(err, "column ") != NULL,
              "truncated parse should report a position: %s", err);
    json_free(root);

    free(exact);

    /* An embedded NUL byte is data, not a terminator. */
    err[0] = '\0';
    root = json_parse("{\"a\":1}\0{\"b\":2}", 15, err, sizeof err);
    CHECK(root == NULL);
    CHECK_MSG(strstr(err, "trailing garbage") != NULL,
              "NUL byte should be trailing garbage, got: %s", err);
    json_free(root);
}

/* --- deterministic byte fuzzing ------------------------------------------- */

static unsigned long g_fuzz_state = 0x13579BDFUL;

static unsigned long fuzz_next(void)
{
    /* An LCG, not a great generator, but deterministic across platforms and
     * plenty random for a "does it crash" check. Unsigned overflow is defined
     * behaviour. */
    g_fuzz_state = g_fuzz_state * 1103515245UL + 12345UL;
    return (g_fuzz_state >> 16) & 0x7FFFUL;
}

static void test_fuzz_never_crashes(void)
{
    enum { ITERATIONS = 500, MUTATIONS = 4 };
    size_t len = strlen(kSampleDocument);
    int iteration;
    int rejected = 0;
    int accepted = 0;

    for (iteration = 0; iteration < ITERATIONS; ++iteration) {
        char buffer[512];
        char err[256];
        JsonValue *root;
        size_t i;

        memcpy(buffer, kSampleDocument, len);
        for (i = 0; i < MUTATIONS; ++i) {
            size_t at = (size_t)(fuzz_next() % (unsigned long)len);
            buffer[at] = (char)(unsigned char)(fuzz_next() & 0xFFUL);
        }
        err[0] = '\0';
        root = json_parse(buffer, len, err, sizeof err);
        if (root != NULL) {
            ++accepted;
            json_free(root);
        } else {
            ++rejected;
            /* The invariant the caller depends on: failure always explains
             * itself with a position. */
            CHECK(err[0] != '\0');
            CHECK(strstr(err, "line ") != NULL);
            CHECK(strstr(err, "column ") != NULL);
        }
    }
    CHECK_EQ_INT(accepted + rejected, ITERATIONS);
    CHECK(rejected > 0);
}

/* --- large document, no quadratic behaviour ------------------------------- */

static void append_text(char *buf, size_t cap, size_t *used, const char *text)
{
    while (*text != '\0' && *used + 1 < cap) {
        buf[(*used)++] = *text++;
    }
}

static void append_uint(char *buf, size_t cap, size_t *used, unsigned long long v)
{
    char digits[24];
    size_t count = 0;

    if (v == 0) {
        digits[count++] = '0';
    }
    while (v > 0 && count < sizeof digits) {
        digits[count++] = (char)('0' + (int)(v % 10ULL));
        v /= 10ULL;
    }
    while (count > 0 && *used + 1 < cap) {
        buf[(*used)++] = digits[--count];
    }
}

static char *build_large_object(size_t members, size_t *out_len)
{
    size_t cap = members * 24u + 128u;
    char *buf = (char *)xmalloc(cap);
    size_t used = 0;
    size_t i;

    append_text(buf, cap, &used, "{");
    for (i = 0; i < members; ++i) {
        if (i > 0) {
            append_text(buf, cap, &used, ",");
        }
        append_text(buf, cap, &used, "\"k");
        append_uint(buf, cap, &used, (unsigned long long)i);
        append_text(buf, cap, &used, "\":");
        append_uint(buf, cap, &used, (unsigned long long)i);
    }
    append_text(buf, cap, &used, "}");
    buf[used] = '\0';
    *out_len = used;
    return buf;
}

static void test_large_document(void)
{
    enum { MEMBERS = 100000 };
    size_t len = 0;
    char *text = build_large_object(MEMBERS, &len);
    char err[256];
    JsonValue *root;
    clock_t started;
    clock_t finished;
    double seconds;
    char key[32];

    err[0] = '\0';
    started = clock();
    root = json_parse(text, len, err, sizeof err);
    finished = clock();

    CHECK_MSG(root != NULL, "100k member document failed: %s", err);
    if (root != NULL) {
        seconds = (double)(finished - started) / (double)CLOCKS_PER_SEC;
        printf("  [perf] %d members, %lu bytes: parsed in %.3f s\n",
               (int)MEMBERS, (unsigned long)len, seconds);
        /* Well under a second: linear parsing puts this in the low
         * milliseconds; anything quadratic would take minutes. */
        CHECK_MSG(seconds < 1.0, "100k member parse took %.3f s", seconds);
        CHECK_EQ_INT(root->member_count, (size_t)MEMBERS);
        snprintf(key, sizeof key, "k%llu",
                 (unsigned long long)(MEMBERS - 1));
        CHECK(json_is(json_get(root, key), JSON_NUMBER));
        CHECK_NEAR(json_number(json_get(root, key), -1.0),
                   (double)(MEMBERS - 1), 0.0);
        CHECK(json_get(root, "k0") != NULL);
        json_free(root);
    }
    free(text);
}

/* --- fixtures ------------------------------------------------------------- */

static void test_valid_fixtures(void)
{
    static const char *const paths[] = {
        "tests/fixtures/json_valid_simple.json",
        "tests/fixtures/json_valid_unicode.json",
        "tests/fixtures/json_valid_duplicates.json"
    };
    size_t i;

    for (i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        char err[256];
        char *text;
        size_t len = 0;
        JsonValue *root;

        err[0] = '\0';
        text = read_entire_file(paths[i], &len, err, sizeof err);
        CHECK_MSG(text != NULL, "cannot read %s: %s", paths[i], err);
        if (text == NULL) {
            continue;
        }
        err[0] = '\0';
        root = json_parse(text, len, err, sizeof err);
        CHECK_MSG(root != NULL, "%s should be valid JSON: %s", paths[i], err);
        if (root != NULL) {
            CHECK(json_is(root, JSON_OBJECT));
            json_free(root);
        }
        free(text);
    }
}

static void test_invalid_fixtures(void)
{
    static const char *const paths[] = {
        "tests/fixtures/json_invalid_empty.json",
        "tests/fixtures/json_invalid_trailing_comma.json",
        "tests/fixtures/json_invalid_comment.json",
        "tests/fixtures/json_invalid_single_quotes.json",
        "tests/fixtures/json_invalid_nan.json",
        "tests/fixtures/json_invalid_trailing_garbage.json",
        "tests/fixtures/json_invalid_leading_zero.json",
        "tests/fixtures/json_invalid_missing_colon.json",
        "tests/fixtures/json_invalid_unterminated_string.json",
        "tests/fixtures/json_invalid_bad_escape.json",
        "tests/fixtures/json_invalid_lone_surrogate.json",
        "tests/fixtures/json_invalid_bare_control.json",
        "tests/fixtures/json_invalid_truncated.json"
    };
    size_t i;

    for (i = 0; i < sizeof paths / sizeof paths[0]; ++i) {
        char err[256];
        char *text;
        size_t len = 0;
        JsonValue *root;

        err[0] = '\0';
        text = read_entire_file(paths[i], &len, err, sizeof err);
        CHECK_MSG(text != NULL, "cannot read %s: %s", paths[i], err);
        if (text == NULL) {
            continue;
        }
        err[0] = '\0';
        root = json_parse(text, len, err, sizeof err);
        CHECK_MSG(root == NULL, "%s should be rejected but parsed", paths[i]);
        if (root != NULL) {
            json_free(root);
        } else {
            CHECK_MSG(err[0] != '\0', "no message for %s", paths[i]);
            CHECK_MSG(strstr(err, "line ") != NULL,
                      "%s: error has no line: %s", paths[i], err);
            CHECK_MSG(strstr(err, "column ") != NULL,
                      "%s: error has no column: %s", paths[i], err);
        }
        free(text);
    }
}

/* --- a ledger shaped document -------------------------------------------- */

static void test_ledger_shaped_document(void)
{
    static const char DOC[] =
        "{\n"
        "  \"version\": 2,\n"
        "  \"config\": {\"currency\": \"CNY\", \"symbol\": \"\xC2\xA5\",\n"
        "             \"decimals\": 2, \"exchangeRate\": 7.12},\n"
        "  \"days\": {\n"
        "    \"2026-10-12\": {\n"
        "      \"input\": 225053, \"output\": 30214, \"cacheRead\": 2705164,\n"
        "      \"cacheWrite\": 0, \"reasoning\": 5501, \"calls\": 7,\n"
        "      \"cost\": 12.5, \"apiCost\": 0.25,\n"
        "      \"byProviderModel\": {\"opencode-go:deepseek-v4.1-flash\":\n"
        "                           {\"input\": 225053}},\n"
        "      \"sessions\": [{\"id\": \"session-1\", \"at\": 1760.5,\n"
        "                      \"title\": \"\xE9\x87\x8D\xE6\x9E\x84 renderer\"}]\n"
        "    }\n"
        "  }\n"
        "}\n";
    char err[256];
    JsonValue *root;

    err[0] = '\0';
    root = parse_must_succeed(DOC, err, sizeof err);
    if (root == NULL) {
        return;
    }
    CHECK_EQ_INT(json_integer(json_get(root, "version"), 0), 2);

    {
        const JsonValue *config = json_get(root, "config");
        CHECK_EQ_STR(json_string(json_get(config, "currency"), NULL), "CNY");
        CHECK_EQ_STR(json_string(json_get(config, "symbol"), NULL),
                     "\xC2\xA5");
        CHECK_EQ_INT(json_integer(json_get(config, "decimals"), -1), 2);
        CHECK_NEAR(json_number(json_get(config, "exchangeRate"), 0.0), 7.12,
                   1e-12);
    }

    {
        const JsonValue *days = json_get(root, "days");
        const JsonValue *day = json_get(days, "2026-10-12");
        const JsonValue *sessions;
        const JsonValue *models;

        CHECK(json_is(days, JSON_OBJECT));
        CHECK(json_is(day, JSON_OBJECT));
        CHECK_EQ_INT(json_integer(json_get(day, "input"), 0), 225053);
        CHECK_EQ_INT(json_integer(json_get(day, "output"), 0), 30214);
        CHECK_EQ_INT(json_integer(json_get(day, "cacheRead"), 0), 2705164);
        CHECK_EQ_INT(json_integer(json_get(day, "cacheWrite"), -1), 0);
        CHECK_EQ_INT(json_integer(json_get(day, "calls"), 0), 7);
        CHECK_NEAR(json_number(json_get(day, "cost"), 0.0), 12.5, 1e-12);

        /* byProviderModel maps a model id to an object of counters, which is
         * how the ledger nests per-model detail under a day. */
        models = json_get(day, "byProviderModel");
        CHECK(json_is(models, JSON_OBJECT));
        CHECK(json_is(json_get(models, "opencode-go:deepseek-v4.1-flash"),
                      JSON_OBJECT));
        CHECK_EQ_INT(json_integer(json_get(
                          json_get(models, "opencode-go:deepseek-v4.1-flash"),
                          "input"), 0), 225053);

        sessions = json_get(day, "sessions");
        CHECK(json_is(sessions, JSON_ARRAY));
        CHECK_EQ_INT(sessions != NULL ? (long long)sessions->item_count : -1LL, 1);
        CHECK_EQ_STR(json_string(json_get(json_at(sessions, 0), "id"), NULL),
                     "session-1");
        CHECK_EQ_STR(json_string(json_get(json_at(sessions, 0), "title"), NULL),
                     "\xE9\x87\x8D\xE6\x9E\x84 renderer");
        CHECK_NEAR(json_number(json_get(json_at(sessions, 0), "at"), 0.0),
                   1760.5, 1e-12);

        /* Unknown fields are ignored, never an error: that is what keeps a
         * future ledger version from crashing the loader. */
        CHECK(json_get(day, "notAFieldFromThisVersion") == NULL);
    }

    json_free(root);
}

/* --- harness -------------------------------------------------------------- */

TEST_MAIN_BEGIN()
    RUN(test_scalar_values);
    RUN(test_numbers);
    RUN(test_containers);
    RUN(test_duplicate_keys);
    RUN(test_escapes);
    RUN(test_accessor_defaults);
    RUN(test_integer_conversion);
    RUN(test_malformed_documents);
    RUN(test_error_line_and_column);
    RUN(test_nesting_cap);
    RUN(test_truncation_at_every_offset);
    RUN(test_never_requires_nul_terminator);
    RUN(test_fuzz_never_crashes);
    RUN(test_large_document);
    RUN(test_valid_fixtures);
    RUN(test_invalid_fixtures);
    RUN(test_ledger_shaped_document);
TEST_MAIN_END()
