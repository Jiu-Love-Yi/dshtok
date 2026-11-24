/* report.c -render the reports.
 *
 * Every command funnels through here. Text output is column aligned with fixed
 * widths so the tables stay readable when pasted into a terminal, an issue or a
 * commit message; --json and --csv exist for everything else. The numbers all
 * come from Ledger, which has already merged the per-day records.
 */

/* fileno() and isatty() are POSIX, and -std=c99 hides their declarations on
 * glibc unless this is asked for before the first system header. MinGW declares
 * _fileno/_isatty unconditionally, so the macro is only set where it is needed;
 * defining it there would be pointless and risks changing header selection. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "dshtok/dshtok.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <io.h>
#  define dsh_stdout_is_tty() (_isatty(_fileno(stdout)) != 0)
#else
#  include <unistd.h>
#  define dsh_stdout_is_tty() (isatty(fileno(stdout)) != 0)
#endif

#define INDENT "  "
#define NUMBER_BUF 64
#define BIG_BUF 1024

/* Column layout, shared by the tables and the tests that assert on them. */
#define DAILY_INPUT_W 13
#define DAILY_OUTPUT_W 13
#define DAILY_CREAD_W 15
#define DAILY_CWRITE_W 13
#define DAILY_TOTAL_W 14
#define DAILY_CALLS_W 7
#define MODEL_NAME_W 44
#define MODEL_TOTAL_W 14
#define MODEL_SHARE_W 7
#define SESSION_DATE_W 10
#define SESSION_TOTAL_W 14
#define SESSION_CALLS_W 8

/* --- small helpers -------------------------------------------------------- */

static int use_color(const Options *opt)
{
    if (opt->color == 0) {
        return 0;
    }
    if (opt->color == 2) {
        return 1;
    }
    return dsh_stdout_is_tty();
}

static const char *rule_char(const Options *opt)
{
    return opt->unicode ? "\xe2\x94\x80" : "-";
}

static void print_rule(FILE *out, const Options *opt, int width)
{
    int i;
    if (width <= 0) {
        width = 56;
    }
    fputs(INDENT, out);
    for (i = 0; i < width; ++i) {
        fputs(rule_char(opt), out);
    }
    fputc('\n', out);
}

static void print_indent(FILE *out)
{
    fputs(INDENT, out);
}

/* Emit label padded to DSHTOK_LABEL_WIDTH, value right aligned to
 * DSHTOK_NUMBER_WIDTH, then an optional trailing note. */
static void print_metric(FILE *out, const char *label, const char *value,
                         const char *note)
{
    char padded[BIG_BUF];
    print_indent(out);
    fmt_pad_right(label, DSHTOK_LABEL_WIDTH, padded, sizeof padded);
    fputs(padded, out);
    fmt_pad_left(value, DSHTOK_NUMBER_WIDTH, padded, sizeof padded);
    fputs(padded, out);
    if (note != NULL && note[0] != '\0') {
        fputs("   ", out);
        fputs(note, out);
    }
    fputc('\n', out);
}

static void print_header(FILE *out, const Options *opt, const char *source)
{
    if (opt->no_header) {
        return;
    }
    fprintf(out, "%s %s %s %s\n", DSHTOK_NAME, DSHTOK_VERSION_STRING,
            opt->unicode ? "\xe2\x80\x94" : "-", DSHTOK_TAGLINE);
    if (source != NULL) {
        fprintf(out, "source: %s\n", source);
    }
    fputc('\n', out);
}

/* The ledger keeps a base cost (in the pricing currency) and a converted CNY
 * figure. Which one to show depends on the currency the plugin is configured
 * to display in. */
static double money_value(const Ledger *l, double base, double cny)
{
    if (l != NULL && (strcmp(l->currency, "CNY") == 0 ||
                      strcmp(l->currency, "RMB") == 0)) {
        return cny;
    }
    return base;
}

/* Machine-readable output declares a currency in its own header, so the cost
 * numbers it emits have to be in that currency too. The ledger stores a base
 * (USD) figure alongside pre-converted CNY/EUR ones, so pick the pair that
 * matches config.currency exactly like the text view does -otherwise a JSON
 * consumer sees "CNY" next to a dollar amount. */
static void usage_cost(const Ledger *l, const Usage *u, double *cost,
                       double *api_cost)
{
    *cost = money_value(l, u->cost, u->costCny);
    *api_cost = money_value(l, u->apiCost, u->apiCostCny);
}

static const char *money_prefix(const Options *opt, const Ledger *l)
{
    if (opt->unicode && l != NULL && l->symbol[0] != '\0') {
        return l->symbol;
    }
    if (l != NULL && l->currency[0] != '\0') {
        return l->currency;
    }
    return "";
}

/* Number of decimal digits a rank column needs. */
static int rank_width(size_t count)
{
    int width = 1;
    while (count >= 10) {
        count /= 10;
        ++width;
    }
    return width;
}

static size_t effective_count(size_t count, long top)
{
    if (top > 0 && (size_t)top < count) {
        return (size_t)top;
    }
    return count;
}

/* --- JSON output ---------------------------------------------------------- */

static void json_escape(FILE *out, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    if (s == NULL) {
        fputs("null", out);
        return;
    }
    fputc('"', out);
    for (; *p != 0; ++p) {
        switch (*p) {
        case '"':
            fputs("\\\"", out);
            break;
        case '\\':
            fputs("\\\\", out);
            break;
        case '\b':
            fputs("\\b", out);
            break;
        case '\f':
            fputs("\\f", out);
            break;
        case '\n':
            fputs("\\n", out);
            break;
        case '\r':
            fputs("\\r", out);
            break;
        case '\t':
            fputs("\\t", out);
            break;
        default:
            if (*p < 0x20) {
                fprintf(out, "\\u%04x", (unsigned)*p);
            } else {
                fputc((int)*p, out); /* UTF-8 goes through unchanged */
            }
            break;
        }
    }
    fputc('"', out);
}

/* An integral value prints without a decimal point; anything else keeps two
 * decimals. Rounds half away from zero so the JSON never disagrees with the
 * text table by one. */
static void json_print_number(FILE *out, double v)
{
    double rounded;
    if (!isfinite(v)) {
        fputs("0", out);
        return;
    }
    rounded = (v >= 0.0) ? floor(v + 0.5) : ceil(v - 0.5);
    if (fabs(v - rounded) < 1e-9) {
        fprintf(out, "%.0f", rounded == 0.0 ? 0.0 : rounded);
    } else {
        fprintf(out, "%.2f", v);
    }
}

static void json_key_number(FILE *out, const char *key, double v, int comma)
{
    fprintf(out, "\"%s\":", key);
    json_print_number(out, v);
    if (comma) {
        fputc(',', out);
    }
}

static void json_key_string(FILE *out, const char *key, const char *v, int comma)
{
    fprintf(out, "\"%s\":", key);
    if (v == NULL) {
        fputs("null", out);
    } else {
        json_escape(out, v);
    }
    if (comma) {
        fputc(',', out);
    }
}

/* The accounting fields shared by the total, per-day, per-model and per-session
 * objects. `first` says whether this is the first member in its object.
 * `l` may be NULL when the caller has no currency context; cost is then emitted
 * in the ledger's base currency. */
static void json_usage_members(FILE *out, const Ledger *l, const Usage *u, int first)
{
    if (!first) {
        fputc(',', out);
    }
    json_key_number(out, "tokens", usage_tokens(u), 1);
    json_key_number(out, "input", u->input, 1);
    json_key_number(out, "output", u->output, 1);
    json_key_number(out, "cacheRead", u->cacheRead, 1);
    json_key_number(out, "cacheWrite", u->cacheWrite, 1);
    json_key_number(out, "reasoning", u->reasoning, 1);
    json_key_number(out, "calls", u->calls, 1);
    {
        double cost = 0.0;
        double api_cost = 0.0;
        usage_cost(l, u, &cost, &api_cost);
        json_key_number(out, "cost", cost, 1);
        json_key_number(out, "apiCost", api_cost, 0);
    }
}

static void json_open_document(FILE *out, const Options *opt, const Ledger *l,
                               const char *source)
{
    fputs("{\n", out);
    fputs("  ", out);
    json_key_string(out, "tool", DSHTOK_NAME, 1);
    fputc('\n', out);
    fputs("  ", out);
    json_key_string(out, "version", DSHTOK_VERSION_STRING, 1);
    fputc('\n', out);
    fputs("  ", out);
    json_key_string(out, "source", source, 1);
    fputc('\n', out);
    fputs("  ", out);
    json_key_string(out, "currency", (l != NULL) ? l->currency : "", 1);
    fputc('\n', out);
    fputs("  \"range\":{\"since\":", out);
    json_escape(out, opt->since);
    fputs(",\"until\":", out);
    json_escape(out, opt->until);
    fputs("}", out);
}

static void json_close_document(FILE *out)
{
    fputs("\n}\n", out);
}

/* --- daily table ---------------------------------------------------------- */

static void print_daily_text(FILE *out, const Options *opt, const Ledger *l)
{
    size_t i;
    int rw = rank_width(l->day_count);
    char cell[NUMBER_BUF];
    (void)rw;

    print_header(out, opt, NULL);
    fprintf(out, INDENT "%*s  %-*s  %*s  %*s  %*s  %*s  %*s  %*s\n",
            0, "", SESSION_DATE_W, "date", DAILY_INPUT_W, "input",
            DAILY_OUTPUT_W, "output", DAILY_CREAD_W, "cache read",
            DAILY_CWRITE_W, "cache write", DAILY_TOTAL_W, "total",
            DAILY_CALLS_W, "calls");

    for (i = 0; i < l->day_count; ++i) {
        const DayRow *d = &l->days[i];
        fmt_tokens(d->usage.input, cell, sizeof cell);
        fprintf(out, INDENT "%s  %*s", d->date, DAILY_INPUT_W, cell);
        fmt_tokens(d->usage.output, cell, sizeof cell);
        fprintf(out, "  %*s", DAILY_OUTPUT_W, cell);
        fmt_tokens(d->usage.cacheRead, cell, sizeof cell);
        fprintf(out, "  %*s", DAILY_CREAD_W, cell);
        fmt_tokens(d->usage.cacheWrite, cell, sizeof cell);
        fprintf(out, "  %*s", DAILY_CWRITE_W, cell);
        fmt_tokens(usage_tokens(&d->usage), cell, sizeof cell);
        fprintf(out, "  %*s", DAILY_TOTAL_W, cell);
        fmt_tokens(d->usage.calls, cell, sizeof cell);
        fprintf(out, "  %*s\n", DAILY_CALLS_W, cell);
    }

    print_indent(out);
    fmt_pad_right("total", SESSION_DATE_W, cell, sizeof cell);
    fputs(cell, out);
    fmt_tokens(l->total.input, cell, sizeof cell);
    fprintf(out, "  %*s", DAILY_INPUT_W, cell);
    fmt_tokens(l->total.output, cell, sizeof cell);
    fprintf(out, "  %*s", DAILY_OUTPUT_W, cell);
    fmt_tokens(l->total.cacheRead, cell, sizeof cell);
    fprintf(out, "  %*s", DAILY_CREAD_W, cell);
    fmt_tokens(l->total.cacheWrite, cell, sizeof cell);
    fprintf(out, "  %*s", DAILY_CWRITE_W, cell);
    fmt_tokens(usage_tokens(&l->total), cell, sizeof cell);
    fprintf(out, "  %*s", DAILY_TOTAL_W, cell);
    fmt_tokens(l->total.calls, cell, sizeof cell);
    fprintf(out, "  %*s\n", DAILY_CALLS_W, cell);
}

static void csv_text(FILE *out, const char *s)
{
    fputc('"', out);
    for (; s != NULL && *s != '\0'; ++s) {
        if (*s == '"') {
            fputs("\"\"", out);
        } else {
            fputc(*s, out);
        }
    }
    fputc('"', out);
}

static void csv_number(FILE *out, double v)
{
    /* Token counters are whole numbers; render them without a decimal point so
     * the column pastes into a spreadsheet as a number. */
    double rounded = (v >= 0.0) ? floor(v + 0.5) : ceil(v - 0.5);
    if (fabs(v - rounded) < 1e-9) {
        fprintf(out, "%.0f", rounded == 0.0 ? 0.0 : rounded);
    } else {
        fprintf(out, "%.2f", v);
    }
}

static void print_daily_csv(FILE *out, const Options *opt, const Ledger *l)
{
    size_t i;
    double cost = 0.0;
    double api_cost = 0.0;
    (void)opt;
    fputs("date,input,output,cacheRead,cacheWrite,tokens,calls,cost,apiCost\n", out);
    for (i = 0; i < l->day_count; ++i) {
        const DayRow *d = &l->days[i];
        fprintf(out, "%s,", d->date);
        csv_number(out, d->usage.input);
        fputc(',', out);
        csv_number(out, d->usage.output);
        fputc(',', out);
        csv_number(out, d->usage.cacheRead);
        fputc(',', out);
        csv_number(out, d->usage.cacheWrite);
        fputc(',', out);
        csv_number(out, usage_tokens(&d->usage));
        fputc(',', out);
        csv_number(out, d->usage.calls);
        fputc(',', out);
        usage_cost(l, &d->usage, &cost, &api_cost);
        csv_number(out, cost);
        fputc(',', out);
        csv_number(out, api_cost);
        fputc('\n', out);
    }
}

/* --- models table --------------------------------------------------------- */

/* with_header is 0 when this table is appended to the `total` report, which
 * has already printed the title block; printing it again there would emit the
 * banner twice in one screenful. */
static void print_models_text(FILE *out, const Options *opt, const Ledger *l,
                              int with_header)
{
    size_t count = 0;
    ModelRow *rows = ledger_models_sorted(l, &count);
    size_t shown = effective_count(count, opt->top);
    int rw = rank_width(shown);
    char cell[BIG_BUF];
    char name[256];
    char num[NUMBER_BUF];
    char pct[NUMBER_BUF];
    size_t i;

    if (with_header) {
        print_header(out, opt, NULL);
    }
    fmt_pad_left("#", rw, cell, sizeof cell);
    fprintf(out, INDENT "%s  %-*s  %*s  %*s\n", cell, MODEL_NAME_W,
            "provider:model", MODEL_TOTAL_W, "total tokens", MODEL_SHARE_W,
            "share");

    for (i = 0; i < shown; ++i) {
        fmt_truncate(rows[i].name != NULL ? rows[i].name : "", MODEL_NAME_W, name,
                     sizeof name);
        fmt_tokens(usage_tokens(&rows[i].usage), num, sizeof num);
        fmt_percent(usage_tokens(&rows[i].usage), usage_tokens(&l->total), pct,
                    sizeof pct);
        fmt_pad_left("", 0, cell, 1);
        fprintf(out, INDENT "%*zu  %-*s  %*s  %*s\n", rw, i + 1, MODEL_NAME_W, name,
                MODEL_TOTAL_W, num, MODEL_SHARE_W, pct);
    }
    free(rows);
}

static void print_models_csv(FILE *out, const Options *opt, const Ledger *l)
{
    size_t count = 0;
    ModelRow *rows = ledger_models_sorted(l, &count);
    size_t shown = effective_count(count, opt->top);
    size_t i;
    double cost = 0.0;
    double api_cost = 0.0;

    fputs("name,tokens,input,output,cacheRead,cacheWrite,calls,cost,apiCost\n", out);
    for (i = 0; i < shown; ++i) {
        const ModelRow *r = &rows[i];
        csv_text(out, r->name);
        fputc(',', out);
        csv_number(out, usage_tokens(&r->usage));
        fputc(',', out);
        csv_number(out, r->usage.input);
        fputc(',', out);
        csv_number(out, r->usage.output);
        fputc(',', out);
        csv_number(out, r->usage.cacheRead);
        fputc(',', out);
        csv_number(out, r->usage.cacheWrite);
        fputc(',', out);
        csv_number(out, r->usage.calls);
        fputc(',', out);
        usage_cost(l, &r->usage, &cost, &api_cost);
        csv_number(out, cost);
        fputc(',', out);
        csv_number(out, api_cost);
        fputc('\n', out);
    }
    free(rows);
}

static void json_emit_models(FILE *out, const Options *opt, const Ledger *l)
{
    size_t count = 0;
    ModelRow *rows = ledger_models_sorted(l, &count);
    size_t shown = effective_count(count, opt->top);
    size_t i;
    double whole = usage_tokens(&l->total);

    fputs("\"models\":[", out);
    for (i = 0; i < shown; ++i) {
        double tokens = usage_tokens(&rows[i].usage);
        if (i > 0) {
            fputc(',', out);
        }
        fputs("\n    {\"name\":", out);
        json_escape(out, rows[i].name);
        fputc(',', out);
        json_usage_members(out, l, &rows[i].usage, 1);
        fputs(",\"share\":", out);
        json_print_number(out, whole > 0.0 ? (tokens / whole) * 100.0 : 0.0);
        fputc('}', out);
    }
    if (shown > 0) {
        fputs("\n  ", out);
    }
    fputs("]", out);
    free(rows);
}

/* --- sessions table ------------------------------------------------------- */

static void print_sessions_text(FILE *out, const Options *opt, const Ledger *l)
{
    size_t count = 0;
    SessionRow *rows = ledger_sessions_sorted(l, &count);
    size_t shown = effective_count(count, opt->top);
    int rw = rank_width(shown);
    char cell[BIG_BUF];
    char num[NUMBER_BUF];
    char calls[NUMBER_BUF];
    char title[256];
    size_t i;

    print_header(out, opt, NULL);
    fmt_pad_left("#", rw, cell, sizeof cell);
    fprintf(out, INDENT "%s  %-*s  %*s  %*s  %s\n", cell, SESSION_DATE_W, "date",
            SESSION_TOTAL_W, "total tokens", SESSION_CALLS_W, "calls", "title");

    for (i = 0; i < shown; ++i) {
        fmt_tokens(usage_tokens(&rows[i].usage), num, sizeof num);
        fmt_tokens(rows[i].usage.calls, calls, sizeof calls);
        fmt_truncate(rows[i].title != NULL ? rows[i].title : "", 48, title,
                     sizeof title);
        fprintf(out, INDENT "%*zu  %-*s  %*s  %*s  %s\n", rw, i + 1,
                SESSION_DATE_W, rows[i].date, SESSION_TOTAL_W, num, SESSION_CALLS_W,
                calls, title);
    }
    free(rows);
}

static void print_sessions_csv(FILE *out, const Options *opt, const Ledger *l)
{
    size_t count = 0;
    SessionRow *rows = ledger_sessions_sorted(l, &count);
    size_t shown = effective_count(count, opt->top);
    size_t i;

    fputs("id,date,tokens,calls,title\n", out);
    for (i = 0; i < shown; ++i) {
        const SessionRow *r = &rows[i];
        csv_text(out, r->id);
        fputc(',', out);
        csv_text(out, r->date);
        fputc(',', out);
        csv_number(out, usage_tokens(&r->usage));
        fputc(',', out);
        csv_number(out, r->usage.calls);
        fputc(',', out);
        csv_text(out, r->title);
        fputc('\n', out);
    }
    free(rows);
}

static void json_emit_sessions(FILE *out, const Options *opt, const Ledger *l)
{
    size_t count = 0;
    SessionRow *rows = ledger_sessions_sorted(l, &count);
    size_t shown = effective_count(count, opt->top);
    size_t i;

    fputs("\"sessions\":[", out);
    for (i = 0; i < shown; ++i) {
        const SessionRow *r = &rows[i];
        if (i > 0) {
            fputc(',', out);
        }
        fputs("\n    {\"id\":", out);
        json_escape(out, r->id);
        fputc(',', out);
        json_key_string(out, "date", r->date, 1);
        json_usage_members(out, l, &r->usage, 1);
        fputs(",\"title\":", out);
        json_escape(out, r->title);
        fputc('}', out);
    }
    if (shown > 0) {
        fputs("\n  ", out);
    }
    fputs("]", out);
    free(rows);
}

static void json_emit_days(FILE *out, const Ledger *l)
{
    size_t i;
    fputs("\"days\":[", out);
    for (i = 0; i < l->day_count; ++i) {
        const DayRow *d = &l->days[i];
        if (i > 0) {
            fputc(',', out);
        }
        fputs("\n    {\"date\":", out);
        json_escape(out, d->date);
        fputc(',', out);
        json_usage_members(out, l, &d->usage, 1);
        fputc('}', out);
    }
    if (l->day_count > 0) {
        fputs("\n  ", out);
    }
    fputs("]", out);
}

/* --- total ---------------------------------------------------------------- */

static void print_total_text(FILE *out, const Options *opt, const Ledger *l,
                             const char *source)
{
    char number[NUMBER_BUF];
    char compact[NUMBER_BUF];
    char note[BIG_BUF];
    char pct[NUMBER_BUF];
    char money[NUMBER_BUF];
    char money_api[NUMBER_BUF];
    double tokens = usage_tokens(&l->total);
    int bold = use_color(opt);

    print_header(out, opt, source);

    fmt_tokens(tokens, number, sizeof number);
    if (tokens >= 10000.0) {
        fmt_compact(tokens, compact, sizeof compact);
        snprintf(note, sizeof note, "(%s)", compact);
    } else {
        note[0] = '\0';
    }
    if (bold) {
        print_indent(out);
        fputs("\033[1m", out);
        {
            char padded[BIG_BUF];
            fmt_pad_right("total tokens", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
            fputs(padded, out);
            fmt_pad_left(number, DSHTOK_NUMBER_WIDTH, padded, sizeof padded);
            fputs(padded, out);
        }
        if (note[0] != '\0') {
            fprintf(out, "   %s", note);
        }
        fputs("\033[0m\n", out);
    } else {
        print_metric(out, "total tokens", number, note);
    }

    print_rule(out, opt, 56);

    fmt_tokens(l->total.input, number, sizeof number);
    fmt_percent(l->total.input, tokens, pct, sizeof pct);
    print_metric(out, "input", number, pct);

    fmt_tokens(l->total.output, number, sizeof number);
    fmt_percent(l->total.output, tokens, pct, sizeof pct);
    print_metric(out, "output", number, pct);

    fmt_tokens(l->total.cacheRead, number, sizeof number);
    fmt_percent(l->total.cacheRead, tokens, pct, sizeof pct);
    print_metric(out, "cache read", number, pct);

    fmt_tokens(l->total.cacheWrite, number, sizeof number);
    fmt_percent(l->total.cacheWrite, tokens, pct, sizeof pct);
    print_metric(out, "cache write", number, pct);

    print_rule(out, opt, 56);

    /* reasoning is a sub-count of output, so it is reported as a share of output
     * and never added into the total. */
    fmt_tokens(l->total.reasoning, number, sizeof number);
    fmt_percent(l->total.reasoning, l->total.output, pct, sizeof pct);
    snprintf(note, sizeof note, "%s of output", pct);
    print_metric(out, "reasoning (in output)", number, note);

    fmt_tokens(l->total.calls, number, sizeof number);
    print_metric(out, "calls", number, "");

    if (l->day_count > 0) {
        fmt_tokens((double)l->day_count, number, sizeof number);
        snprintf(note, sizeof note, "%s .. %s", l->days[0].date,
                 l->days[l->day_count - 1].date);
        print_metric(out, "days", number, note);
    } else {
        print_metric(out, "days", "0", "no data in range");
    }

    if (l->has_config) {
        double cost = money_value(l, l->total.cost, l->total.costCny);
        double api = money_value(l, l->total.apiCost, l->total.apiCostCny);
        if (opt->unicode) {
            fmt_money_symbol(cost, l->symbol, l->currency, 2, money, sizeof money);
            fmt_money_symbol(api, l->symbol, l->currency, 2, money_api,
                             sizeof money_api);
            snprintf(note, sizeof note, "(api %s)", money_api);
        } else {
            fmt_money(cost, money_prefix(opt, l), 2, money, sizeof money);
            fmt_money(api, money_prefix(opt, l), 2, money_api, sizeof money_api);
            snprintf(note, sizeof note, "(api %s)", money_api);
        }
        print_metric(out, "cost", money, note);
    }

    if (opt->verbose) {
        fputc('\n', out);
        print_models_text(out, opt, l, 0);
    }
}

static void print_total_json(FILE *out, const Options *opt, const Ledger *l,
                             const char *source)
{
    double tokens = usage_tokens(&l->total);
    json_open_document(out, opt, l, source);
    fputs(",\n  \"days_count\":", out);
    json_print_number(out, (double)l->day_count);
    fputs(",\n  \"total\":{", out);
    json_usage_members(out, l, &l->total, 1);
    fputc('}', out);
    if (opt->verbose) {
        fputs(",\n  ", out);
        json_emit_days(out, l);
        fputs(",\n  ", out);
        json_emit_models(out, opt, l);
        fputs(",\n  ", out);
        json_emit_sessions(out, opt, l);
    }
    (void)tokens;
    json_close_document(out);
}

/* --- doctor --------------------------------------------------------------- */

/* Size of a file in bytes, or -1. A ledger that exists but cannot be measured
 * is worth reporting as unknown rather than as zero: the difference between
 * "empty" and "I could not look" is exactly what doctor is for. Uses only
 * stdio so it behaves the same on every platform we support. */
static long long report_file_size(const char *path)
{
    FILE *f;
    long long size;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = (long long)ftell(f);
    fclose(f);
    return size < 0 ? -1 : size;
}

/* "2.4 MB" for a byte count, using the same 1024-based steps a file manager
 * would. Kept next to the doctor renderer because nothing else needs it. */
static void report_size_human(long long bytes, char *dst, size_t dstsz)
{
    static const char *const units[] = {"bytes", "KB", "MB", "GB", "TB"};
    double value;
    size_t unit = 0;

    if (bytes < 0) {
        str_copy(dst, dstsz, "unknown");
        return;
    }
    value = (double)bytes;
    while (value >= 1024.0 && unit + 1 < sizeof units / sizeof units[0]) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0) {
        /* Whole bytes read better without a decimal point. */
        fmt_grouped(value, dst, dstsz);
    } else {
        /* fmt_money with an empty currency is a decimal-precision grouped
         * number with no prefix, which is exactly what this needs. */
        fmt_money(value, "", 1, dst, dstsz);
    }
    {
        size_t used = strlen(dst);
        if (used + 1 < dstsz) {
            dst[used] = ' ';
            str_copy(dst + used + 1, dstsz - used - 1, units[unit]);
        }
    }
}

static void print_doctor_text(FILE *out, const Options *opt, const Ledger *l,
                              const char *source, const PathCandidates *pc,
                              const char *load_error)
{
    size_t i;
    char padded[BIG_BUF];
    char number[NUMBER_BUF];

    print_header(out, opt, NULL);

    fputs("search\n", out);
    print_indent(out);
    fmt_pad_right("DSH_HOME", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
    fputs(padded, out);
    {
        char *home = path_dsh_home();
        fputs(home != NULL ? home : "(unset)", out);
        free(home);
    }
    fputc('\n', out);
    print_indent(out);
    fmt_pad_right("DSHTOK_LEDGER", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
    fputs(padded, out);
    {
        const char *env = getenv("DSHTOK_LEDGER");
        fputs((env != NULL && env[0] != '\0') ? env : "(unset)", out);
    }
    fputc('\n', out);
    print_indent(out);
    fmt_pad_right("candidates", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
    fputs(padded, out);
    fputc('\n', out);

    if (pc != NULL) {
        if (pc->count == 0) {
            fprintf(out, "%-22s%s\n", "", "(none: --path was given, no search was done)");
        }
        for (i = 0; i < pc->count; ++i) {
            int found = path_is_readable_file(pc->candidates[i]);
            fprintf(out, "%-22s%s%s%s\n", "", found ? "[found] " : "        ",
                    pc->candidates[i],
                    (source != NULL && strcmp(source, pc->candidates[i]) == 0)
                        ? "  <- selected"
                        : "");
        }
    }

    if (source == NULL) {
        fputs("\nno ledger found\n", out);
        fputs("  This file is written by the dsh-cost-meter plugin, which must be\n"
              "  installed and must have recorded at least one request.\n"
              "  See docs/data-source.md.\n",
              out);
        return;
    }

    fputs("\nledger\n", out);
    print_indent(out);
    fmt_pad_right("path", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
    fprintf(out, "%s%s\n", padded, source);

    {
        long long bytes = report_file_size(source);
        char human[NUMBER_BUF];
        report_size_human(bytes, human, sizeof human);
        print_indent(out);
        fmt_pad_right("size", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
        if (bytes < 1024) {
            /* Under 1024 the human form already is the exact count plus a unit
             * word, so a parenthesised repeat would read "512 bytes (512
             * bytes)". "unknown" also lands here and prints on its own. */
            fprintf(out, "%s%s\n", padded, human);
        } else {
            char raw[NUMBER_BUF];
            fmt_grouped((double)bytes, raw, sizeof raw);
            fprintf(out, "%s%s bytes (%s)\n", padded, raw, human);
        }
    }

    if (l == NULL) {
        print_indent(out);
        fmt_pad_right("status", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
        fprintf(out, "%s%s\n", padded,
                (load_error != NULL && load_error[0] != '\0') ? load_error
                                                             : "unreadable");
        return;
    }

    print_indent(out);
    fmt_pad_right("status", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
    fprintf(out, "%sok\n", padded);

    fmt_tokens((double)l->day_count, number, sizeof number);
    print_metric(out, "days", number, "");
    if (l->day_count > 0) {
        print_indent(out);
        fmt_pad_right("range", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
        fprintf(out, "%s%s .. %s\n", padded, l->days[0].date,
                l->days[l->day_count - 1].date);
    }
    fmt_tokens((double)l->session_count, number, sizeof number);
    print_metric(out, "sessions", number, "");
    fmt_tokens((double)l->model_count, number, sizeof number);
    print_metric(out, "models", number, "");
    fmt_tokens(l->total.calls, number, sizeof number);
    print_metric(out, "calls", number, "");
    fmt_tokens(usage_tokens(&l->total), number, sizeof number);
    print_metric(out, "tokens", number, "");
    if (l->has_config) {
        print_indent(out);
        fmt_pad_right("currency", DSHTOK_LABEL_WIDTH, padded, sizeof padded);
        fprintf(out, "%s%s%s (%d decimals)\n", padded, l->currency,
                (l->symbol[0] != '\0') ? " " : "", l->decimals);
    }
}

/* --- entry point ---------------------------------------------------------- */

static int command_is(const char *command, const char *name)
{
    return command != NULL && strcmp(command, name) == 0;
}

int report_run(const Options *opt, const char *ledger_path)
{
    PathCandidates pc;
    const char *resolved = NULL; /* borrowed from pc; never freed on its own */
    const char *source = NULL;
    Ledger ledger;
    char err[512];
    int is_doctor;
    int rc = 0;

    if (opt == NULL) {
        return 2;
    }
    is_doctor = command_is(opt->command, "doctor");

    memset(&pc, 0, sizeof pc);
    memset(&ledger, 0, sizeof ledger);
    err[0] = '\0';

    /* Resolve which file to read. An explicit --path is used as given so that a
     * typo produces "cannot open file" for that exact name rather than a list of
     * guesses. */
    if (ledger_path != NULL && ledger_path[0] != '\0') {
        source = ledger_path;
    } else {
        path_candidates_build(&pc, opt->profile);
        resolved = path_first_existing(&pc);
        source = resolved;
    }

    if (!is_doctor && (source == NULL)) {
        size_t i;
        fputs("dshtok: no ledger found\n", stderr);
        fputs("  looked for:\n", stderr);
        for (i = 0; i < pc.count; ++i) {
            fprintf(stderr, "    %s\n", pc.candidates[i]);
        }
        fputs("  This file is written by the dsh-cost-meter plugin, which must be "
              "installed and\n  must have recorded at least one request. See "
              "docs/data-source.md.\n",
              stderr);
        path_candidates_free(&pc);
        return 1;
    }

    if (is_doctor) {
        /* doctor reports on the search itself, so it must not fail when nothing
         * was found -that is the most useful outcome it can have. */
        if (source != NULL) {
            if (ledger_load(source, &ledger, err, sizeof err) == 0) {
                if ((opt->since != NULL && opt->since[0] != '\0') ||
                    (opt->until != NULL && opt->until[0] != '\0')) {
                    if (ledger_filter_range(&ledger, opt->since, opt->until) ==
                        (size_t)-1) {
                        fprintf(stderr, "dshtok: bad date in --since/--until (want "
                                        "YYYY-MM-DD)\n");
                        ledger_free(&ledger);
                        path_candidates_free(&pc);
                        return 2;
                    }
                }
                print_doctor_text(stdout, opt, &ledger, source, &pc, err);
                ledger_free(&ledger);
                path_candidates_free(&pc);
                return 0;
            }
        }
        print_doctor_text(stdout, opt, NULL, source, &pc, err);
        ledger_free(&ledger);
        path_candidates_free(&pc);
        return 0;
    }

    if (ledger_load(source, &ledger, err, sizeof err) != 0) {
        fprintf(stderr, "dshtok: %s: %s\n", source,
                (err[0] != '\0') ? err : "cannot read ledger");
        path_candidates_free(&pc);
        return 1;
    }

    if ((opt->since != NULL && opt->since[0] != '\0') ||
        (opt->until != NULL && opt->until[0] != '\0')) {
        if (ledger_filter_range(&ledger, opt->since, opt->until) == (size_t)-1) {
            fprintf(stderr,
                    "dshtok: bad date in --since/--until (want YYYY-MM-DD)\n");
            ledger_free(&ledger);
            path_candidates_free(&pc);
            return 2;
        }
    }

    if (opt->json && opt->csv) {
        fputs("dshtok: --json and --csv cannot be combined\n", stderr);
        ledger_free(&ledger);
        path_candidates_free(&pc);
        return 2;
    }

    if (opt->quiet) {
        fprintf(stdout, "%.0f\n", usage_tokens(&ledger.total));
        ledger_free(&ledger);
        path_candidates_free(&pc);
        return 0;
    }

    if (command_is(opt->command, "total") || opt->command == NULL) {
        if (opt->csv) {
            fputs("dshtok: --csv is not supported for 'total'; try 'daily', "
                  "'models' or 'sessions'\n",
                  stderr);
            ledger_free(&ledger);
            path_candidates_free(&pc);
            return 2;
        }
        if (opt->json) {
            print_total_json(stdout, opt, &ledger, source);
        } else {
            print_total_text(stdout, opt, &ledger, source);
        }
    } else if (command_is(opt->command, "daily")) {
        if (opt->json) {
            json_open_document(stdout, opt, &ledger, source);
            fputs(",\n  ", stdout);
            json_emit_days(stdout, &ledger);
            json_close_document(stdout);
        } else if (opt->csv) {
            print_daily_csv(stdout, opt, &ledger);
        } else {
            print_daily_text(stdout, opt, &ledger);
        }
    } else if (command_is(opt->command, "models")) {
        if (opt->json) {
            json_open_document(stdout, opt, &ledger, source);
            fputs(",\n  ", stdout);
            json_emit_models(stdout, opt, &ledger);
            json_close_document(stdout);
        } else if (opt->csv) {
            print_models_csv(stdout, opt, &ledger);
        } else {
            print_models_text(stdout, opt, &ledger, 1);
        }
    } else if (command_is(opt->command, "sessions")) {
        if (opt->json) {
            json_open_document(stdout, opt, &ledger, source);
            fputs(",\n  ", stdout);
            json_emit_sessions(stdout, opt, &ledger);
            json_close_document(stdout);
        } else if (opt->csv) {
            print_sessions_csv(stdout, opt, &ledger);
        } else {
            print_sessions_text(stdout, opt, &ledger);
        }
    } else {
        fprintf(stderr, "dshtok: unknown command '%s'\n", opt->command);
        rc = 2;
    }

    ledger_free(&ledger);
    path_candidates_free(&pc);
    return rc;
}

void report_usage(FILE *stream, int full)
{
    if (stream == NULL) {
        return;
    }
    if (!full) {
        fprintf(stream, "usage: %s [OPTIONS] [COMMAND]\n", DSHTOK_NAME);
        return;
    }
    fprintf(stream,
            "%s %s - %s\n"
            "\n"
            "Reports how many tokens DeepSeek Harness has used in total, read from\n"
            "the ledger written by the dsh-cost-meter plugin.\n"
            "\n"
            "usage: %s [OPTIONS] [COMMAND]\n"
            "\n"
            "commands\n"
            "  total      cumulative totals (default)\n"
            "  daily      one line per day\n"
            "  models     totals grouped by provider:model\n"
            "  sessions   per-session leaderboard\n"
            "  doctor     show where the ledger was looked for and what was found\n"
            "  help       show this help\n"
            "\n"
            "options\n"
            "  -p, --path FILE   read this ledger.json instead of auto-detecting\n"
            "      --profile N   look under profiles/N when auto-detecting\n"
            "      --since DATE  include days >= DATE (YYYY-MM-DD)\n"
            "      --until DATE  include days <= DATE (YYYY-MM-DD)\n"
            "      --top N       rows shown by 'sessions' and 'models'\n"
            "      --json        machine readable output\n"
            "      --csv         CSV output for daily, models and sessions\n"
            "  -u, --unicode     allow UTF-8 box drawing and the currency symbol\n"
            "      --no-color    disable ANSI colour\n"
            "      --no-header   omit the title and source lines\n"
            "  -q, --quiet       print only the total token count\n"
            "  -v, --verbose     include the model breakdown\n"
            "  -h, --help        show this help\n"
            "  -V, --version     show the version\n"
            "\n"
            "environment\n"
            "  DSH_HOME          DeepSeek Harness home directory (default ~/.dsh)\n"
            "  DSHTOK_LEDGER     explicit path to ledger.json\n"
            "  DSH_PROFILE_DIR   derive the home from a profile directory\n"
            "\n"
            "exit codes\n"
            "  0 success   1 runtime error   2 usage error\n",
            DSHTOK_NAME, DSHTOK_VERSION_STRING, DSHTOK_TAGLINE, DSHTOK_NAME);
}
