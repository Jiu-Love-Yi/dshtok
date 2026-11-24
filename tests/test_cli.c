/* End to end tests for the compiled dshtok binary.
 *
 * These tests drive the built binary as a child process and inspect its exit
 * code and its two output streams, which is the only way to pin down the
 * contract in SPEC.md:
 *   exit 0 success, exit 1 runtime error, exit 2 usage error.
 *
 * The ledger under test is a private copy of tests/fixtures/ledger_basic.json
 * placed in build/tmp, so no test can observe leftovers from a previous run and
 * no test can ever reach the user's real ~/.dsh ledger. DSH_HOME is also
 * pointed at an empty scratch directory, and DSHTOK_LEDGER is cleared, so the
 * binary cannot auto-detect anything outside this directory.
 *
 * When the binary has not been built the whole file reports "skipped" and exits
 * 0, so `make test` still works on a checkout that has not been compiled yet. */

/* setenv/unsetenv/mkdir are POSIX, and -std=c99 hides their declarations on
 * glibc unless this is asked for before the first system header. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "dshtok/version.h"
#include "dshtok/json.h"
#include "mini_test.h"

#define BIG 8192

/* The executable is spelled with a backslash on Windows: cmd.exe reads
 * "build/dshtok.exe" as the command "build" plus a /dshtok.exe switch. */
#ifdef _WIN32
static const char *BINARY = "build\\dshtok.exe";
#else
static const char *BINARY = "build/dshtok.exe";
#endif

static const char *FIXTURE = "tests/fixtures/ledger_basic.json";
static const char *TMP_LEDGER = "build/tmp/cli-ledger.json";
static const char *MISSING_LEDGER = "build/tmp/cli-does-not-exist.json";
static const char *MALFORMED_LEDGER = "tests/fixtures/ledger_malformed.json";

/* ------------------------------------------------------------------ */
/* deterministic environment                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    char *value; /* NULL when the variable was unset */
} EnvSave;

static char *env_get(const char *name)
{
    const char *v = getenv(name);
    if (v == NULL) {
        return NULL;
    }
    {
        size_t n = strlen(v);
        char *copy = (char *)malloc(n + 1);
        if (copy != NULL) {
            memcpy(copy, v, n + 1);
        }
        return copy;
    }
}

static void env_set(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value == NULL ? "" : value);
#else
    if (value == NULL) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

/* Remember a variable before env_isolate() overwrites it. */
static void env_save(EnvSave *saved, const char *name)
{
    saved->name = name;
    saved->value = env_get(name);
}

/* Put back whatever env_save() remembered, then free the copy. */
static void env_restore(EnvSave *saved)
{
    env_set(saved->name, saved->value);
    free(saved->value);
    saved->value = NULL;
}

/* Redirect the environment so the child can only see the scratch home and
 * cannot fall back to the user's real ~/.dsh ledger. */
static void env_isolate(EnvSave *home, EnvSave *ledger)
{
    env_save(home, "DSH_HOME");
    env_save(ledger, "DSHTOK_LEDGER");
    env_set("DSH_HOME", "build/tmp/cli-home");
    env_set("DSHTOK_LEDGER", NULL);
}

/* ------------------------------------------------------------------ */
/* scratch directory and fixture copy                                  */
/* ------------------------------------------------------------------ */

static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

static int copy_file(const char *from, const char *to)
{
    FILE *in;
    FILE *out;
    char chunk[4096];
    size_t n;

    in = fopen(from, "rb");
    if (in == NULL) {
        return -1;
    }
    out = fopen(to, "wb");
    if (out == NULL) {
        fclose(in);
        return -1;
    }
    while ((n = fread(chunk, 1, sizeof chunk, in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) {
            fclose(in);
            fclose(out);
            return -1;
        }
    }
    fclose(in);
    fclose(out);
    return 0;
}

/* Create build/tmp and put a fresh copy of the fixture there, so that every
 * run starts from the same ledger no matter what a previous run did. */
static int prepare_scratch(void)
{
    make_dir("build");
    make_dir("build/tmp");
    make_dir("build/tmp/cli-home");
    return copy_file(FIXTURE, TMP_LEDGER);
}

/* ------------------------------------------------------------------ */
/* child process plumbing                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int exit_code;
    char out[BIG];
    char err[BIG];
} Run;

/* Run `binary args >out 2>err` and record everything. Both streams go through
 * files so that stdout and stderr stay separable: the contract says reports go
 * to stdout and diagnostics to stderr. */
static int run_capture(const char *args, Run *r)
{
    char command[BIG * 2];
    int rc;

    memset(r, 0, sizeof *r);
    r->exit_code = -1;

    snprintf(command, sizeof command,
             "%s %s >build/tmp/cli-out.txt 2>build/tmp/cli-err.txt",
             BINARY, args);

    rc = system(command);
    if (rc == -1) {
        return -1;
    }
#ifdef _WIN32
    r->exit_code = rc;
#else
    r->exit_code = (rc >> 8) & 0xFF;
#endif

    {
        FILE *f = fopen("build/tmp/cli-out.txt", "rb");
        if (f != NULL) {
            size_t n = fread(r->out, 1, sizeof r->out - 1, f);
            r->out[n] = '\0';
            fclose(f);
        }
    }
    {
        FILE *f = fopen("build/tmp/cli-err.txt", "rb");
        if (f != NULL) {
            size_t n = fread(r->err, 1, sizeof r->err - 1, f);
            r->err[n] = '\0';
            fclose(f);
        }
    }
    return 0;
}

/* Convenience: run one command against the scratch ledger. */
static int run_ledger(const char *args, Run *r)
{
    char full[BIG * 2];
    snprintf(full, sizeof full, "--path %s %s", TMP_LEDGER, args);
    return run_capture(full, r);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static void t_version(void)
{
    Run r;

    CHECK(run_capture("--version", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(strstr(r.out, DSHTOK_VERSION_STRING) != NULL);
}

static void t_help(void)
{
    Run r;

    CHECK(run_capture("--help", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    /* Help has to name the commands and the options a user is expected to
     * discover from it. */
    CHECK(strstr(r.out, "total") != NULL);
    CHECK(strstr(r.out, "sessions") != NULL);
    CHECK(strstr(r.out, "--since") != NULL);
}

static void t_unknown_option_is_usage_error(void)
{
    Run r;

    CHECK(run_capture("--no-such-option", &r) == 0);
    /* Exit 2: the user typed something the parser does not understand. */
    CHECK_EQ_INT(r.exit_code, 2);
    CHECK(r.err[0] != '\0');
}

static void t_unknown_command_is_usage_error(void)
{
    Run r;

    CHECK(run_capture("frobnicate", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 2);
    CHECK(r.err[0] != '\0');
}

static void t_missing_ledger_is_runtime_error(void)
{
    Run r;
    char args[BIG];

    snprintf(args, sizeof args, "--path %s total", MISSING_LEDGER);
    CHECK(run_capture(args, &r) == 0);
    /* Exit 1: the command was well formed but the data is not there. */
    CHECK_EQ_INT(r.exit_code, 1);
    CHECK(r.err[0] != '\0');
}

static void t_malformed_ledger_is_runtime_error(void)
{
    Run r;
    char args[BIG];

    snprintf(args, sizeof args, "--path %s total", MALFORMED_LEDGER);
    CHECK(run_capture(args, &r) == 0);
    CHECK_EQ_INT(r.exit_code, 1);
    CHECK(r.err[0] != '\0');
}

static void t_json_total(void)
{
    Run r;

    CHECK(run_ledger("--json total", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    /* A JSON document starts with an object and carries the headline total. */
    CHECK(r.out[0] == '{');
    CHECK(strstr(r.out, "\"total\"") != NULL);
    CHECK(strstr(r.out, "1100") != NULL);
}

static void t_json_total_parses(void)
{
    Run r;
    JsonValue *root;

    CHECK(run_ledger("--json total", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);

    /* The output claims to be JSON, so it must actually parse. This catches
     * emitters that forget a comma, double a comma or leave a key open. */
    root = json_parse(r.out, strlen(r.out), NULL, 0);
    CHECK_MSG(root != NULL, "%s", "--json total did not produce valid JSON");
    if (root != NULL) {
        CHECK(json_is(root, JSON_OBJECT));
        CHECK(json_get(root, "total") != NULL);
        CHECK_NEAR(json_number(json_get(json_get(root, "total"), "tokens"), -1),
                   1100.0, 0.001);
        json_free(root);
    }
}

static void t_json_models(void)
{
    Run r;

    CHECK(run_ledger("--json models", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(r.out[0] == '{');
    CHECK(strstr(r.out, "deepseek:chat") != NULL);
}

static void t_json_models_parses(void)
{
    Run r;
    JsonValue *root;

    CHECK(run_ledger("--json models", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);

    root = json_parse(r.out, strlen(r.out), NULL, 0);
    CHECK_MSG(root != NULL, "%s", "--json models did not produce valid JSON");
    if (root != NULL) {
        const JsonValue *models = json_get(root, "models");
        CHECK(json_is(models, JSON_ARRAY));
        CHECK(json_at(models, 0) != NULL);
        json_free(root);
    }
}

static void t_json_sessions_parses(void)
{
    Run r;
    JsonValue *root;

    CHECK(run_ledger("--json sessions", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);

    root = json_parse(r.out, strlen(r.out), NULL, 0);
    CHECK_MSG(root != NULL, "%s", "--json sessions did not produce valid JSON");
    if (root != NULL) {
        const JsonValue *sessions = json_get(root, "sessions");
        CHECK(json_is(sessions, JSON_ARRAY));
        CHECK(json_at(sessions, 0) != NULL);
        json_free(root);
    }
}

static void t_json_daily_parses(void)
{
    Run r;
    JsonValue *root;

    CHECK(run_ledger("--json daily", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);

    root = json_parse(r.out, strlen(r.out), NULL, 0);
    CHECK_MSG(root != NULL, "%s", "--json daily did not produce valid JSON");
    json_free(root);
}

static void t_quiet_prints_only_a_number(void)
{
    Run r;
    const char *p;
    int only_number = 1;
    int digits = 0;

    CHECK(run_ledger("-q total", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);

    /* -q means machine readable: one line, nothing but the number. */
    p = r.out;
    while (*p != '\0' && *p != '\r' && *p != '\n') {
        if ((*p >= '0' && *p <= '9') || *p == ',' || *p == '.') {
            if (*p >= '0' && *p <= '9') {
                ++digits;
            }
        } else {
            only_number = 0;
        }
        ++p;
    }
    CHECK(only_number);
    CHECK(digits > 0);

    /* Nothing of substance is printed after the first line. */
    while (*p == '\r' || *p == '\n') {
        ++p;
    }
    CHECK_EQ_STR(p, "");
}

static void t_quiet_matches_json_total(void)
{
    Run quiet;
    Run json;

    CHECK(run_ledger("-q total", &quiet) == 0);
    CHECK(run_ledger("--json total", &json) == 0);
    CHECK_EQ_INT(quiet.exit_code, 0);
    CHECK_EQ_INT(json.exit_code, 0);

    /* The two machine readable forms must agree on the headline number. */
    {
        JsonValue *root = json_parse(json.out, strlen(json.out), NULL, 0);
        char expected[64];
        if (root != NULL) {
            double tokens =
                json_number(json_get(json_get(root, "total"), "tokens"), -1);
            snprintf(expected, sizeof expected, "%.0f", tokens);
            CHECK_MSG(strncmp(quiet.out, expected, strlen(expected)) == 0,
                      "quiet printed '%s' but json total is %s",
                      quiet.out, expected);
            json_free(root);
        }
    }
}

static void t_csv_output(void)
{
    Run r;

    CHECK(run_ledger("--csv daily", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(strchr(r.out, ',') != NULL);
    CHECK(strstr(r.out, "2026-01-10") != NULL);
    /* A CSV header names the columns. */
    CHECK(strstr(r.out, "date") != NULL);
}

static void t_since_until_filtering(void)
{
    Run r;
    JsonValue *root;

    /* Both bounds inclusive: day 10 and day 11 both survive. */
    CHECK(run_ledger("--since 2026-01-10 --until 2026-01-11 --json total", &r)
          == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(r.out[0] == '{');

    root = json_parse(r.out, strlen(r.out), NULL, 0);
    if (root != NULL) {
        CHECK_NEAR(json_number(json_get(root, "days_count"), -1), 2.0, 0.001);
        CHECK_NEAR(json_number(json_get(json_get(root, "total"), "tokens"), -1),
                   1100.0, 0.001);
        json_free(root);
    }

    /* A window outside the data is a successful empty report, not an error. */
    CHECK(run_ledger("--since 2030-01-01 --until 2030-12-31 --json total", &r)
          == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(r.out[0] == '{');

    root = json_parse(r.out, strlen(r.out), NULL, 0);
    if (root != NULL) {
        CHECK_NEAR(json_number(json_get(root, "days_count"), -1), 0.0, 0.001);
        CHECK_NEAR(json_number(json_get(json_get(root, "total"), "tokens"), -1),
                   0.0, 0.001);
        json_free(root);
    }
}

static void t_single_day_window(void)
{
    Run r;
    JsonValue *root;

    CHECK(run_ledger("--since 2026-01-11 --until 2026-01-11 --json total", &r)
          == 0);
    CHECK_EQ_INT(r.exit_code, 0);

    root = json_parse(r.out, strlen(r.out), NULL, 0);
    if (root != NULL) {
        /* One day only, and only that day's 100 tokens. */
        CHECK_NEAR(json_number(json_get(root, "days_count"), -1), 1.0, 0.001);
        CHECK_NEAR(json_number(json_get(json_get(root, "total"), "tokens"), -1),
                   100.0, 0.001);
        json_free(root);
    }
}

static void t_invalid_date_is_usage_error(void)
{
    Run r;

    CHECK(run_ledger("--since 01/10/2026 --json total", &r) == 0);
    /* A mistyped date is caught before the ledger is read. */
    CHECK_EQ_INT(r.exit_code, 2);
    CHECK(r.err[0] != '\0');
}

/* A date can have the right shape and still name no day. Those two cases need
 * different tests in the code -- `ledger_valid_date` only checks the shape --
 * so they need different tests here too: if the strict check ever regresses to
 * the lenient one, the month and leap-day cases below are what catch it. */
static void t_impossible_dates_are_usage_errors(void)
{
    static const char *const k_impossible[] = {
        "2026-13-99", /* month 13 */
        "2026-00-10", /* month 0 */
        "2026-01-00", /* day 0 */
        "2026-02-30", /* February has no 30th */
        "2025-02-29", /* 2025 is not a leap year */
        "2026-04-31", /* April has 30 days */
    };
    size_t i;

    for (i = 0; i < sizeof k_impossible / sizeof k_impossible[0]; ++i) {
        Run r;
        char args[64];
        snprintf(args, sizeof args, "--since %s --json total", k_impossible[i]);
        CHECK(run_ledger(args, &r) == 0);
        CHECK_MSG(r.exit_code == 2, "%s", k_impossible[i]);
        CHECK_MSG(r.err[0] != '\0', "%s", k_impossible[i]);
    }
}

/* The strictness must not overshoot: these are real days and must keep working,
 * including the leap day that a naive year % 4 test gets right by luck and a
 * naive day <= 28 test gets wrong. */
static void t_real_dates_are_accepted(void)
{
    static const char *const k_real[] = {
        "2024-02-29", /* a real leap day */
        "2000-02-29", /* divisible by 400, so real */
        "2026-12-31",
        "1999-01-01",
    };
    size_t i;

    for (i = 0; i < sizeof k_real / sizeof k_real[0]; ++i) {
        Run r;
        char args[64];
        snprintf(args, sizeof args, "--since %s --json total", k_real[i]);
        CHECK(run_ledger(args, &r) == 0);
        CHECK_MSG(r.exit_code == 0, "%s", k_real[i]);
    }
}

static void t_bad_top_value_is_usage_error(void)
{
    Run r;

    CHECK(run_ledger("--top notanumber sessions", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 2);
    CHECK(r.err[0] != '\0');
}

static void t_all_commands_run(void)
{
    static const char *const commands[] = {
        "total", "daily", "models", "sessions", "doctor", "help"
    };
    size_t i;

    for (i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        Run r;
        char args[BIG];

        snprintf(args, sizeof args, "--no-color %s", commands[i]);
        CHECK(run_ledger(args, &r) == 0);
        CHECK_MSG(r.exit_code == 0, "command '%s' failed", commands[i]);
        CHECK(r.out[0] != '\0');
    }
}

static void t_doctor_without_ledger_still_succeeds(void)
{
    Run r;
    char args[BIG];

    /* doctor inspects the search itself, so an absent ledger is its finding,
     * not a runtime error. */
    snprintf(args, sizeof args, "--path %s doctor", MISSING_LEDGER);
    CHECK(run_capture(args, &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(r.out[0] != '\0');
}

static void t_quiet_and_json_together(void)
{
    Run r;

    /* Both ask for machine readable output; either way the process must not
     * fail and must not print a table header. */
    CHECK(run_ledger("-q --json total", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(strstr(r.out, "Total") == NULL);
}

static void t_no_header_and_json_agree(void)
{
    Run plain;
    Run bare;

    CHECK(run_ledger("--json total", &plain) == 0);
    CHECK(run_ledger("--no-header --json total", &bare) == 0);
    CHECK_EQ_INT(plain.exit_code, 0);
    CHECK_EQ_INT(bare.exit_code, 0);
    /* --no-header only affects the human table, so the JSON is unchanged. */
    CHECK_EQ_STR(bare.out, plain.out);
}

/* ------------------------------------------------------------------ */
/* doctor: ledger size                                                 */
/* ------------------------------------------------------------------ */

/* Byte count of a file, or -1. Measured rather than hard coded, so the
 * expectation follows the fixture instead of a number pasted into the test. */
static long long file_bytes(const char *path)
{
    FILE *f;
    long long size;

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
    return size;
}

/* Group a count with commas the way the report does. */
static void group_commas(unsigned long long value, char *dst, size_t dstsz)
{
    char digits[32];
    size_t nd;
    size_t out;
    size_t i;

    snprintf(digits, sizeof digits, "%llu", value);
    nd = strlen(digits);
    out = 0;
    for (i = 0; i < nd && out + 2 < dstsz; ++i) {
        if (i > 0 && (nd - i) % 3 == 0) {
            dst[out++] = ',';
        }
        dst[out++] = digits[i];
    }
    dst[out] = '\0';
}

/* Copy the one report line whose text contains `label`. Asserting against the
 * line instead of the whole report keeps a match from drifting to another
 * field. */
static int doctor_line(const Run *r, const char *label, char *dst, size_t dstsz)
{
    const char *p = r->out;

    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        const char *word = strstr(p, label);

        if (word != NULL && word < p + len) {
            if (len >= dstsz) {
                len = dstsz - 1;
            }
            memcpy(dst, p, len);
            dst[len] = '\0';
            return 1;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }
    return 0;
}

static void t_doctor_reports_ledger_size(void)
{
    Run r;
    char line[BIG];
    char grouped[64];
    long long bytes;

    bytes = file_bytes(TMP_LEDGER);
    CHECK(bytes > 1024);
    if (bytes <= 0) {
        return;
    }
    group_commas((unsigned long long)bytes, grouped, sizeof grouped);

    CHECK(run_ledger("doctor", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(doctor_line(&r, "size", line, sizeof line) == 1);
    /* The exact byte count is printed, comma grouped, so it can be compared
     * with what a file manager says. */
    CHECK(strstr(line, grouped) != NULL);
    /* Above one kilobyte a 1024-based human form follows in parentheses. */
    CHECK(strstr(line, " bytes (") != NULL);
    CHECK(strstr(line, " KB)") != NULL);
    /* A ledger that is present is never described as unknown. */
    CHECK(strstr(line, "unknown") == NULL);
}

static void t_doctor_size_is_unknown_when_absent(void)
{
    Run r;
    char args[BIG];
    char line[BIG];

    snprintf(args, sizeof args, "--path %s doctor", MISSING_LEDGER);
    CHECK(run_capture(args, &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(doctor_line(&r, "size", line, sizeof line) == 1);
    /* "unknown", not 0: telling "not there" apart from "empty" is exactly what
     * doctor is for. */
    CHECK(strstr(line, "unknown") != NULL);
    CHECK(strstr(line, "0 bytes") == NULL);
}

static void t_doctor_size_under_one_kb_prints_once(void)
{
    char path[] = "build/tmp/cli-tiny.json";
    size_t i;
    FILE *f;
    Run r;
    char args[BIG];
    char line[BIG];
    long long bytes;
    char grouped[64];

    f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f == NULL) {
        return;
    }
    for (i = 0; i < 512; ++i) {
        fputc('x', f);
    }
    fclose(f);

    bytes = file_bytes(path);
    CHECK_EQ_INT((int)bytes, 512);
    if (bytes != 512) {
        remove(path);
        return;
    }
    group_commas((unsigned long long)bytes, grouped, sizeof grouped);

    snprintf(args, sizeof args, "--path %s doctor", path);
    CHECK(run_capture(args, &r) == 0);
    CHECK_EQ_INT(r.exit_code, 0);
    CHECK(doctor_line(&r, "size", line, sizeof line) == 1);
    CHECK(strstr(line, grouped) != NULL);
    CHECK(strstr(line, "bytes") != NULL);
    /* Under a kilobyte the human form already is the exact count, so the
     * renderer must not repeat it as "512 bytes (512 bytes)". */
    CHECK_MSG(strstr(line, "(") == NULL, "redundant parentheses in '%s'", line);

    remove(path);
}

/* When no candidate holds a ledger the tool has to say what it looked for:
 * otherwise a first run cannot be told apart from a wrong DSH_HOME. The search
 * only runs when no hint was given, so every hint is cleared here and both home
 * variables point at an empty scratch directory. */
static void t_no_ledger_found_explains_the_search(void)
{
    EnvSave dsh_home;
    EnvSave ledger_var;
    EnvSave profile_dir;
    EnvSave home;
    EnvSave userprofile;
    Run r;

    env_save(&dsh_home, "DSH_HOME");
    env_save(&ledger_var, "DSHTOK_LEDGER");
    env_save(&profile_dir, "DSH_PROFILE_DIR");
    env_save(&home, "HOME");
    env_save(&userprofile, "USERPROFILE");

    make_dir("build/tmp/cli-empty-home");
    env_set("DSH_HOME", NULL);
    env_set("DSHTOK_LEDGER", NULL);
    env_set("DSH_PROFILE_DIR", NULL);
    env_set("HOME", "build/tmp/cli-empty-home");
    env_set("USERPROFILE", "build/tmp/cli-empty-home");

    CHECK(run_capture("total", &r) == 0);
    CHECK_EQ_INT(r.exit_code, 1);

    env_restore(&userprofile);
    env_restore(&home);
    env_restore(&profile_dir);
    env_restore(&ledger_var);
    env_restore(&dsh_home);

    CHECK(strstr(r.err, "no ledger found") != NULL);
    CHECK_MSG(strstr(r.err, "looked for") != NULL,
              "stderr did not list the candidates: %s", r.err);
}

TEST_MAIN_BEGIN()
    /* Everything above needs the compiled binary; a checkout that has not been
     * built must not fail the suite. */
    {
        FILE *probe = fopen(BINARY, "rb");
        if (probe == NULL) {
            printf("skipped: binary not built\n");
            return 0;
        }
        fclose(probe);
    }

    if (prepare_scratch() != 0) {
        printf("skipped: cannot prepare build/tmp fixture\n");
        return 0;
    }

    {
        EnvSave home;
        EnvSave ledger;

        env_isolate(&home, &ledger);

        RUN(t_version);
        RUN(t_help);
        RUN(t_unknown_option_is_usage_error);
        RUN(t_unknown_command_is_usage_error);
        RUN(t_missing_ledger_is_runtime_error);
        RUN(t_no_ledger_found_explains_the_search);
        RUN(t_malformed_ledger_is_runtime_error);
        RUN(t_json_total);
        RUN(t_json_total_parses);
        RUN(t_json_models);
        RUN(t_json_models_parses);
        RUN(t_json_sessions_parses);
        RUN(t_json_daily_parses);
        RUN(t_quiet_prints_only_a_number);
        RUN(t_quiet_matches_json_total);
        RUN(t_csv_output);
        RUN(t_since_until_filtering);
        RUN(t_single_day_window);
        RUN(t_invalid_date_is_usage_error);
        RUN(t_impossible_dates_are_usage_errors);
        RUN(t_real_dates_are_accepted);
        RUN(t_bad_top_value_is_usage_error);
        RUN(t_all_commands_run);
        RUN(t_doctor_without_ledger_still_succeeds);
        RUN(t_doctor_reports_ledger_size);
        RUN(t_doctor_size_is_unknown_when_absent);
        RUN(t_doctor_size_under_one_kb_prints_once);
        RUN(t_quiet_and_json_together);
        RUN(t_no_header_and_json_agree);

        /* Leave the caller's environment exactly as it was found. */
        env_restore(&ledger);
        env_restore(&home);
    }
TEST_MAIN_END()
