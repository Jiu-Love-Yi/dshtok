#ifndef DSHTOK_LEDGER_H
#define DSHTOK_LEDGER_H

#include <stddef.h>

#define DSHTOK_DATE_LEN 11   /* "YYYY-MM-DD" plus NUL */
#define DSHTOK_ID_LEN 80     /* session ids are "session-<uuid>" today */

/* One row of token accounting. Every field is a double because the ledger
 * mixes integers and floats freely and because cost fields carry fractions. */
typedef struct {
    double input;
    double output;
    double cacheRead;
    double cacheWrite;
    double reasoning;   /* overlaps output — never part of the headline total */
    double calls;
    double cost;        /* list price, in the ledger's pricing currency */
    double apiCost;     /* what was actually billed */
    double costCny;
    double apiCostCny;
} Usage;

/* The headline number: input, cacheRead and cacheWrite are mutually exclusive
 * buckets, output is separate, reasoning is a sub-count of output and therefore
 * excluded. See docs/data-source.md. */
double usage_tokens(const Usage *u);

void usage_add(Usage *dst, const Usage *src);
void usage_zero(Usage *dst);

/* A session as recorded under one day. The same session id can appear under
 * several days; these entries are merged by ledger_reindex(). Ownership: the
 * Ledger owns title. */
typedef struct {
    char id[DSHTOK_ID_LEN];
    char *title;        /* NULL when the day did not carry one */
    double at;          /* ms since the epoch, 0 when absent */
    Usage usage;
} SessionEntry;

/* A "provider:model" row as recorded under one day. Ownership: owned by the day. */
typedef struct {
    char *name;
    Usage usage;
} ModelEntry;

typedef struct {
    char date[DSHTOK_DATE_LEN];
    Usage usage;
    SessionEntry *sessions;
    size_t session_count;
    ModelEntry *models;
    size_t model_count;
} DayRow;

/* A merged leaderboard row: one per distinct session id across all selected
 * days. The strings are owned by the Ledger that produced the row. */
typedef struct {
    char id[DSHTOK_ID_LEN];
    char date[DSHTOK_DATE_LEN];  /* earliest day the session appears under */
    double at;                   /* latest "at" timestamp seen */
    char *title;
    Usage usage;
} SessionRow;

/* A merged "provider:model" row across all selected days. */
typedef struct {
    char *name;
    Usage usage;
} ModelRow;

typedef struct {
    char currency[16];
    char symbol[32];
    int decimals;
    int has_config;
    double exchangeRate;

    DayRow *days;
    size_t day_count;
    size_t day_capacity;

    /* Derived by ledger_reindex(); rebuilt whenever the day set changes. */
    ModelRow *models;
    size_t model_count;
    SessionRow *sessions;
    size_t session_count;

    Usage total;
} Ledger;

/* Load and index a ledger file: parses the JSON, builds the day array, then
 * calls ledger_reindex() to derive the merged tables and the grand total.
 * Returns 0 on success or -1 on failure with a user facing message in err. */
int ledger_load(const char *path, Ledger *out, char *err, size_t errsz);

/* Release everything a successful ledger_load() allocated. Safe on a zeroed
 * Ledger and idempotent. */
void ledger_free(Ledger *l);

/* Recompute l->total, l->models and l->sessions from l->days. Called by
 * ledger_load() and by ledger_filter_range(); call it yourself if you mutate
 * the day array. Frees the previous derived tables first, so calling it twice
 * is safe. */
void ledger_reindex(Ledger *l);

/* Keep only the days inside [since, until]; either bound may be NULL. Bounds are
 * inclusive and validated with ledger_valid_date() first — an invalid bound is
 * caller error and makes the function return (size_t)-1 without touching the
 * ledger. On success the derived tables are rebuilt and the number of days kept
 * is returned. */
size_t ledger_filter_range(Ledger *l, const char *since, const char *until);

/* Return the merged model / session tables as a sorted copy, highest token
 * count first. Both return a freshly malloc'd array that the caller frees with
 * free(); the strings inside stay owned by the Ledger, so copying the array is
 * cheap and freeing it never frees a name. out_count may be NULL. */
ModelRow *ledger_models_sorted(const Ledger *l, size_t *out_count);
SessionRow *ledger_sessions_sorted(const Ledger *l, size_t *out_count);

/* True when s looks like "YYYY-MM-DD" — four digits, two digits, two digits, all
 * numeric. This is a shape check only: "2026-13-45" passes. It exists to reject
 * typos in --since/--until before they are used as comparison keys. */
int ledger_valid_date(const char *s);

#endif /* DSHTOK_LEDGER_H */
