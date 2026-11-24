#ifndef DSHTOK_FORMAT_H
#define DSHTOK_FORMAT_H

#include <stddef.h>

#include "dshtok/ledger.h"

/* Number formatting. Every function writes into a caller supplied buffer, always
 * NUL terminates, and never writes more than bufsz bytes. Each returns buf. */

/* Round to the nearest integer, group the digits with ',' and render.
 *   5167155497 -> "5,167,155,497"
 *   -1234.6    -> "-1,235"
 * Rounding is half away from zero. Non-finite input renders as "n/a". */
char *fmt_grouped(double value, char *buf, size_t bufsz);

/* Token counts are never negative: clamp at 0, round, then group. */
char *fmt_tokens(double value, char *buf, size_t bufsz);

/* Compact human form used for the parenthesised hint next to the grand total.
 * Below 1000 the value is rendered exactly with no suffix; above it one
 * decimal-scaled unit is used, promoted as soon as rounding would reach 1000:
 *   999        -> "999"
 *   1234       -> "1.23K"
 *   1234567    -> "1.23M"
 *   5167155497 -> "5.17B"
 *   1.2e13     -> "12.0T"
 * K and M and B use two decimals, T uses one. Negative input keeps its sign.
 * Non-finite input renders as "n/a". */
char *fmt_compact(double value, char *buf, size_t bufsz);

/* part/whole as a percentage with one decimal: "96.1%". A zero or non-finite
 * whole yields "0.0%". */
char *fmt_percent(double part, double whole, char *buf, size_t bufsz);

/* Currency amount with an explicit precision: fmt_money(1760.4831, "CNY", 2)
 * -> "CNY 1,760.48". decimals is clamped to 0..6. An empty or NULL currency
 * omits the prefix entirely. */
char *fmt_money(double value, const char *currency, int decimals,
                char *buf, size_t bufsz);

/* Same as fmt_money but using the ledger's symbol when it has one, e.g.
 * "¥1,760.4831". Falls back to the ISO code when symbol is NULL or empty. */
char *fmt_money_symbol(double value, const char *symbol, const char *currency,
                       int decimals, char *buf, size_t bufsz);

/* Left pad s with spaces to width columns, writing into buf. A string already
 * wider than width is copied unchanged. When the result does not fit, the string
 * is truncated rather than the padding being dropped — a half padded column is
 * more confusing than a short one. */
char *fmt_pad_left(const char *s, int width, char *buf, size_t bufsz);

/* Space pad on the right, same rules. */
char *fmt_pad_right(const char *s, int width, char *buf, size_t bufsz);

/* Copy at most width bytes of src and never split a multi-byte UTF-8 sequence:
 * a trailing partial sequence is dropped. width is a byte budget, not a column
 * count. */
char *fmt_truncate(const char *src, size_t width, char *dst, size_t dstsz);

/* Column widths shared by report.c and the tests. */
#define DSHTOK_LABEL_WIDTH 21
#define DSHTOK_NUMBER_WIDTH 17

#endif /* DSHTOK_FORMAT_H */
