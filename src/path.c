/* path.c — find the ledger.
 *
 * DeepSeek Harness keeps its data under a single home directory, by default
 * ~/.dsh. The cost ledger written by the cost-meter plugin lives at
 * <home>/storages/cost-meter/ledger.json. This file knows how to look that up
 * the way the host itself would, and how to say clearly that it could not.
 */

#include "dshtok/path.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dshtok/util.h"

/* The ledger's path relative to the DSH home directory. */
#define LEDGER_RELATIVE "storages/cost-meter/ledger.json"

/* Two different sources of "the DSH home" usually agree, because DSH_HOME is
 * normally <user home>/.dsh and step 3 probes exactly that. Without this check
 * such a machine would list the same file twice and, worse, present the second
 * copy as if it were an independent fallback. Windows paths also differ only by
 * case often enough that comparing verbatim would miss it. */
static int path_same_file_name(const char *a, const char *b)
{
#ifdef _WIN32
    while (*a != '\0' && *b != '\0') {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (unsigned char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (unsigned char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
#else
    return strcmp(a, b) == 0;
#endif
}

static void push_candidate(PathCandidates *out, char *path)
{
    size_t i;
    if (out == NULL || path == NULL) {
        free(path);
        return;
    }
    if (out->count >= sizeof out->candidates / sizeof out->candidates[0]) {
        free(path);
        return;
    }
    for (i = 0; i < out->count; ++i) {
        if (path_same_file_name(out->candidates[i], path)) {
            free(path);
            return;
        }
    }
    out->candidates[out->count++] = path;
}

static void push_joined(PathCandidates *out, const char *base, const char *leaf)
{
    char *joined;
    if (base == NULL || base[0] == '\0') {
        return;
    }
    joined = path_join(base, leaf);
    if (joined != NULL) {
        /* Present a single separator style in messages, whatever the platform. */
        path_normalize_slashes(joined);
        push_candidate(out, joined);
    }
}

char *path_home_dir(void)
{
    /* Windows and POSIX disagree about the variable and the platform macros say
     * which one we are on, but checking both in this order is harmless and
     * covers the odd environment that sets HOME on Windows (MSYS2 does). */
#if defined(_WIN32)
    const char *names[] = {"USERPROFILE", "HOME"};
#else
    const char *names[] = {"HOME", "USERPROFILE"};
#endif
    size_t i;

    for (i = 0; i < sizeof names / sizeof names[0]; ++i) {
        const char *v = getenv(names[i]);
        if (v != NULL && v[0] != '\0') {
            return xstrdup(v);
        }
    }
    return NULL;
}

/* Last-resort user directory for POSIX systems where HOME is not exported but
 * the login name is. Containers and cron jobs routinely run with a stripped
 * environment, and WSL shells started from a Windows shortcut can lose HOME
 * while keeping USER; in both cases /home/<user> is still where .dsh lives.
 * Only consulted when HOME and USERPROFILE both failed, so it can never
 * override a home directory the caller actually told us about. */
static char *path_home_from_user(void)
{
#ifdef _WIN32
    /* Windows has no /home; USERPROFILE is the only home there is. */
    return NULL;
#else
    const char *user = getenv("USER");
    char *joined;
    if (user == NULL || user[0] == '\0') {
        return NULL;
    }
    /* A USER carrying a separator is not a login name, and pasting it into a
     * path would let the environment escape /home. Refuse rather than guess. */
    if (strchr(user, '/') != NULL || strcmp(user, ".") == 0 || strcmp(user, "..") == 0) {
        return NULL;
    }
    joined = path_join("/home", user);
    if (joined == NULL) {
        return NULL;
    }
    return joined;
#endif
}

char *path_dsh_home(void)
{
    const char *env = getenv("DSH_HOME");
    char *home;

    if (env != NULL && env[0] != '\0') {
        return xstrdup(env);
    }
    /* The host also honours DSH_PROFILE_DIR, from which the home is the parent
     * of profiles/<name>; deriving it keeps a profiled install working even
     * when DSH_HOME itself was not exported into this shell. */
    env = getenv("DSH_PROFILE_DIR");
    if (env != NULL && env[0] != '\0') {
        size_t len = strlen(env);
        char *copy = xstrdup(env);
        /* Strip a trailing separator, then walk back over the profile name and
         * the "profiles" component. Missing components simply mean we cannot
         * derive anything, and the caller falls back to the home directory. */
        while (len > 0 && (copy[len - 1] == '/' || copy[len - 1] == '\\')) {
            copy[--len] = '\0';
        }
        {
            char *slash = copy + len;
            int steps = 0;
            while (slash > copy && steps < 2) {
                --slash;
                if (*slash == '/' || *slash == '\\') {
                    *slash = '\0';
                    ++steps;
                }
            }
            if (slash > copy && steps == 2) {
                path_normalize_slashes(copy);
                return copy;
            }
        }
        free(copy);
    }

    home = path_home_dir();
    if (home == NULL) {
        home = path_home_from_user();
    }
    if (home == NULL) {
        return NULL;
    }
    {
        char *joined = path_join(home, ".dsh");
        free(home);
        return joined;
    }
}

void path_candidates_build(PathCandidates *out, const char *profile)
{
    const char *env;

    if (out == NULL) {
        return;
    }
    out->count = 0;

    /* 1. An explicit override always wins. DSHTOK_LEDGER exists so a user can
     *    point the tool at a copied ledger without retyping --path. */
    env = getenv("DSHTOK_LEDGER");
    if (env != NULL && env[0] != '\0') {
        char *copy = xstrdup(env);
        path_normalize_slashes(copy);
        push_candidate(out, copy);
    }

    /* 2. <DSH home>/storages/cost-meter/ledger.json, and the same under the
     *    requested profile when one was named. */
    {
        char *home = path_dsh_home();
        if (home != NULL) {
            path_normalize_slashes(home);
            if (profile != NULL && profile[0] != '\0') {
                char *profile_dir = path_join(home, "profiles");
                char *with_name = path_join(profile_dir, profile);
                push_joined(out, with_name, LEDGER_RELATIVE);
                free(with_name);
                free(profile_dir);
            }
            push_joined(out, home, LEDGER_RELATIVE);
            free(home);
        }
    }

    /* 3. The conventional location under the user's home, for the case where
     *    no DSH variable is exported at all (cron, a fresh shell, CI). */
    {
        char *home = path_home_dir();
        if (home == NULL) {
            home = path_home_from_user();
        }
        if (home != NULL) {
            char *dot = path_join(home, ".dsh");
            if (profile != NULL && profile[0] != '\0') {
                char *profile_dir = path_join(dot, "profiles");
                char *with_name = path_join(profile_dir, profile);
                push_joined(out, with_name, LEDGER_RELATIVE);
                free(with_name);
                free(profile_dir);
            }
            push_joined(out, dot, LEDGER_RELATIVE);
            free(dot);
            free(home);
        }
    }

    /* 4. A portable install unpacked next to the working directory. */
    push_joined(out, ".dsh", LEDGER_RELATIVE);
}

void path_candidates_free(PathCandidates *pc)
{
    size_t i;
    if (pc == NULL) {
        return;
    }
    for (i = 0; i < pc->count; ++i) {
        free(pc->candidates[i]);
        pc->candidates[i] = NULL;
    }
    pc->count = 0;
}

const char *path_first_existing(const PathCandidates *pc)
{
    size_t i;
    if (pc == NULL) {
        return NULL;
    }
    for (i = 0; i < pc->count; ++i) {
        if (path_is_readable_file(pc->candidates[i])) {
            return pc->candidates[i]; /* owned by the list, valid until freed */
        }
    }
    return NULL;
}
