/* main.c — argument parsing and dispatch.
 *
 * The command line is deliberately small enough to parse by hand: no getopt, no
 * platform differences, and every error message names the offending argument.
 * Options may appear before or after the command, which is what people type.
 */

#include "dshtok/dshtok.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const k_commands[] = {"total", "daily", "models", "sessions",
                                         "doctor", "help"};
static const size_t k_command_count = sizeof k_commands / sizeof k_commands[0];

static int is_known_command(const char *s)
{
    size_t i;
    for (i = 0; i < k_command_count; ++i) {
        if (strcmp(s, k_commands[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

/* `ledger_valid_date` checks the shape only: four digits, dash, two, dash, two.
 * That is the right test for a key read out of the ledger, because a real ledger
 * is the authority on which days exist and we should not be pickier than it is.
 * It is the wrong test for a value the user typed, because "2026-13-99" is not a
 * day that can exist, and filtering on it yields an empty report and a success
 * exit -- indistinguishable from "you have spent nothing", which is the most
 * misleading answer this tool could give. So the command line asks a stricter
 * question: does this name a day on the calendar? */
static int cli_date_is_real(const char *s)
{
    static const int k_month_days[13] = {0,  31, 28, 31, 30, 31, 30,
                                         31, 31, 30, 31, 30, 31};
    int year, month, day, leap;

    if (!ledger_valid_date(s)) {
        return 0;
    }
    year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 +
           (s[3] - '0');
    month = (s[5] - '0') * 10 + (s[6] - '0');
    day = (s[8] - '0') * 10 + (s[9] - '0');

    if (month < 1 || month > 12 || day < 1) {
        return 0;
    }
    leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) {
        return day <= 29;
    }
    return day <= k_month_days[month];
}

/* One message shape for both options, so a reader who mistypes either one gets
 * told the same thing. */
static void cli_complain_about_date(const char *option, const char *value)
{
    fprintf(stderr, "dshtok: %s wants a real YYYY-MM-DD date, got '%s'\n",
            option, value);
}

/* "--name=value" and "-pvalue" both need the value split off. Returns the value
 * when the argument carries one inline, otherwise NULL and the caller consumes
 * the next argument. */
static const char *inline_value(const char *arg, const char *long_name,
                                const char *short_name)
{
    size_t long_len = strlen(long_name);
    if (short_name != NULL && strncmp(arg, short_name, strlen(short_name)) == 0) {
        size_t short_len = strlen(short_name);
        if (arg[short_len] != '\0' && arg[short_len] != '=') {
            return arg + short_len;
        }
        if (arg[short_len] == '=') {
            return arg + short_len + 1;
        }
        return NULL;
    }
    if (strncmp(arg, long_name, long_len) == 0 && arg[long_len] == '=') {
        return arg + long_len + 1;
    }
    return NULL;
}

static int is_exactly(const char *arg, const char *long_name,
                      const char *short_name)
{
    if (strcmp(arg, long_name) == 0) {
        return 1;
    }
    if (short_name != NULL && strcmp(arg, short_name) == 0) {
        return 1;
    }
    return 0;
}

static int parse_long_number(const char *text, long *out)
{
    char *end = NULL;
    long value;
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    value = strtol(text, &end, 10);
    if (end == NULL || *end != '\0') {
        return 0;
    }
    if (value < 0) {
        return 0;
    }
    *out = value;
    return 1;
}

int main(int argc, char **argv)
{
    Options opt;
    const char *ledger_path = NULL;
    int i;
    int want_help = 0;
    int want_version = 0;
    int rc;

    memset(&opt, 0, sizeof opt);
    opt.command = "total";
    opt.color = 1; /* auto */

    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        const char *value;

        if (arg == NULL || arg[0] == '\0') {
            continue;
        }

        if (strcmp(arg, "--") == 0) {
            /* Everything after -- is a command, so a command name that looks
             * like an option can still be used. */
            if (i + 1 < argc) {
                if (!is_known_command(argv[i + 1])) {
                    fprintf(stderr, "dshtok: unknown command '%s'\n", argv[i + 1]);
                    return 2;
                }
                opt.command = argv[i + 1];
            }
            break;
        }

        if (arg[0] != '-') {
            if (is_known_command(arg)) {
                opt.command = arg;
                continue;
            }
            fprintf(stderr, "dshtok: unknown command '%s'\n", arg);
            fprintf(stderr, "run 'dshtok --help' for usage\n");
            return 2;
        }

        if (is_exactly(arg, "--help", "-h")) {
            want_help = 1;
            continue;
        }
        if (is_exactly(arg, "--version", "-V")) {
            want_version = 1;
            continue;
        }
        if (is_exactly(arg, "--json", NULL)) {
            opt.json = 1;
            continue;
        }
        if (strcmp(arg, "-j") == 0) {
            opt.json = 1;
            continue;
        }
        if (is_exactly(arg, "--csv", NULL)) {
            opt.csv = 1;
            continue;
        }
        if (is_exactly(arg, "--unicode", "-u")) {
            opt.unicode = 1;
            continue;
        }
        if (is_exactly(arg, "--no-color", NULL)) {
            opt.color = 0;
            continue;
        }
        if (is_exactly(arg, "--color", NULL)) {
            opt.color = 2;
            continue;
        }
        if (is_exactly(arg, "--no-header", NULL)) {
            opt.no_header = 1;
            continue;
        }
        if (is_exactly(arg, "--quiet", "-q")) {
            opt.quiet = 1;
            continue;
        }
        if (is_exactly(arg, "--verbose", "-v")) {
            opt.verbose = 1;
            continue;
        }

        value = inline_value(arg, "--path", "-p");
        if (value != NULL) {
            ledger_path = value;
            continue;
        }
        if (is_exactly(arg, "--path", "-p") || is_exactly(arg, "--path", NULL)) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dshtok: %s requires a file name\n", arg);
                return 2;
            }
            ledger_path = argv[++i];
            continue;
        }

        value = inline_value(arg, "--profile", NULL);
        if (value != NULL) {
            opt.profile = value;
            continue;
        }
        if (strcmp(arg, "--profile") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dshtok: --profile requires a name\n");
                return 2;
            }
            opt.profile = argv[++i];
            continue;
        }

        value = inline_value(arg, "--since", NULL);
        if (value != NULL) {
            opt.since = value;
            continue;
        }
        if (strcmp(arg, "--since") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dshtok: --since requires a date\n");
                return 2;
            }
            opt.since = argv[++i];
            continue;
        }

        value = inline_value(arg, "--until", NULL);
        if (value != NULL) {
            opt.until = value;
            continue;
        }
        if (strcmp(arg, "--until") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dshtok: --until requires a date\n");
                return 2;
            }
            opt.until = argv[++i];
            continue;
        }

        value = inline_value(arg, "--top", NULL);
        if (value != NULL) {
            if (!parse_long_number(value, &opt.top)) {
                fprintf(stderr, "dshtok: --top wants a non-negative number, got "
                                "'%s'\n",
                        value);
                return 2;
            }
            continue;
        }
        if (strcmp(arg, "--top") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "dshtok: --top requires a number\n");
                return 2;
            }
            if (!parse_long_number(argv[++i], &opt.top)) {
                fprintf(stderr, "dshtok: --top wants a non-negative number, got "
                                "'%s'\n",
                        argv[i]);
                return 2;
            }
            continue;
        }

        fprintf(stderr, "dshtok: unknown option '%s'\n", arg);
        fprintf(stderr, "run 'dshtok --help' for usage\n");
        return 2;
    }

    if (want_help || strcmp(opt.command, "help") == 0) {
        report_usage(stdout, 1);
        return 0;
    }
    if (want_version) {
        printf("%s %s\n", DSHTOK_NAME, DSHTOK_VERSION_STRING);
        return 0;
    }

    /* A mistyped date is a usage error, and it is worth catching before the
     * ledger is even opened. */
    if (opt.since != NULL && opt.since[0] != '\0' &&
        !cli_date_is_real(opt.since)) {
        cli_complain_about_date("--since", opt.since);
        return 2;
    }
    if (opt.until != NULL && opt.until[0] != '\0' &&
        !cli_date_is_real(opt.until)) {
        cli_complain_about_date("--until", opt.until);
        return 2;
    }
    if (opt.since != NULL && opt.until != NULL &&
        ledger_valid_date(opt.since) && ledger_valid_date(opt.until) &&
        strcmp(opt.since, opt.until) > 0) {
        fprintf(stderr, "dshtok: --since is after --until\n");
        return 2;
    }

    rc = report_run(&opt, ledger_path);
    return rc;
}
