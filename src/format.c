/* format.c — number and column formatting.
 *
 * The tool prints token counts that reach the billions, so every table has to
 * line up and every number has to be readable at a glance. Two rules keep the
 * output stable: digits are grouped with ',' regardless of the user's locale
 * (setlocale is never called), and a number is always rounded the same way
 * before it is grouped, so the columns cannot disagree by one.
 */

#include "dshtok/format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Scratch size for a fully rendered number: the widest double is 309 integer
 * digits, plus a sign, plus up to six decimals. 512 is comfortably past that. */
#define NUMBER_SCRATCH 512

static void copy_bounded(char *dst, size_t dstsz, const char *src)
{
    size_t i;
    if (dst == NULL || dstsz == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < dstsz && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Append src to whatever dst already holds, truncating at dstsz - 1 instead of
 * running past it. strncat would do the same job but its bounds arithmetic makes
 * gcc's fortified build warn about a possible truncation on every call, so the
 * copy is spelled out. */
static void append_bounded(char *dst, size_t dstsz, const char *src)
{
    size_t used;
    size_t n;

    if (dst == NULL || dstsz == 0 || src == NULL) {
        return;
    }
    used = strlen(dst);
    if (used + 1 >= dstsz) {
        return;
    }
    n = strlen(src);
    if (n > dstsz - used - 1) {
        n = dstsz - used - 1;
    }
    memcpy(dst + used, src, n);
    dst[used + n] = '\0';
}

/* Insert a ',' between every three digits of an already sign-free integer string.
 * The result is written to out, truncated safely when it does not fit. */
static void group_digits(const char *digits, char *out, size_t outsz)
{
    size_t len = strlen(digits);
    size_t commas;
    size_t i;
    size_t src = 0;
    size_t dst = 0;
    size_t first;

    if (out == NULL || outsz == 0) {
        return;
    }
    if (len == 0) {
        out[0] = '\0';
        return;
    }

    commas = (len - 1) / 3;
    if (len + commas + 1 > outsz) {
        /* Not enough room for the readable form: fall back to the raw digits,
         * which at least stays parseable. */
        copy_bounded(out, outsz, digits);
        return;
    }

    first = len % 3;
    if (first == 0) {
        first = 3;
    }
    for (i = 0; i < first; ++i) {
        out[dst++] = digits[src++];
    }
    while (src < len) {
        out[dst++] = ',';
        for (i = 0; i < 3; ++i) {
            out[dst++] = digits[src++];
        }
    }
    out[dst] = '\0';
}

/* Render value with `decimals` fractional digits, grouping the integer part.
 * Value is used as given: the caller has already decided the sign treatment. */
static void format_fixed_grouped(double value, int decimals,
                                 char *buf, size_t bufsz)
{
    char raw[NUMBER_SCRATCH];
    char grouped[NUMBER_SCRATCH + 128];
    const char *dot;
    size_t int_len;
    int negative;

    if (buf == NULL || bufsz == 0) {
        return;
    }
    if (!isfinite(value)) {
        copy_bounded(buf, bufsz, "n/a");
        return;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 6) {
        decimals = 6;
    }

    /* signbit also catches the -0.0 that rounding produces (ceil(-0.4 - 0.5)
     * is -0.0), which value < 0.0 alone would miss. */
    negative = signbit(value) != 0;
    /* printf renders a negative zero as "-0"; the ledger never has one and a
     * stray -0 is noise, so format the magnitude and add the sign ourselves. */
    snprintf(raw, sizeof raw, "%.*f", decimals, negative ? -value : value);
    if (negative) {
        /* Rounding can collapse a small negative value to zero (-0.4 at zero
         * decimals). A "-0" column reads like a real negative count, so drop the
         * sign whenever the rendered magnitude is all zeros. */
        const char *p = raw;
        int all_zero = 1;
        for (; *p != '\0'; ++p) {
            if (*p != '0' && *p != '.') {
                all_zero = 0;
                break;
            }
        }
        if (all_zero) {
            negative = 0;
        }
    }

    dot = strchr(raw, '.');
    int_len = (dot != NULL) ? (size_t)(dot - raw) : strlen(raw);

    grouped[0] = '\0';
    if (int_len > 0) {
        char int_part[NUMBER_SCRATCH];
        if (int_len >= sizeof int_part) {
            copy_bounded(buf, bufsz, raw);
            return;
        }
        memcpy(int_part, raw, int_len);
        int_part[int_len] = '\0';
        if (negative) {
            snprintf(grouped, sizeof grouped, "-");
        }
        {
            char g[NUMBER_SCRATCH + 128];
            group_digits(int_part, g, sizeof g);
            append_bounded(grouped, sizeof grouped, g);
        }
        if (dot != NULL) {
            append_bounded(grouped, sizeof grouped, dot);
        }
    } else {
        copy_bounded(grouped, sizeof grouped, raw);
    }

    copy_bounded(buf, bufsz, grouped);
}

char *fmt_grouped(double value, char *buf, size_t bufsz)
{
    double rounded;

    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (!isfinite(value)) {
        copy_bounded(buf, bufsz, "n/a");
        return buf;
    }
    /* Round half away from zero. printf would round half to even, which makes a
     * column look arbitrary when the same total is shown rounded and unrounded. */
    if (value >= 0.0) {
        rounded = floor(value + 0.5);
    } else {
        rounded = ceil(value - 0.5);
    }
    format_fixed_grouped(rounded, 0, buf, bufsz);
    return buf;
}

char *fmt_tokens(double value, char *buf, size_t bufsz)
{
    if (!isfinite(value)) {
        if (buf != NULL && bufsz > 0) {
            copy_bounded(buf, bufsz, "n/a");
        }
        return buf;
    }
    if (value < 0.0) {
        /* Token counters are monotonic; a negative is corruption, not data, and
         * printing -1 in a table of billions only spreads confusion. */
        value = 0.0;
    }
    return fmt_grouped(value, buf, bufsz);
}

char *fmt_compact(double value, char *buf, size_t bufsz)
{
    static const char *const suffixes[] = {"", "K", "M", "B", "T", "P", "E"};
    const int suffix_count = (int)(sizeof suffixes / sizeof suffixes[0]);

    char raw[NUMBER_SCRATCH];
    double magnitude;
    int k;
    int negative;
    int decimals;
    double scaled;

    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (!isfinite(value)) {
        copy_bounded(buf, bufsz, "n/a");
        return buf;
    }

    negative = (value < 0.0);
    magnitude = negative ? -value : value;

    if (magnitude < 1000.0) {
        /* Exact, with trailing zeros trimmed: 999 -> "999", 999.6 -> "999.6". */
        size_t n;
        snprintf(raw, sizeof raw, "%.2f", magnitude);
        n = strlen(raw);
        while (n > 0 && raw[n - 1] == '0') {
            raw[--n] = '\0';
        }
        if (n > 0 && raw[n - 1] == '.') {
            raw[--n] = '\0';
        }
        if (n == 0) {
            copy_bounded(raw, sizeof raw, "0");
        }
        if (negative) {
            char signed_buf[NUMBER_SCRATCH + 2];
            snprintf(signed_buf, sizeof signed_buf, "-%s", raw);
            copy_bounded(buf, bufsz, signed_buf);
        } else {
            copy_bounded(buf, bufsz, raw);
        }
        return buf;
    }

    k = 1;
    scaled = magnitude / 1000.0;
    decimals = 2;
    for (;;) {
        double limit;
        decimals = (k <= 3) ? 2 : 1;
        limit = 1000.0;
        /* Promote when rounding to the unit's precision would reach 1000, so the
         * output never says "1000.00K" where it could say "1.00M". */
        if (scaled < limit) {
            double factor = (decimals == 2) ? 100.0 : 10.0;
            double rounded = floor(scaled * factor + 0.5) / factor;
            if (rounded < limit) {
                break;
            }
        }
        if (k + 1 >= suffix_count) {
            break;
        }
        k += 1;
        scaled = magnitude / pow(1000.0, (double)k);
    }

    snprintf(raw, sizeof raw, "%.*f%s", decimals, scaled, suffixes[k]);
    if (negative) {
        char signed_buf[NUMBER_SCRATCH + 2];
        snprintf(signed_buf, sizeof signed_buf, "-%s", raw);
        copy_bounded(buf, bufsz, signed_buf);
    } else {
        copy_bounded(buf, bufsz, raw);
    }
    return buf;
}

char *fmt_percent(double part, double whole, char *buf, size_t bufsz)
{
    double pct;

    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (!isfinite(part) || !isfinite(whole) || whole == 0.0) {
        copy_bounded(buf, bufsz, "0.0%");
        return buf;
    }
    pct = (part / whole) * 100.0;
    snprintf(buf, bufsz, "%.1f%%", pct);
    return buf;
}

char *fmt_money(double value, const char *currency, int decimals,
                char *buf, size_t bufsz)
{
    char number[NUMBER_SCRATCH + 128];

    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 6) {
        decimals = 6;
    }
    format_fixed_grouped(value, decimals, number, sizeof number);
    if (currency == NULL || currency[0] == '\0') {
        snprintf(buf, bufsz, "%s", number);
    } else {
        snprintf(buf, bufsz, "%s %s", currency, number);
    }
    return buf;
}

char *fmt_money_symbol(double value, const char *symbol, const char *currency,
                       int decimals, char *buf, size_t bufsz)
{
    if (symbol == NULL || symbol[0] == '\0') {
        return fmt_money(value, currency, decimals, buf, bufsz);
    }
    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (decimals < 0) {
        decimals = 0;
    }
    if (decimals > 6) {
        decimals = 6;
    }
    {
        char number[NUMBER_SCRATCH + 128];
        format_fixed_grouped(value, decimals, number, sizeof number);
        snprintf(buf, bufsz, "%s%s", symbol, number);
    }
    return buf;
}

char *fmt_pad_left(const char *s, int width, char *buf, size_t bufsz)
{
    size_t len;
    int pad;
    size_t i;

    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (s == NULL) {
        s = "";
    }
    len = strlen(s);
    if (width < 0) {
        width = 0;
    }
    pad = (int)len < width ? width - (int)len : 0;
    if ((size_t)pad + len + 1 > bufsz) {
        /* No room for the padding: emit the string without it. */
        copy_bounded(buf, bufsz, s);
        return buf;
    }
    for (i = 0; i < (size_t)pad; ++i) {
        buf[i] = ' ';
    }
    memcpy(buf + pad, s, len);
    buf[pad + len] = '\0';
    return buf;
}

char *fmt_pad_right(const char *s, int width, char *buf, size_t bufsz)
{
    size_t len;
    size_t pad;
    size_t i;

    if (buf == NULL || bufsz == 0) {
        return buf;
    }
    if (s == NULL) {
        s = "";
    }
    len = strlen(s);
    if (width < 0) {
        width = 0;
    }
    pad = (size_t)width > len ? (size_t)width - len : 0;
    if (len + pad + 1 > bufsz) {
        copy_bounded(buf, bufsz, s);
        return buf;
    }
    memcpy(buf, s, len);
    for (i = 0; i < pad; ++i) {
        buf[len + i] = ' ';
    }
    buf[len + pad] = '\0';
    return buf;
}

char *fmt_truncate(const char *src, size_t width, char *dst, size_t dstsz)
{
    size_t len;
    size_t cut;

    if (dst == NULL || dstsz == 0) {
        return dst;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return dst;
    }
    len = strlen(src);
    cut = (len + 1 <= dstsz) ? len : dstsz - 1;
    if (cut > width) {
        cut = width;
    }
    if (cut > len) {
        cut = len;
    }
    memcpy(dst, src, cut);
    dst[cut] = '\0';

    /* Refuse to end in the middle of a multi-byte sequence: back up over the
     * continuation bytes, look at the lead byte, and drop the whole glyph when
     * it does not fit. A terminal that receives half a glyph prints garbage. */
    {
        size_t i = cut;
        size_t need = 1;
        unsigned char lead;

        while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) {
            --i;
        }
        if (i == 0) {
            return dst;
        }
        lead = (unsigned char)dst[i - 1];
        if ((lead & 0x80) == 0x00) {
            need = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            need = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            need = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            need = 4;
        } else {
            need = 1; /* malformed byte: leave it, truncating cannot help */
        }
        if (i - 1 + need > cut) {
            /* Only drop the glyph when truncation is what split it. If the
             * source itself stops mid-sequence the bytes are a faithful prefix
             * of the input, and removing them would be surprising. */
            if (i - 1 + need <= len) {
                dst[i - 1] = '\0';
            }
        }
    }
    return dst;
}
