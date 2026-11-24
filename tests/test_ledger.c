/* Tests for the frozen ledger API: loading, merging across days, range
 * filtering and the sorted leaderboards.
 *
 * Every test reads a fixture from tests/fixtures (located relative to the
 * repository root, which is the working directory `make test` uses) so nothing
 * here can ever touch the real ~/.dsh ledger. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dshtok/ledger.h"
#include "mini_test.h"

#define BIG 512

/* Fixture paths are relative to the repo root. */
static const char *FIX = "tests/fixtures/";

static char *fixture(const char *name)
{
    size_t n = strlen(FIX) + strlen(name) + 1;
    char *p = (char *)malloc(n);

    if (p == NULL) {
        fprintf(stderr, "out of memory building a fixture path\n");
        exit(2);
    }
    memcpy(p, FIX, strlen(FIX));
    memcpy(p + strlen(FIX), name, strlen(name) + 1);
    return p;
}

/* Load a named fixture into a zeroed Ledger. Returns the load result. */
static int load_fixture(const char *name, Ledger *l, char *err, size_t errsz)
{
    char *path = fixture(name);
    int rc;

    memset(l, 0, sizeof *l);
    rc = ledger_load(path, l, err, errsz);
    free(path);
    return rc;
}

/* ------------------------------------------------------------------ */
/* usage helpers                                                       */
/* ------------------------------------------------------------------ */

static void t_usage_tokens(void)
{
    Usage u;

    usage_zero(&u);
    CHECK_NEAR(usage_tokens(&u), 0.0, 1e-9);

    u.input = 10.0;
    u.output = 20.0;
    u.cacheRead = 30.0;
    u.cacheWrite = 40.0;
    CHECK_NEAR(usage_tokens(&u), 100.0, 1e-9);

    /* reasoning is a sub-count of output: it must not move the headline. */
    u.reasoning = 500.0;
    CHECK_NEAR(usage_tokens(&u), 100.0, 1e-9);

    /* cost fields are money, not tokens. */
    u.cost = 99.0;
    u.apiCost = 99.0;
    u.costCny = 99.0;
    u.apiCostCny = 99.0;
    u.calls = 99.0;
    CHECK_NEAR(usage_tokens(&u), 100.0, 1e-9);

    /* NULL is tolerated by the frozen header. */
    CHECK_NEAR(usage_tokens(NULL), 0.0, 1e-9);
}

static void t_usage_add_and_zero(void)
{
    Usage a;
    Usage b;

    usage_zero(&a);
    CHECK_NEAR(a.input, 0.0, 1e-9);
    CHECK_NEAR(a.output, 0.0, 1e-9);
    CHECK_NEAR(a.cacheRead, 0.0, 1e-9);
    CHECK_NEAR(a.cacheWrite, 0.0, 1e-9);
    CHECK_NEAR(a.reasoning, 0.0, 1e-9);
    CHECK_NEAR(a.calls, 0.0, 1e-9);
    CHECK_NEAR(a.cost, 0.0, 1e-9);
    CHECK_NEAR(a.apiCost, 0.0, 1e-9);
    CHECK_NEAR(a.costCny, 0.0, 1e-9);
    CHECK_NEAR(a.apiCostCny, 0.0, 1e-9);

    a.input = 1.0;
    a.output = 2.0;
    a.cacheRead = 3.0;
    a.cacheWrite = 4.0;
    a.reasoning = 5.0;
    a.calls = 6.0;
    a.cost = 7.0;
    a.apiCost = 8.0;
    a.costCny = 9.0;
    a.apiCostCny = 10.0;

    usage_zero(&b);
    usage_add(&b, &a);
    CHECK_NEAR(b.input, 1.0, 1e-9);
    CHECK_NEAR(b.output, 2.0, 1e-9);
    CHECK_NEAR(b.cacheRead, 3.0, 1e-9);
    CHECK_NEAR(b.cacheWrite, 4.0, 1e-9);
    CHECK_NEAR(b.reasoning, 5.0, 1e-9);
    CHECK_NEAR(b.calls, 6.0, 1e-9);
    CHECK_NEAR(b.cost, 7.0, 1e-9);
    CHECK_NEAR(b.apiCost, 8.0, 1e-9);
    CHECK_NEAR(b.costCny, 9.0, 1e-9);
    CHECK_NEAR(b.apiCostCny, 10.0, 1e-9);

    /* Adding again accumulates rather than replaces. */
    usage_add(&b, &a);
    CHECK_NEAR(b.input, 2.0, 1e-9);
    CHECK_NEAR(usage_tokens(&b), 20.0, 1e-9);

    /* Adding nothing is a no-op; NULL source must be safe in a well behaved
     * implementation, but the header does not promise it, so it is untested. */
    usage_zero(&a);
    usage_add(&b, &a);
    CHECK_NEAR(b.input, 2.0, 1e-9);
}

/* ------------------------------------------------------------------ */
/* ledger_valid_date                                                   */
/* ------------------------------------------------------------------ */

static void t_valid_date(void)
{
    /* Valid shapes. */
    CHECK_EQ_INT(ledger_valid_date("2026-01-10"), 1);
    CHECK_EQ_INT(ledger_valid_date("1999-12-31"), 1);
    CHECK_EQ_INT(ledger_valid_date("0000-00-00"), 1);

    /* Shape only: out of range but correctly shaped values pass. The header is
     * explicit that this is not a calendar check. */
    CHECK_EQ_INT(ledger_valid_date("2026-13-45"), 1);
    CHECK_EQ_INT(ledger_valid_date("2026-99-99"), 1);

    /* Wrong shape. */
    CHECK_EQ_INT(ledger_valid_date("2026-1-10"), 0);
    CHECK_EQ_INT(ledger_valid_date("2026-01-1"), 0);
    CHECK_EQ_INT(ledger_valid_date("26-01-10"), 0);
    CHECK_EQ_INT(ledger_valid_date("2026/01/10"), 0);
    CHECK_EQ_INT(ledger_valid_date(""), 0);
    CHECK_EQ_INT(ledger_valid_date("2026-01-10x"), 0);
    CHECK_EQ_INT(ledger_valid_date("2026-01-100"), 0);
    CHECK_EQ_INT(ledger_valid_date("2026-0a-10"), 0);
    CHECK_EQ_INT(ledger_valid_date("2026-01-1a"), 0);
    CHECK_EQ_INT(ledger_valid_date("abcd-ef-gh"), 0);

    /* NULL must not crash; false is the only sensible answer. */
    CHECK_EQ_INT(ledger_valid_date(NULL), 0);
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

static void t_load_missing_file(void)
{
    Ledger l;
    char err[BIG];

    memset(&l, 0, sizeof l);
    memset(err, 0, sizeof err);
    CHECK_EQ_INT(ledger_load("tests/fixtures/definitely_absent_ledger.json",
                             &l, err, sizeof err), -1);
    CHECK(err[0] != '\0');
    /* A failed load must leave a freeable ledger behind. */
    ledger_free(&l);
}

static void t_load_malformed(void)
{
    Ledger l;
    char err[BIG];

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_malformed.json", &l, err, sizeof err), -1);
    CHECK(err[0] != '\0');
    ledger_free(&l);
}

static void t_load_not_an_object(void)
{
    Ledger l;
    char err[BIG];

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_not_an_object.json", &l, err, sizeof err),
                 -1);
    CHECK(err[0] != '\0');
    ledger_free(&l);
}

static void t_load_basic(void)
{
    Ledger l;
    char err[BIG];
    double expected;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);
    CHECK_EQ_STR(err, "");

    CHECK_EQ_INT(l.day_count, 2);

    /* Days come out in chronological order. */
    CHECK_EQ_STR(l.days[0].date, "2026-01-10");
    CHECK_EQ_STR(l.days[1].date, "2026-01-11");

    /* Per day usage is the sum of the four buckets. */
    CHECK_NEAR(usage_tokens(&l.days[0].usage), 1000.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.reasoning, 50.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.calls, 5.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.cost, 1.25, 1e-9);
    CHECK_NEAR(l.days[1].usage.input, 10.0, 1e-9);

    /* Config is carried through. */
    CHECK_EQ_INT(l.has_config, 1);
    CHECK_EQ_STR(l.currency, "CNY");
    CHECK_EQ_STR(l.symbol, "\xC2\xA5");
    CHECK_EQ_INT(l.decimals, 2);
    CHECK_NEAR(l.exchangeRate, 7.1, 1e-9);

    /* Grand total adds both days: 1000 + 100. */
    expected = 1100.0;
    CHECK_NEAR(usage_tokens(&l.total), expected, 1e-9);
    CHECK_NEAR(l.total.cost, 1.75, 1e-9);
    CHECK_NEAR(l.total.reasoning, 55.0, 1e-9);
    CHECK_NEAR(l.total.calls, 7.0, 1e-9);

    /* Per day entry counts. */
    CHECK_EQ_INT(l.days[0].session_count, 1);
    CHECK_EQ_INT(l.days[0].model_count, 1);
    CHECK_EQ_INT(l.days[1].session_count, 2);
    CHECK_EQ_INT(l.days[1].model_count, 2);

    ledger_free(&l);
}

static void t_load_minimal_no_config(void)
{
    Ledger l;
    char err[BIG];

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_minimal.json", &l, err, sizeof err), 0);

    /* No config block: has_config is 0 and the money line must be suppressed. */
    CHECK_EQ_INT(l.has_config, 0);
    CHECK_EQ_STR(l.currency, "");

    CHECK_EQ_INT(l.day_count, 1);
    CHECK_NEAR(usage_tokens(&l.total), 10.0, 1e-9);

    /* Absent optional arrays leave the counts at zero. */
    CHECK_EQ_INT(l.days[0].session_count, 0);
    CHECK_EQ_INT(l.days[0].model_count, 0);
    CHECK_EQ_INT(l.session_count, 0);
    CHECK_EQ_INT(l.model_count, 0);

    ledger_free(&l);
}

static void t_load_empty_days(void)
{
    Ledger l;
    char err[BIG];

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_empty_days.json", &l, err, sizeof err), 0);
    CHECK_EQ_INT(l.day_count, 0);
    CHECK_NEAR(usage_tokens(&l.total), 0.0, 1e-9);
    ledger_free(&l);
}

static void t_load_missing_numerics_default_zero(void)
{
    Ledger l;
    char err[BIG];

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_missing_numerics.json", &l, err,
                              sizeof err), 0);
    CHECK_EQ_INT(l.day_count, 1);

    /* Only input was present; everything else defaults to zero rather than
     * failing the load. */
    CHECK_NEAR(l.days[0].usage.input, 10.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.output, 0.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.cacheRead, 0.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.cacheWrite, 0.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.reasoning, 0.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.calls, 0.0, 1e-9);
    CHECK_NEAR(l.days[0].usage.cost, 0.0, 1e-9);
    CHECK_NEAR(usage_tokens(&l.total), 10.0, 1e-9);

    ledger_free(&l);
}

static void t_load_future_version(void)
{
    Ledger l;
    char err[BIG];

    memset(err, 0, sizeof err);
    /* A future version must not crash the loader; unknown fields are ignored. */
    CHECK_EQ_INT(load_fixture("ledger_future_version.json", &l, err,
                              sizeof err), 0);
    CHECK_EQ_INT(l.day_count, 1);
    CHECK_NEAR(usage_tokens(&l.total), 34.0, 1e-9);
    CHECK_EQ_STR(l.currency, "USD");
    CHECK_EQ_STR(l.days[0].date, "2026-05-05");

    ledger_free(&l);
}

/* ------------------------------------------------------------------ */
/* merging across days                                                 */
/* ------------------------------------------------------------------ */

static void t_merge_session_across_days(void)
{
    Ledger l;
    char err[BIG];
    SessionRow *rows;
    size_t n = 0;
    size_t i;
    size_t alpha = (size_t)-1;
    size_t beta = (size_t)-1;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    /* session-alpha appears under both days: one merged row. */
    CHECK_EQ_INT(l.session_count, 2);

    rows = ledger_sessions_sorted(&l, &n);
    CHECK(rows != NULL);
    CHECK_EQ_INT(n, 2);

    for (i = 0; i < n; ++i) {
        if (strcmp(rows[i].id, "session-alpha") == 0) {
            alpha = i;
        } else if (strcmp(rows[i].id, "session-beta") == 0) {
            beta = i;
        }
    }
    CHECK(alpha != (size_t)-1);
    CHECK(beta != (size_t)-1);

    if (alpha != (size_t)-1) {
        /* Usage is summed across the two days (1000 + 100 tokens). */
        CHECK_NEAR(usage_tokens(&rows[alpha].usage), 1100.0, 1e-9);
        CHECK_NEAR(rows[alpha].usage.calls, 7.0, 1e-9);
        /* Title comes from the only day that carries one. */
        CHECK_EQ_STR(rows[alpha].title, "Alpha task");
        /* date is the EARLIEST day, at is the LATEST timestamp. */
        CHECK_EQ_STR(rows[alpha].date, "2026-01-10");
        CHECK_NEAR(rows[alpha].at, 1768100000000.0, 1e-3);
    }
    if (beta != (size_t)-1) {
        CHECK_EQ_STR(rows[beta].title, "Beta task");
        CHECK_EQ_STR(rows[beta].date, "2026-01-11");
    }

    /* Sorted highest token count first. */
    CHECK(usage_tokens(&rows[0].usage) >= usage_tokens(&rows[1].usage));

    free(rows);
    ledger_free(&l);
}

static void t_merge_model_across_days(void)
{
    Ledger l;
    char err[BIG];
    ModelRow *rows;
    size_t n = 0;
    size_t i;
    size_t chat = (size_t)-1;
    size_t reasoner = (size_t)-1;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    /* "deepseek:chat" appears under both days and must collapse to one row. */
    CHECK_EQ_INT(l.model_count, 2);

    rows = ledger_models_sorted(&l, &n);
    CHECK(rows != NULL);
    CHECK_EQ_INT(n, 2);

    for (i = 0; i < n; ++i) {
        if (strcmp(rows[i].name, "deepseek:chat") == 0) {
            chat = i;
        } else if (strcmp(rows[i].name, "deepseek:reasoner") == 0) {
            reasoner = i;
        }
    }
    CHECK(chat != (size_t)-1);
    CHECK(reasoner != (size_t)-1);

    if (chat != (size_t)-1) {
        CHECK_NEAR(usage_tokens(&rows[chat].usage), 1100.0, 1e-9);
        CHECK_NEAR(rows[chat].usage.calls, 7.0, 1e-9);
        CHECK_NEAR(rows[chat].usage.reasoning, 55.0, 1e-9);
    }
    if (reasoner != (size_t)-1) {
        CHECK_NEAR(usage_tokens(&rows[reasoner].usage), 0.0, 1e-9);
    }

    /* Highest token count first. */
    CHECK(usage_tokens(&rows[0].usage) >= usage_tokens(&rows[1].usage));

    free(rows);
    ledger_free(&l);
}

/* ------------------------------------------------------------------ */
/* reindex                                                             */
/* ------------------------------------------------------------------ */

static void t_reindex_is_idempotent(void)
{
    Ledger l;
    char err[BIG];
    double before;
    size_t models_before;
    size_t sessions_before;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    before = usage_tokens(&l.total);
    models_before = l.model_count;
    sessions_before = l.session_count;

    /* Calling it again must not double count or leak a stale table. */
    ledger_reindex(&l);
    CHECK_NEAR(usage_tokens(&l.total), before, 1e-9);
    CHECK_EQ_INT(l.model_count, models_before);
    CHECK_EQ_INT(l.session_count, sessions_before);

    ledger_reindex(&l);
    CHECK_NEAR(usage_tokens(&l.total), before, 1e-9);
    CHECK_EQ_INT(l.model_count, models_before);
    CHECK_EQ_INT(l.session_count, sessions_before);

    ledger_free(&l);
}

static void t_reindex_on_zeroed_ledger(void)
{
    Ledger l;

    memset(&l, 0, sizeof l);
    /* Safe without a prior load. */
    ledger_reindex(&l);
    CHECK_EQ_INT(l.day_count, 0);
    CHECK_EQ_INT(l.model_count, 0);
    CHECK_EQ_INT(l.session_count, 0);
    CHECK_NEAR(usage_tokens(&l.total), 0.0, 1e-9);
    ledger_free(&l);
}

/* ------------------------------------------------------------------ */
/* ledger_filter_range                                                 */
/* ------------------------------------------------------------------ */

static void t_filter_keeps_both_ends(void)
{
    Ledger l;
    char err[BIG];
    size_t kept;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    /* Bounds are INCLUSIVE on both ends: both days survive. */
    kept = ledger_filter_range(&l, "2026-01-10", "2026-01-11");
    CHECK_EQ_INT(kept, 2);
    CHECK_EQ_INT(l.day_count, 2);
    /* Totals are recomputed over the kept days. */
    CHECK_NEAR(usage_tokens(&l.total), 1100.0, 1e-9);

    ledger_free(&l);
}

static void t_filter_single_day(void)
{
    Ledger l;
    char err[BIG];
    size_t kept;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    /* Same day on both ends keeps exactly that day. */
    kept = ledger_filter_range(&l, "2026-01-11", "2026-01-11");
    CHECK_EQ_INT(kept, 1);
    CHECK_EQ_INT(l.day_count, 1);
    CHECK_EQ_STR(l.days[0].date, "2026-01-11");
    CHECK_NEAR(usage_tokens(&l.total), 100.0, 1e-9);

    /* The derived tables shrink with the day set. */
    CHECK_EQ_INT(l.session_count, 2);
    CHECK_EQ_INT(l.model_count, 2);

    ledger_free(&l);
}

static void t_filter_open_ended(void)
{
    Ledger l;
    char err[BIG];
    size_t kept;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);
    kept = ledger_filter_range(&l, "2026-01-11", NULL);
    CHECK_EQ_INT(kept, 1);
    CHECK_NEAR(usage_tokens(&l.total), 100.0, 1e-9);
    ledger_free(&l);

    memset(&l, 0, sizeof l);
    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);
    kept = ledger_filter_range(&l, NULL, "2026-01-10");
    CHECK_EQ_INT(kept, 1);
    CHECK_NEAR(usage_tokens(&l.total), 1000.0, 1e-9);
    ledger_free(&l);

    /* Both bounds NULL keeps everything. */
    memset(&l, 0, sizeof l);
    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);
    kept = ledger_filter_range(&l, NULL, NULL);
    CHECK_EQ_INT(kept, 2);
    CHECK_NEAR(usage_tokens(&l.total), 1100.0, 1e-9);
    ledger_free(&l);
}

static void t_filter_selects_nothing(void)
{
    Ledger l;
    char err[BIG];
    size_t kept;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    /* A window outside the data keeps zero days and zeroes the total. */
    kept = ledger_filter_range(&l, "2030-01-01", "2030-12-31");
    CHECK_EQ_INT(kept, 0);
    CHECK_EQ_INT(l.day_count, 0);
    CHECK_NEAR(usage_tokens(&l.total), 0.0, 1e-9);
    CHECK_EQ_INT(l.session_count, 0);
    CHECK_EQ_INT(l.model_count, 0);

    ledger_free(&l);
}

static void t_filter_invalid_bound_untouched(void)
{
    Ledger l;
    char err[BIG];
    size_t kept;
    size_t days_before;
    size_t sessions_before;
    double total_before;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    days_before = l.day_count;
    sessions_before = l.session_count;
    total_before = usage_tokens(&l.total);

    /* An invalid bound is caller error: (size_t)-1 and the ledger is untouched. */
    kept = ledger_filter_range(&l, "not-a-date", NULL);
    CHECK_EQ_INT(kept, (size_t)-1);
    CHECK_EQ_INT(l.day_count, days_before);
    CHECK_EQ_INT(l.session_count, sessions_before);
    CHECK_NEAR(usage_tokens(&l.total), total_before, 1e-9);

    kept = ledger_filter_range(&l, NULL, "2026-1-1");
    CHECK_EQ_INT(kept, (size_t)-1);
    CHECK_EQ_INT(l.day_count, days_before);
    CHECK_NEAR(usage_tokens(&l.total), total_before, 1e-9);

    ledger_free(&l);
}

/* ------------------------------------------------------------------ */
/* sorted tables and ownership                                         */
/* ------------------------------------------------------------------ */

static void t_sorted_null_out_count(void)
{
    Ledger l;
    char err[BIG];
    ModelRow *m;
    SessionRow *s;

    memset(err, 0, sizeof err);
    CHECK_EQ_INT(load_fixture("ledger_basic.json", &l, err, sizeof err), 0);

    /* out_count is documented as optional. */
    m = ledger_models_sorted(&l, NULL);
    CHECK(m != NULL);
    free(m);

    s = ledger_sessions_sorted(&l, NULL);
    CHECK(s != NULL);
    free(s);

    ledger_free(&l);
}

static void t_sorted_on_zeroed_ledger(void)
{
    Ledger l;
    size_t n = 12345;

    memset(&l, 0, sizeof l);

    /* An empty ledger has nothing to sort: NULL or a freeable empty array are
     * both acceptable, but the count must be zero and free() must be safe. */
    {
        ModelRow *m = ledger_models_sorted(&l, &n);
        CHECK_EQ_INT(n, 0);
        free(m);
    }
    n = 12345;
    {
        SessionRow *s = ledger_sessions_sorted(&l, &n);
        CHECK_EQ_INT(n, 0);
        free(s);
    }
}

/* ------------------------------------------------------------------ */
/* free                                                                */
/* ------------------------------------------------------------------ */

static void t_free_safety(void)
{
    Ledger l;

    /* Freeing a zeroed ledger is a no-op. */
    memset(&l, 0, sizeof l);
    ledger_free(&l);

    /* Freeing twice is safe on an implementation that zeroes as it goes; the
     * header says idempotent, so this must hold. */
    ledger_free(&l);
    CHECK_EQ_INT(l.day_count, 0);
}

TEST_MAIN_BEGIN()
    RUN(t_usage_tokens);
    RUN(t_usage_add_and_zero);
    RUN(t_valid_date);
    RUN(t_load_missing_file);
    RUN(t_load_malformed);
    RUN(t_load_not_an_object);
    RUN(t_load_basic);
    RUN(t_load_minimal_no_config);
    RUN(t_load_empty_days);
    RUN(t_load_missing_numerics_default_zero);
    RUN(t_load_future_version);
    RUN(t_merge_session_across_days);
    RUN(t_merge_model_across_days);
    RUN(t_reindex_is_idempotent);
    RUN(t_reindex_on_zeroed_ledger);
    RUN(t_filter_keeps_both_ends);
    RUN(t_filter_single_day);
    RUN(t_filter_open_ended);
    RUN(t_filter_selects_nothing);
    RUN(t_filter_invalid_bound_untouched);
    RUN(t_sorted_null_out_count);
    RUN(t_sorted_on_zeroed_ledger);
    RUN(t_free_safety);
TEST_MAIN_END()
