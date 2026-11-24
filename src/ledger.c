/* ledger.c — read the cost-meter ledger and turn it into rows.
 *
 * The ledger is a JSON object whose `days` member is keyed by "YYYY-MM-DD".
 * Each day carries the authoritative per-day sums plus two breakdowns: the
 * sessions that ran that day, and the "provider:model" split. Only the day
 * totals are used for the grand total; the breakdowns are merged across days to
 * build the leaderboards.
 *
 * Everything the tool reports comes from this one file. Nothing is recomputed
 * from raw request events: those caches are pruned over time and would silently
 * under-report. See docs/data-source.md for why.
 */

#include "dshtok/ledger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dshtok/json.h"
#include "dshtok/util.h"

/* --- Usage ---------------------------------------------------------------- */

double usage_tokens(const Usage *u)
{
    if (u == NULL) {
        return 0.0;
    }
    /* input / cacheRead / cacheWrite are mutually exclusive buckets and output is
     * a separate stream, so this sum never double counts. reasoning is a sub
     * count of output and is deliberately left out. */
    return u->input + u->output + u->cacheRead + u->cacheWrite;
}

void usage_zero(Usage *u)
{
    if (u == NULL) {
        return;
    }
    memset(u, 0, sizeof *u);
}

void usage_add(Usage *dst, const Usage *src)
{
    if (dst == NULL || src == NULL) {
        return;
    }
    dst->input += src->input;
    dst->output += src->output;
    dst->cacheRead += src->cacheRead;
    dst->cacheWrite += src->cacheWrite;
    dst->reasoning += src->reasoning;
    dst->calls += src->calls;
    dst->cost += src->cost;
    dst->apiCost += src->apiCost;
    dst->costCny += src->costCny;
    dst->apiCostCny += src->apiCostCny;
}

/* Read every recognised numeric field of one accounting object. Unknown members
 * are ignored, which is what keeps a future ledger version from crashing us. */
static void usage_from_json(const JsonValue *obj, Usage *out)
{
    usage_zero(out);
    if (obj == NULL || obj->type != JSON_OBJECT) {
        return;
    }
    out->input = json_number(json_get(obj, "input"), 0.0);
    out->output = json_number(json_get(obj, "output"), 0.0);
    out->cacheRead = json_number(json_get(obj, "cacheRead"), 0.0);
    out->cacheWrite = json_number(json_get(obj, "cacheWrite"), 0.0);
    out->reasoning = json_number(json_get(obj, "reasoning"), 0.0);
    out->calls = json_number(json_get(obj, "calls"), 0.0);
    out->cost = json_number(json_get(obj, "cost"), 0.0);
    out->apiCost = json_number(json_get(obj, "apiCost"), 0.0);
    out->costCny = json_number(json_get(obj, "costCny"), 0.0);
    out->apiCostCny = json_number(json_get(obj, "apiCostCny"), 0.0);
}

/* --- date helpers --------------------------------------------------------- */

int ledger_valid_date(const char *s)
{
    int i;
    if (s == NULL) {
        return 0;
    }
    for (i = 0; i < 10; ++i) {
        char c = s[i];
        if (i == 4 || i == 7) {
            if (c != '-') {
                return 0;
            }
        } else if (c < '0' || c > '9') {
            return 0;
        }
    }
    /* Exactly ten characters: a longer string is not a date, it is a typo we
     * should refuse rather than truncate into one. */
    return s[10] == '\0';
}

static int cmp_day_date(const void *a, const void *b)
{
    const DayRow *x = (const DayRow *)a;
    const DayRow *y = (const DayRow *)b;
    return strcmp(x->date, y->date);
}

/* --- day construction ----------------------------------------------------- */

static DayRow *ledger_push_day(Ledger *l)
{
    if (l->day_count == l->day_capacity) {
        size_t next = (l->day_capacity == 0) ? 32 : l->day_capacity * 2;
        l->days = (DayRow *)xrealloc(l->days, next * sizeof *l->days);
        l->day_capacity = next;
    }
    memset(&l->days[l->day_count], 0, sizeof l->days[0]);
    return &l->days[l->day_count++];
}

static void day_read_sessions(DayRow *day, const JsonValue *arr)
{
    size_t i;
    if (arr == NULL || arr->type != JSON_ARRAY || arr->item_count == 0) {
        return;
    }
    day->sessions = (SessionEntry *)xcalloc(arr->item_count, sizeof *day->sessions);
    for (i = 0; i < arr->item_count; ++i) {
        const JsonValue *item = arr->items[i];
        const char *id;
        SessionEntry *entry;
        if (item == NULL || item->type != JSON_OBJECT) {
            continue;
        }
        id = json_string(json_get(item, "id"), NULL);
        if (id == NULL || id[0] == '\0') {
            continue; /* a session with no id cannot be merged or shown */
        }
        entry = &day->sessions[day->session_count++];
        str_copy(entry->id, sizeof entry->id, id);
        entry->at = json_number(json_get(item, "at"), 0.0);
        {
            const char *title = json_string(json_get(item, "title"), NULL);
            entry->title = (title != NULL && title[0] != '\0') ? xstrdup(title) : NULL;
        }
        usage_from_json(item, &entry->usage);
    }
}

static void day_read_models(DayRow *day, const JsonValue *obj)
{
    size_t i;
    if (obj == NULL || obj->type != JSON_OBJECT || obj->member_count == 0) {
        return;
    }
    day->models = (ModelEntry *)xcalloc(obj->member_count, sizeof *day->models);
    for (i = 0; i < obj->member_count; ++i) {
        const JsonMember *m = &obj->members[i];
        ModelEntry *entry;
        if (m->key == NULL || m->key[0] == '\0' || m->value == NULL) {
            continue;
        }
        if (m->value->type != JSON_OBJECT) {
            continue;
        }
        entry = &day->models[day->model_count++];
        entry->name = xstrdup(m->key);
        usage_from_json(m->value, &entry->usage);
    }
}

static void day_free(DayRow *day)
{
    size_t i;
    if (day == NULL) {
        return;
    }
    for (i = 0; i < day->session_count; ++i) {
        free(day->sessions[i].title);
        day->sessions[i].title = NULL;
    }
    free(day->sessions);
    day->sessions = NULL;
    day->session_count = 0;

    for (i = 0; i < day->model_count; ++i) {
        free(day->models[i].name);
        day->models[i].name = NULL;
    }
    free(day->models);
    day->models = NULL;
    day->model_count = 0;
}

/* --- merging -------------------------------------------------------------- */

typedef struct {
    char id[DSHTOK_ID_LEN];
    char date[DSHTOK_DATE_LEN];
    double at;
    char *title;
    Usage usage;
} TempSession;

typedef struct {
    const char *name;
    Usage usage;
} TempModel;

static int cmp_temp_session(const void *a, const void *b)
{
    const TempSession *x = (const TempSession *)a;
    const TempSession *y = (const TempSession *)b;
    return strcmp(x->id, y->id);
}

static int cmp_temp_model(const void *a, const void *b)
{
    const TempModel *x = (const TempModel *)a;
    const TempModel *y = (const TempModel *)b;
    return strcmp(x->name, y->name);
}

static int cmp_model_row(const void *a, const void *b)
{
    const ModelRow *x = (const ModelRow *)a;
    const ModelRow *y = (const ModelRow *)b;
    double tx = usage_tokens(&x->usage);
    double ty = usage_tokens(&y->usage);
    if (tx > ty) {
        return -1;
    }
    if (tx < ty) {
        return 1;
    }
    return strcmp(x->name, y->name);
}

static int cmp_session_row(const void *a, const void *b)
{
    const SessionRow *x = (const SessionRow *)a;
    const SessionRow *y = (const SessionRow *)b;
    double tx = usage_tokens(&x->usage);
    double ty = usage_tokens(&y->usage);
    if (tx > ty) {
        return -1;
    }
    if (tx < ty) {
        return 1;
    }
    /* Stable tie break so the leaderboard does not shuffle between runs. */
    return strcmp(x->id, y->id);
}

static void ledger_build_models(Ledger *l)
{
    size_t total = 0;
    size_t i;
    size_t d;
    TempModel *tmp;
    size_t pos = 0;
    size_t out = 0;

    for (d = 0; d < l->day_count; ++d) {
        total += l->days[d].model_count;
    }
    if (total == 0) {
        return;
    }

    tmp = (TempModel *)xcalloc(total, sizeof *tmp);
    for (d = 0; d < l->day_count; ++d) {
        for (i = 0; i < l->days[d].model_count; ++i) {
            tmp[pos].name = l->days[d].models[i].name;
            tmp[pos].usage = l->days[d].models[i].usage;
            ++pos;
        }
    }
    qsort(tmp, pos, sizeof *tmp, cmp_temp_model);

    l->models = (ModelRow *)xcalloc(pos, sizeof *l->models);
    i = 0;
    while (i < pos) {
        size_t run_end = i + 1;
        ModelRow *row = &l->models[out++];
        row->name = (char *)tmp[i].name;
        row->usage = tmp[i].usage;
        while (run_end < pos && strcmp(tmp[run_end].name, tmp[i].name) == 0) {
            usage_add(&row->usage, &tmp[run_end].usage);
            ++run_end;
        }
        i = run_end;
    }
    l->model_count = out;
    free(tmp);
    qsort(l->models, l->model_count, sizeof *l->models, cmp_model_row);
}

static void ledger_build_sessions(Ledger *l)
{
    size_t total = 0;
    size_t i;
    size_t d;
    TempSession *tmp;
    size_t pos = 0;
    size_t out = 0;

    for (d = 0; d < l->day_count; ++d) {
        total += l->days[d].session_count;
    }
    if (total == 0) {
        return;
    }

    tmp = (TempSession *)xcalloc(total, sizeof *tmp);
    for (d = 0; d < l->day_count; ++d) {
        const DayRow *day = &l->days[d];
        for (i = 0; i < day->session_count; ++i) {
            const SessionEntry *e = &day->sessions[i];
            TempSession *t = &tmp[pos++];
            str_copy(t->id, sizeof t->id, e->id);
            str_copy(t->date, sizeof t->date, day->date);
            t->at = e->at;
            t->title = e->title;
            t->usage = e->usage;
        }
    }
    qsort(tmp, pos, sizeof *tmp, cmp_temp_session);

    l->sessions = (SessionRow *)xcalloc(pos, sizeof *l->sessions);
    i = 0;
    while (i < pos) {
        size_t run_end = i + 1;
        SessionRow *row = &l->sessions[out++];
        str_copy(row->id, sizeof row->id, tmp[i].id);
        str_copy(row->date, sizeof row->date, tmp[i].date);
        row->at = tmp[i].at;
        row->title = tmp[i].title;
        row->usage = tmp[i].usage;
        while (run_end < pos && strcmp(tmp[run_end].id, tmp[i].id) == 0) {
            const TempSession *t = &tmp[run_end];
            usage_add(&row->usage, &t->usage);
            /* Keep the earliest day (the array is not date-sorted within a run,
             * so compare) and the latest timestamp. */
            if (strcmp(t->date, row->date) < 0) {
                str_copy(row->date, sizeof row->date, t->date);
            }
            if (t->at > row->at) {
                row->at = t->at;
            }
            if (row->title == NULL && t->title != NULL) {
                row->title = t->title;
            }
            ++run_end;
        }
        i = run_end;
    }
    l->session_count = out;
    free(tmp);
}

void ledger_reindex(Ledger *l)
{
    size_t d;

    if (l == NULL) {
        return;
    }

    free(l->models);
    l->models = NULL;
    l->model_count = 0;

    free(l->sessions);
    l->sessions = NULL;
    l->session_count = 0;

    usage_zero(&l->total);
    for (d = 0; d < l->day_count; ++d) {
        usage_add(&l->total, &l->days[d].usage);
    }

    ledger_build_models(l);
    ledger_build_sessions(l);
}

/* --- loading -------------------------------------------------------------- */

static void ledger_clear(Ledger *l)
{
    if (l == NULL) {
        return;
    }
    memset(l, 0, sizeof *l);
    l->decimals = 2;
    str_copy(l->currency, sizeof l->currency, "");
    str_copy(l->symbol, sizeof l->symbol, "");
}

void ledger_free(Ledger *l)
{
    size_t d;
    if (l == NULL) {
        return;
    }
    for (d = 0; d < l->day_count; ++d) {
        day_free(&l->days[d]);
    }
    free(l->days);
    free(l->models);
    free(l->sessions);
    ledger_clear(l);
}

static void ledger_read_config(Ledger *l, const JsonValue *config)
{
    const char *currency;
    const char *symbol;

    if (config == NULL || config->type != JSON_OBJECT) {
        return;
    }
    l->has_config = 1;

    currency = json_string(json_get(config, "currency"), NULL);
    if (currency != NULL && currency[0] != '\0') {
        str_copy(l->currency, sizeof l->currency, currency);
    }
    symbol = json_string(json_get(config, "symbol"), NULL);
    if (symbol != NULL && symbol[0] != '\0') {
        str_copy(l->symbol, sizeof l->symbol, symbol);
    }
    {
        long long decimals = json_integer(json_get(config, "decimals"), 2);
        if (decimals < 0) {
            decimals = 0;
        }
        if (decimals > 6) {
            decimals = 6;
        }
        l->decimals = (int)decimals;
    }
    l->exchangeRate = json_number(json_get(config, "exchangeRate"), 0.0);
}

int ledger_load(const char *path, Ledger *out, char *err, size_t errsz)
{
    char *text = NULL;
    size_t text_len = 0;
    JsonValue *root = NULL;
    const JsonValue *days;
    size_t i;

    if (out == NULL) {
        return -1;
    }
    ledger_clear(out);
    if (err != NULL && errsz > 0) {
        err[0] = '\0';
    }

    text = read_entire_file(path, &text_len, err, errsz);
    if (text == NULL) {
        return -1;
    }

    root = json_parse(text, text_len, err, errsz);
    free(text); /* the DOM owns copies of every string it kept */
    if (root == NULL) {
        return -1;
    }

    if (root->type != JSON_OBJECT) {
        str_copy(err, errsz, "ledger root is not a JSON object");
        json_free(root);
        return -1;
    }

    ledger_read_config(out, json_get(root, "config"));

    days = json_get(root, "days");
    if (days == NULL) {
        /* A ledger with no `days` is not corrupt, it is empty: the plugin has
         * been installed but has not recorded a request yet. Report zeroes so
         * the user sees "0 tokens" instead of an error. */
        str_copy(err, errsz, "");
        json_free(root);
        return 0;
    }
    if (days->type != JSON_OBJECT) {
        str_copy(err, errsz, "ledger \"days\" is not an object");
        json_free(root);
        return -1;
    }

    for (i = 0; i < days->member_count; ++i) {
        const JsonMember *m = &days->members[i];
        DayRow *day;
        if (m->key == NULL || !ledger_valid_date(m->key)) {
            continue; /* not a day: ignore rather than guess */
        }
        if (m->value == NULL || m->value->type != JSON_OBJECT) {
            continue;
        }
        day = ledger_push_day(out);
        str_copy(day->date, sizeof day->date, m->key);
        usage_from_json(m->value, &day->usage);
        day_read_sessions(day, json_get(m->value, "sessions"));
        day_read_models(day, json_get(m->value, "byProviderModel"));
    }
    json_free(root);

    /* The host writes days in insertion order, but a hand-edited or merged
     * ledger might not. Sorting keeps every "first day"/"last day" honest. */
    if (out->day_count > 1) {
        qsort(out->days, out->day_count, sizeof *out->days, cmp_day_date);
    }

    ledger_reindex(out);
    return 0;
}

/* --- filtering and sorted views ------------------------------------------- */

size_t ledger_filter_range(Ledger *l, const char *since, const char *until)
{
    size_t read;
    size_t write;
    size_t kept = 0;

    if (l == NULL) {
        return (size_t)-1;
    }
    if (since != NULL && since[0] != '\0' && !ledger_valid_date(since)) {
        return (size_t)-1;
    }
    if (until != NULL && until[0] != '\0' && !ledger_valid_date(until)) {
        return (size_t)-1;
    }

    write = 0;
    for (read = 0; read < l->day_count; ++read) {
        DayRow *day = &l->days[read];
        int keep = 1;
        if (since != NULL && since[0] != '\0' && strcmp(day->date, since) < 0) {
            keep = 0;
        }
        if (until != NULL && until[0] != '\0' && strcmp(day->date, until) > 0) {
            keep = 0;
        }
        if (!keep) {
            day_free(day);
            continue;
        }
        if (write != read) {
            l->days[write] = l->days[read];
            memset(&l->days[read], 0, sizeof l->days[0]);
        }
        ++write;
        ++kept;
    }
    l->day_count = write;

    ledger_reindex(l);
    return kept;
}

ModelRow *ledger_models_sorted(const Ledger *l, size_t *out_count)
{
    ModelRow *copy;
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (l == NULL || l->model_count == 0) {
        return NULL;
    }
    copy = (ModelRow *)xmalloc(l->model_count * sizeof *copy);
    memcpy(copy, l->models, l->model_count * sizeof *copy);
    qsort(copy, l->model_count, sizeof *copy, cmp_model_row);
    if (out_count != NULL) {
        *out_count = l->model_count;
    }
    return copy;
}

SessionRow *ledger_sessions_sorted(const Ledger *l, size_t *out_count)
{
    SessionRow *copy;
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (l == NULL || l->session_count == 0) {
        return NULL;
    }
    copy = (SessionRow *)xmalloc(l->session_count * sizeof *copy);
    memcpy(copy, l->sessions, l->session_count * sizeof *copy);
    qsort(copy, l->session_count, sizeof *copy, cmp_session_row);
    if (out_count != NULL) {
        *out_count = l->session_count;
    }
    return copy;
}
