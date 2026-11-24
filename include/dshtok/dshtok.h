#ifndef DSHTOK_DSHTOK_H
#define DSHTOK_DSHTOK_H

#include <stdio.h>

/* Umbrella header: everything a translation unit is allowed to use. */

#include "dshtok/version.h"
#include "dshtok/util.h"
#include "dshtok/json.h"
#include "dshtok/ledger.h"
#include "dshtok/format.h"
#include "dshtok/path.h"

/* Options shared by every command. Parsed once in main.c. */
typedef struct {
    const char *command;    /* "total", "daily", "models", "sessions", "doctor" */
    const char *path;       /* --path, or NULL for auto-detection */
    const char *profile;    /* --profile, or NULL */
    const char *since;      /* --since, or NULL */
    const char *until;      /* --until, or NULL */
    long top;               /* --top, <=0 means unlimited */
    int json;               /* --json */
    int csv;                /* --csv */
    int unicode;            /* --unicode */
    int color;              /* 0 = never, 1 = auto, 2 = always */
    int quiet;              /* --quiet */
    int no_header;          /* --no-header */
    int verbose;            /* --verbose */
} Options;

/* Render the requested report. Returns the process exit code: 0 success,
 * 1 runtime error, 2 usage error. */
int report_run(const Options *opt, const char *ledger_path);

/* Print the usage text to stream. When full is 0 only the one line synopsis is
 * printed. */
void report_usage(FILE *stream, int full);

#endif /* DSHTOK_DSHTOK_H */
