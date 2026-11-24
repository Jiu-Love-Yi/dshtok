/* test_format.c - the number and column formatters.
 *
 * Two properties are tested for every formatter: the exact rendering the header
 * promises, and the buffer contract (always NUL terminated, never a byte past
 * bufsz). The buffer cases run each function through an 8 byte buffer flanked by
 * a canary, and through a zero length buffer that must not be written at all.
 */

#include "mini_test.h"

#include "dshtok/format.h"

#include <string.h>

#define BIG 640

/* An 8 byte destination with a canary on each side. The three members are char
 * arrays, so there is no padding between them and a stray write lands on a
 * canary. */
typedef struct {
    unsigned char lead[8];
    char buf[8];
    unsigned char tail[8];
} Small;

#define SMALL_FILL 0xA5

static void small_init(Small *s)
{
    memset(s->lead, SMALL_FILL, sizeof s->lead);
    memset(s->buf, SMALL_FILL, sizeof s->buf);
    memset(s->tail, SMALL_FILL, sizeof s->tail);
}

/* Neither canary was disturbed. */
static int small_canary_ok(const Small *s)
{
    size_t i;
    for (i = 0; i < sizeof s->lead; ++i) {
        if (s->lead[i] != SMALL_FILL) {
            return 0;
        }
    }
    for (i = 0; i < sizeof s->tail; ++i) {
        if (s->tail[i] != SMALL_FILL) {
            return 0;
        }
    }
    return 1;
}

/* Canaries untouched and the destination holds a NUL terminated string. */
static int small_ok(const Small *s)
{
    return small_canary_ok(s) &&
           memchr(s->buf, '\0', sizeof s->buf) != NULL;
}

/* Canaries untouched and buf itself not written either. */
static int small_untouched(const Small *s)
{
    return small_canary_ok(s) &&
           s->buf[0] == (char)SMALL_FILL;
}

static void t_grouped(void)
{
    char buf[BIG];

    fmt_grouped(0.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0");
    fmt_grouped(7.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "7");
    fmt_grouped(999.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "999");
    fmt_grouped(1000.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1,000");
    fmt_grouped(1234567.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1,234,567");
    fmt_grouped(5167155497.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "5,167,155,497");
    fmt_grouped(1e12, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1,000,000,000,000");

    fmt_grouped(-1234.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-1,234");
    fmt_grouped(-1234.6, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-1,235");

    /* Rounding is half away from zero, in both directions. */
    fmt_grouped(2.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "3");
    fmt_grouped(3.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "4");
    fmt_grouped(-2.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-3");
    fmt_grouped(0.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1");

    fmt_grouped(NAN, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");
    fmt_grouped(INFINITY, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");
    fmt_grouped(-INFINITY, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");

    /* The return value is always the buffer it was given. */
    CHECK(fmt_grouped(1.0, buf, sizeof buf) == buf);
}

/* Rounding must not resurrect a negative zero: "-0" in a token column reads like
 * a real (tiny) negative count. */
static void t_grouped_no_negative_zero(void)
{
    char buf[BIG];

    fmt_grouped(-0.4, buf, sizeof buf);
    CHECK_MSG(strcmp(buf, "0") == 0, "rendered \"%s\", want \"0\"", buf);
    fmt_grouped(-0.0, buf, sizeof buf);
    CHECK_MSG(strcmp(buf, "0") == 0, "rendered \"%s\", want \"0\"", buf);
}

/* A double this large is still finite: 1e300 has 301 integer digits plus 100
 * grouping commas, which is the widest realistic table entry. */
static void t_grouped_huge(void)
{
    char buf[BIG];
    size_t i;
    size_t len;
    int only_digits_and_commas = 1;

    fmt_grouped(1e300, buf, sizeof buf);
    len = strlen(buf);
    CHECK_EQ_INT(len, 401);
    for (i = 0; i < len; ++i) {
        if (buf[i] != ',' && (buf[i] < '0' || buf[i] > '9')) {
            only_digits_and_commas = 0;
        }
    }
    CHECK(only_digits_and_commas);
    CHECK_EQ_INT(buf[0], '1');
    CHECK_EQ_INT(buf[1], ',');
}
static void t_tokens(void)
{
    char buf[BIG];

    fmt_tokens(0.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0");
    /* Token counters are monotonic: a negative is corruption and must not be
     * shown as a negative count. */
    fmt_tokens(-5.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0");
    fmt_tokens(-0.4, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0");
    fmt_tokens(0.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1");
    fmt_tokens(5167155497.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "5,167,155,497");
    fmt_tokens(NAN, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");
    fmt_tokens(INFINITY, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");
    CHECK(fmt_tokens(1.0, buf, sizeof buf) == buf);
}

static void t_compact(void)
{
    char buf[BIG];

    fmt_compact(0.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0");
    fmt_compact(0.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.5");
    fmt_compact(12.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "12");
    fmt_compact(999.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "999");
    fmt_compact(999.99, buf, sizeof buf);
    CHECK_EQ_STR(buf, "999.99");

    /* K with two decimals. */
    fmt_compact(1000.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.00K");
    fmt_compact(1234.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.23K");
    fmt_compact(12000.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "12.00K");

    /* 9999 rounds to 10.00K at two decimals; 10000 is the same value, so the
     * pair must not disagree. */
    fmt_compact(9999.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "10.00K");
    fmt_compact(10000.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "10.00K");

    /* Promotion happens as soon as rounding would reach 1000 of the unit. */
    fmt_compact(999499.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "999.50K");
    fmt_compact(999999.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.00M");

    fmt_compact(1000000.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.00M");
    fmt_compact(1234567.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.23M");
    fmt_compact(1e9, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.00B");
    fmt_compact(5167155497.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "5.17B");

    /* T carries one decimal, not two. */
    fmt_compact(1e12, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.0T");
    fmt_compact(1.2e13, buf, sizeof buf);
    CHECK_EQ_STR(buf, "12.0T");
    fmt_compact(999999999999999.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1.0P");

    /* The sign survives. */
    fmt_compact(-999.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-999");
    fmt_compact(-1234.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-1.23K");
    fmt_compact(-0.5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "-0.5");

    fmt_compact(NAN, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");
    fmt_compact(INFINITY, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");
    fmt_compact(-INFINITY, buf, sizeof buf);
    CHECK_EQ_STR(buf, "n/a");

    CHECK(fmt_compact(1.0, buf, sizeof buf) == buf);
}

/* Above T the header leaves the unit unspecified; the only hard requirements are
 * that the result is bounded and terminated. */
static void t_compact_huge(void)
{
    char buf[BIG];

    fmt_compact(1.7976931348623157e308, buf, sizeof buf);
    CHECK(strlen(buf) > 0);
    CHECK(strlen(buf) < BIG);
    CHECK(strlen(buf) <= sizeof buf - 1);
}

static void t_percent(void)
{
    char buf[BIG];

    fmt_percent(4966453816.0, 5167155497.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "96.1%");
    fmt_percent(0.0, 100.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");
    fmt_percent(100.0, 100.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "100.0%");
    fmt_percent(1.0, 3.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "33.3%");
    fmt_percent(2.0, 3.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "66.7%");

    /* A zero denominator is the empty-ledger case, not a crash. */
    fmt_percent(0.0, 0.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");
    fmt_percent(5.0, 0.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");

    /* Non-finite either side also degrades to 0.0%. */
    fmt_percent(NAN, 100.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");
    fmt_percent(1.0, NAN, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");
    fmt_percent(1.0, INFINITY, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");
    fmt_percent(INFINITY, 1.0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "0.0%");

    CHECK(fmt_percent(1.0, 2.0, buf, sizeof buf) == buf);
    CHECK_NEAR(atof("50.0%"), 50.0, 1e-9);
}

static void t_money(void)
{
    char buf[BIG];
    char long_code[32];

    /* The example from format.h. */
    fmt_money(1760.4831, "CNY", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.48");

    fmt_money(1760.4831, "CNY", 0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760");
    fmt_money(1760.4831, "CNY", 1, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.5");
    fmt_money(1760.4831, "CNY", 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.483");
    fmt_money(1760.4831, "CNY", 4, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.4831");

    /* decimals is clamped to 0..6. */
    fmt_money(1760.4831, "CNY", -3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760");
    fmt_money(1760.4831, "CNY", 9, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.483100");

    /* No currency, no prefix. */
    fmt_money(1760.4831, NULL, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1,760.48");
    fmt_money(1760.4831, "", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "1,760.48");

    /* A currency code longer than the usual three characters is not truncated
     * and does not break the grouping. */
    strcpy(long_code, "ABCDEFGHIJKLMNOPQRSTUVWXYZ");
    fmt_money(1760.4831, long_code, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "ABCDEFGHIJKLMNOPQRSTUVWXYZ 1,760.48");

    fmt_money(0.0, "CNY", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 0.00");
    fmt_money(-1760.4831, "CNY", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY -1,760.48");

    CHECK(fmt_money(1.0, "CNY", 2, buf, sizeof buf) == buf);
}

static void t_money_symbol(void)
{
    char buf[BIG];

    /* U+00A5 YEN SIGN in UTF-8, as the ledger stores it. */
    fmt_money_symbol(1760.4831, "\xC2\xA5", "CNY", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "\xC2\xA5" "1,760.48");
    fmt_money_symbol(1760.4831, NULL, "CNY", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.48");
    fmt_money_symbol(1760.4831, "", "CNY", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "CNY 1,760.48");
    fmt_money_symbol(0.5, "$", NULL, 1, buf, sizeof buf);
    CHECK_EQ_STR(buf, "$0.5");
    CHECK(fmt_money_symbol(1.0, "$", "USD", 2, buf, sizeof buf) == buf);
}

static void t_pad_left(void)
{
    char buf[BIG];

    fmt_pad_left("7", 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "  7");
    fmt_pad_left("abc", 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    /* Wider than the column: copied unchanged, no truncation. */
    fmt_pad_left("abc", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    fmt_pad_left("abc", 0, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    fmt_pad_left("abc", -5, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    /* Padding an empty string is all padding. */
    fmt_pad_left("", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "  ");
    fmt_pad_left(NULL, 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "   ");
    CHECK(fmt_pad_left("x", 2, buf, sizeof buf) == buf);
}

static void t_pad_right(void)
{
    char buf[BIG];

    fmt_pad_right("7", 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "7  ");
    fmt_pad_right("abc", 3, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    fmt_pad_right("abc", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    fmt_pad_right("", 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "  ");
    fmt_pad_right(NULL, 2, buf, sizeof buf);
    CHECK_EQ_STR(buf, "  ");
    fmt_pad_right("abc", -1, buf, sizeof buf);
    CHECK_EQ_STR(buf, "abc");
    CHECK(fmt_pad_right("x", 2, buf, sizeof buf) == buf);
}

static void t_truncate(void)
{
    char dst[BIG];
    /* "zhong gou" in UTF-8: E9 87 8D E6 9E 84, then " renderer". */
    const char *utf8 = "\xE9\x87\x8D\xE6\x9E\x84" " renderer";

    fmt_truncate("abcdef", 3, dst, sizeof dst);
    CHECK_EQ_STR(dst, "abc");
    fmt_truncate("abcdef", 0, dst, sizeof dst);
    CHECK_EQ_STR(dst, "");
    fmt_truncate("abc", 10, dst, sizeof dst);
    CHECK_EQ_STR(dst, "abc");
    fmt_truncate("abc", 3, dst, sizeof dst);
    CHECK_EQ_STR(dst, "abc");
    fmt_truncate(NULL, 3, dst, sizeof dst);
    CHECK_EQ_STR(dst, "");

    /* A byte budget that lands mid-glyph drops the partial glyph instead of
     * emitting half a character. */
    fmt_truncate(utf8, 3, dst, sizeof dst);
    CHECK_EQ_STR(dst, "\xE9\x87\x8D");
    fmt_truncate(utf8, 4, dst, sizeof dst);
    CHECK_EQ_STR(dst, "\xE9\x87\x8D");
    fmt_truncate(utf8, 5, dst, sizeof dst);
    CHECK_EQ_STR(dst, "\xE9\x87\x8D");
    fmt_truncate(utf8, 6, dst, sizeof dst);
    CHECK_EQ_STR(dst, "\xE9\x87\x8D\xE6\x9E\x84");
    fmt_truncate(utf8, 7, dst, sizeof dst);
    CHECK_EQ_STR(dst, "\xE9\x87\x8D\xE6\x9E\x84" " ");

    /* dstsz is a harder limit than width: the glyph needs three bytes plus the
     * terminator, so a 4 byte destination fits it and a 3 byte one does not. */
    fmt_truncate(utf8, 100, dst, 4);
    CHECK_EQ_STR(dst, "\xE9\x87\x8D");
    fmt_truncate(utf8, 100, dst, 3);
    CHECK_EQ_STR(dst, "");

    /* Two byte sequence. */
    fmt_truncate("\xC3\xA9" "x", 1, dst, sizeof dst);
    CHECK_EQ_STR(dst, "");
    fmt_truncate("\xC3\xA9" "x", 2, dst, sizeof dst);
    CHECK_EQ_STR(dst, "\xC3\xA9");

    /* A source that is itself a lone lead byte, with a budget that covers it,
     * is not "split" by truncation, so the only portable guarantee is that the
     * destination holds a NUL terminated prefix of the source. */
    fmt_truncate("\xE9", 1, dst, sizeof dst);
    CHECK_EQ_INT(strncmp(dst, "\xE9", 1), 0);

    CHECK(fmt_truncate("abc", 10, dst, sizeof dst) == dst);
}

/* Every formatter, given a destination that is too small, must stay inside it. */
static void t_small_buffers(void)
{
    Small s;
    char *ret;

    small_init(&s);
    ret = fmt_grouped(5167155497.0, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_grouped wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_tokens(-5167155497.0, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_tokens wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_compact(5167155497.0, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_compact wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_percent(1.0, 3.0, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_percent wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_money(1760.4831, "CNY", 2, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_money wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_money_symbol(1760.4831, "\xC2\xA5", "CNY", 2, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_money_symbol wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_pad_left("x", 40, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_pad_left wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_pad_right("x", 40, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_pad_right wrote past an 8 byte buffer");

    small_init(&s);
    ret = fmt_truncate("abcdefghijklmnop", 10, s.buf, 8);
    CHECK(ret == s.buf);
    CHECK_MSG(small_ok(&s), "%s", "fmt_truncate wrote past an 8 byte buffer");

    /* A one byte destination can only ever hold the terminator. */
    small_init(&s);
    fmt_grouped(1234.0, s.buf, 1);
    CHECK_MSG(small_ok(&s), "%s", "fmt_grouped wrote past a 1 byte buffer");
    CHECK_EQ_INT(s.buf[0], 0);

    small_init(&s);
    fmt_pad_left("abc", 10, s.buf, 1);
    CHECK_MSG(small_ok(&s), "%s", "fmt_pad_left wrote past a 1 byte buffer");
    CHECK_EQ_INT(s.buf[0], 0);

    small_init(&s);
    fmt_truncate("abc", 10, s.buf, 1);
    CHECK_MSG(small_ok(&s), "%s", "fmt_truncate wrote past a 1 byte buffer");
    CHECK_EQ_INT(s.buf[0], 0);
}

/* A zero length destination must not be written at all. */
static void t_zero_buffers(void)
{
    Small s;

    small_init(&s);
    fmt_grouped(1.0, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_grouped wrote to a 0 byte buffer");

    small_init(&s);
    fmt_tokens(1.0, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_tokens wrote to a 0 byte buffer");

    small_init(&s);
    fmt_compact(1.0, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_compact wrote to a 0 byte buffer");

    small_init(&s);
    fmt_percent(1.0, 2.0, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_percent wrote to a 0 byte buffer");

    small_init(&s);
    fmt_money(1.0, "CNY", 2, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_money wrote to a 0 byte buffer");

    small_init(&s);
    fmt_money_symbol(1.0, "$", "USD", 2, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_money_symbol wrote to a 0 byte buffer");

    small_init(&s);
    fmt_pad_left("abc", 8, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_pad_left wrote to a 0 byte buffer");

    small_init(&s);
    fmt_pad_right("abc", 8, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_pad_right wrote to a 0 byte buffer");

    small_init(&s);
    fmt_truncate("abc", 8, s.buf, 0);
    CHECK_MSG(small_untouched(&s), "%s", "fmt_truncate wrote to a 0 byte buffer");
}

/* A NULL destination is allowed to be a no-op, but must not crash. */
static void t_null_buffers(void)
{
    CHECK(fmt_grouped(1.0, NULL, 32) == NULL);
    CHECK(fmt_tokens(1.0, NULL, 32) == NULL);
    CHECK(fmt_compact(1.0, NULL, 32) == NULL);
    CHECK(fmt_percent(1.0, 2.0, NULL, 32) == NULL);
    CHECK(fmt_money(1.0, "CNY", 2, NULL, 32) == NULL);
    CHECK(fmt_money_symbol(1.0, "$", "USD", 2, NULL, 32) == NULL);
    CHECK(fmt_pad_left("abc", 8, NULL, 32) == NULL);
    CHECK(fmt_pad_right("abc", 8, NULL, 32) == NULL);
    CHECK(fmt_truncate("abc", 8, NULL, 32) == NULL);
}

/* The column widths report.c relies on are part of the contract. */
static void t_column_widths(void)
{
    CHECK_EQ_INT(DSHTOK_LABEL_WIDTH, 21);
    CHECK_EQ_INT(DSHTOK_NUMBER_WIDTH, 17);
}

TEST_MAIN_BEGIN()
    RUN(t_grouped);
    RUN(t_grouped_no_negative_zero);
    RUN(t_grouped_huge);
    RUN(t_tokens);
    RUN(t_compact);
    RUN(t_compact_huge);
    RUN(t_percent);
    RUN(t_money);
    RUN(t_money_symbol);
    RUN(t_pad_left);
    RUN(t_pad_right);
    RUN(t_truncate);
    RUN(t_small_buffers);
    RUN(t_zero_buffers);
    RUN(t_null_buffers);
    RUN(t_column_widths);
TEST_MAIN_END()
