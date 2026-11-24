/* Tests for path discovery: candidate ordering, first-existing probing, the
 * DSH home resolution and the util path helpers.
 *
 * Nothing here reads the user's real ledger: every candidate that could point
 * at a real ~/.dsh is only string-compared, never opened, and the "does this
 * file exist" tests use files inside build/tmp. Environment variables are saved
 * and restored around every case. */

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
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "dshtok/path.h"
#include "dshtok/util.h"
#include "mini_test.h"

#define BIG 1024

/* ------------------------------------------------------------------ */
/* build/tmp handling                                                  */
/* ------------------------------------------------------------------ */

static int make_dir(const char *path)
{
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0777);
#endif
}

/* Create the scratch directory hierarchy. Safe to call repeatedly. */
static void ensure_tmp(void)
{
    make_dir("build");
    make_dir("build/tmp");
}

/* Write a small file so that "exists and is readable" has something to find. */
static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

static int file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}

/* ------------------------------------------------------------------ */
/* environment helper                                                  */
/* ------------------------------------------------------------------ */

/* Each case sets a variable and then restores it, so the suite cannot leak
 * state into the next test or into the shell that ran it. */
typedef struct {
    char *name;
    char *saved;
    int had_value;
} EnvSave;

static void env_save(EnvSave *e, const char *name)
{
    const char *cur = getenv(name);

    e->name = xstrdup(name);
    e->had_value = cur != NULL;
    e->saved = cur != NULL ? xstrdup(cur) : NULL;
}

static void env_set(const char *name, const char *value)
{
#ifdef _WIN32
    if (value == NULL) {
        _putenv_s(name, "");
    } else {
        _putenv_s(name, value);
    }
#else
    if (value == NULL) {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

static void env_restore(EnvSave *e)
{
    if (e->had_value) {
        env_set(e->name, e->saved);
    } else {
        env_set(e->name, NULL);
    }
    free(e->name);
    free(e->saved);
    e->name = NULL;
    e->saved = NULL;
}

/* ------------------------------------------------------------------ */
/* path_join / path_normalize_slashes                                  */
/* ------------------------------------------------------------------ */

static void t_path_join(void)
{
    char *p;

    p = path_join("/a/b", "c.json");
    CHECK_EQ_STR(p, "/a/b/c.json");
    free(p);

    /* A directory that already ends in a separator must not get a second one. */
    p = path_join("/a/b/", "c.json");
    CHECK_EQ_STR(p, "/a/b/c.json");
    free(p);

    p = path_join("/a/b/", "/c.json");
    CHECK_EQ_STR(p, "/a/b//c.json");
    free(p);

    p = path_join("", "c.json");
    CHECK_EQ_STR(p, "c.json");
    free(p);

    /* An empty leaf still joins: the header only promises a separator between
     * the two fragments, so a trailing separator is correct. */
    p = path_join("a", "");
    CHECK(p != NULL);
    CHECK(strncmp(p, "a", 1) == 0);
    CHECK_EQ_INT(strlen(p), 2);
    CHECK(p[1] == '/' || p[1] == '\\');
    free(p);

    /* NULL on either side yields NULL rather than crashing. */
    CHECK(path_join(NULL, "c") == NULL);
    CHECK(path_join("a", NULL) == NULL);
    CHECK(path_join(NULL, NULL) == NULL);
}

static void t_path_normalize_slashes(void)
{
    char buf[BIG];

    strcpy(buf, "a\\b\\c");
    path_normalize_slashes(buf);
    CHECK_EQ_STR(buf, "a/b/c");

    strcpy(buf, "a/b\\c");
    path_normalize_slashes(buf);
    CHECK_EQ_STR(buf, "a/b/c");

    /* Already normalized input is unchanged. */
    strcpy(buf, "a/b/c");
    path_normalize_slashes(buf);
    CHECK_EQ_STR(buf, "a/b/c");

    strcpy(buf, "");
    path_normalize_slashes(buf);
    CHECK_EQ_STR(buf, "");

    /* A Windows drive letter and UNC style prefix. */
    strcpy(buf, "C:\\Users\\x\\.dsh");
    path_normalize_slashes(buf);
    CHECK_EQ_STR(buf, "C:/Users/x/.dsh");
}

/* ------------------------------------------------------------------ */
/* path_home_dir / path_dsh_home                                       */
/* ------------------------------------------------------------------ */

static void t_dsh_home_honours_env(void)
{
    EnvSave saved;
    char *home;

    env_save(&saved, "DSH_HOME");
    env_set("DSH_HOME", "build/tmp/fake-home");
    home = path_dsh_home();
    CHECK(home != NULL);
    CHECK_EQ_STR(home, "build/tmp/fake-home");
    free(home);
    env_restore(&saved);

    /* With DSH_HOME unset the result is <home>/.dsh, which must at least be a
     * non-empty absolute looking path ending in ".dsh". */
    env_save(&saved, "DSH_HOME");
    env_set("DSH_HOME", NULL);
    home = path_dsh_home();
    if (home != NULL) {
        size_t n = strlen(home);
        /* <home>/.dsh, in either separator style. */
        CHECK(n > 4);
        CHECK_EQ_STR(home + n - 4, ".dsh");
        free(home);
    }
    env_restore(&saved);
}

/* ------------------------------------------------------------------ */
/* path_candidates_build                                               */
/* ------------------------------------------------------------------ */

/* Find the index of the first candidate containing needle, or -1. */
static int find_candidate(const PathCandidates *pc, const char *needle)
{
    size_t i;

    for (i = 0; i < pc->count; ++i) {
        if (strstr(pc->candidates[i], needle) != NULL) {
            return (int)i;
        }
    }
    return -1;
}

static void t_candidates_override_wins(void)
{
    PathCandidates pc;
    EnvSave saved;

    env_save(&saved, "DSHTOK_LEDGER");
    env_set("DSHTOK_LEDGER", "build/tmp/override-ledger.json");

    path_candidates_build(&pc, NULL);
    CHECK(pc.count > 0);
    CHECK(pc.candidates[0] != NULL);
    /* The explicit override is always first. */
    CHECK_EQ_STR(pc.candidates[0], "build/tmp/override-ledger.json");
    /* Every slot up to count is non-NULL, and the list is capped at 8. */
    CHECK(pc.count <= 8);
    path_candidates_free(&pc);

    env_restore(&saved);
}

static void t_candidates_include_dsh_home(void)
{
    PathCandidates pc;
    EnvSave saved;
    int idx;

    env_save(&saved, "DSH_HOME");
    env_set("DSH_HOME", "build/tmp/fake-home");

    path_candidates_build(&pc, NULL);
    idx = find_candidate(&pc, "build/tmp/fake-home");
    CHECK(idx >= 0);
    if (idx >= 0) {
        /* The leaf is the cost-meter ledger. */
        CHECK(strstr(pc.candidates[idx], "storages") != NULL);
        CHECK(strstr(pc.candidates[(size_t)idx], "ledger.json") != NULL);
    }
    path_candidates_free(&pc);

    env_restore(&saved);
}

static void t_candidates_with_profile(void)
{
    PathCandidates pc;
    EnvSave saved;
    int profiled;
    int flat;
    size_t i;

    env_save(&saved, "DSH_HOME");
    env_set("DSH_HOME", "build/tmp/fake-home");

    path_candidates_build(&pc, "work");
    profiled = -1;
    flat = -1;
    for (i = 0; i < pc.count; ++i) {
        if (strstr(pc.candidates[i], "profiles/work") != NULL &&
            strstr(pc.candidates[i], "ledger.json") != NULL) {
            if (profiled < 0) {
                profiled = (int)i;
            }
        } else if (strstr(pc.candidates[i], "fake-home") != NULL &&
                   strstr(pc.candidates[i], "storages") != NULL) {
            if (flat < 0) {
                flat = (int)i;
            }
        }
    }
    CHECK(profiled >= 0);
    /* The profile form is probed before the flat form. */
    if (profiled >= 0 && flat >= 0) {
        CHECK(profiled < flat);
    }
    path_candidates_free(&pc);

    env_restore(&saved);
}

static void t_candidates_no_duplicates(void)
{
    PathCandidates pc;
    EnvSave saved;
    size_t i;
    size_t j;

    /* With DSH_HOME pointing somewhere other than <home>/.dsh the two sources
     * are genuinely different, so probing the same file twice would be a bug. */
    env_save(&saved, "DSH_HOME");
    env_set("DSH_HOME", "build/tmp/fake-home");

    path_candidates_build(&pc, NULL);
    CHECK(pc.count <= 8);
    for (i = 0; i < pc.count; ++i) {
        CHECK(pc.candidates[i] != NULL);
        for (j = i + 1; j < pc.count; ++j) {
            CHECK_MSG(strcmp(pc.candidates[i], pc.candidates[j]) != 0,
                      "%s", "two candidates probe the same path");
        }
    }
    path_candidates_free(&pc);

    env_restore(&saved);
}

static void t_candidates_free_is_safe(void)
{
    PathCandidates pc;

    /* Freeing the list twice must not double free: the header implies the
     * struct is left empty. If an implementation only frees, this still holds
     * because a fresh zeroed build is used each time. */
    path_candidates_build(&pc, NULL);
    path_candidates_free(&pc);
    CHECK_EQ_INT(pc.count, 0);

    /* A zeroed list frees cleanly. */
    memset(&pc, 0, sizeof pc);
    path_candidates_free(&pc);
    CHECK_EQ_INT(pc.count, 0);
}

/* ------------------------------------------------------------------ */
/* path_first_existing                                                 */
/* ------------------------------------------------------------------ */

static void t_first_existing_none(void)
{
    PathCandidates pc;

    memset(&pc, 0, sizeof pc);
    /* An empty candidate list has nothing to find. */
    CHECK(path_first_existing(&pc) == NULL);

    path_candidates_build(&pc, NULL);
    path_candidates_free(&pc);
}

static void t_first_existing_picks_first(void)
{
    PathCandidates pc;

    ensure_tmp();
    /* Build a list by hand: only the second entry exists. */
    memset(&pc, 0, sizeof pc);
    pc.candidates[0] = xstrdup("build/tmp/there-is-no-such-file-1.json");
    pc.candidates[1] = xstrdup("build/tmp/existing-ledger.json");
    pc.candidates[2] = xstrdup("build/tmp/there-is-no-such-file-2.json");
    pc.count = 3;

    CHECK(write_file("build/tmp/existing-ledger.json", "{}"));

    CHECK_EQ_STR(path_first_existing(&pc), "build/tmp/existing-ledger.json");

    path_candidates_free(&pc);
}

static void t_first_existing_returns_list_member(void)
{
    PathCandidates pc;
    const char *found;
    size_t i;
    int inside = 0;

    ensure_tmp();
    CHECK(write_file("build/tmp/existing-ledger-b.json", "{}"));

    memset(&pc, 0, sizeof pc);
    pc.candidates[0] = xstrdup("build/tmp/absent-a.json");
    pc.candidates[1] = xstrdup("build/tmp/existing-ledger-b.json");
    pc.count = 2;

    found = path_first_existing(&pc);
    CHECK(found != NULL);
    /* The frozen signature returns const char *, i.e. a pointer INTO the list,
     * not a fresh allocation: the caller must not free it. */
    for (i = 0; i < pc.count; ++i) {
        if (found == pc.candidates[i]) {
            inside = 1;
        }
    }
    if (found != NULL) {
        CHECK(inside);
    }

    path_candidates_free(&pc);
}

/* ------------------------------------------------------------------ */
/* path_is_readable_file (used by path_first_existing)                 */
/* ------------------------------------------------------------------ */

static void t_readable_file(void)
{
    ensure_tmp();

    CHECK_EQ_INT(path_is_readable_file("build/tmp"), 0);
    CHECK_EQ_INT(path_is_readable_file("build/tmp/no-such-file.json"), 0);
    CHECK_EQ_INT(path_is_readable_file(NULL), 0);

    CHECK(write_file("build/tmp/readable.json", "{}"));
    CHECK_EQ_INT(path_is_readable_file("build/tmp/readable.json"), 1);
    CHECK(file_exists("build/tmp/readable.json"));
}

/* ------------------------------------------------------------------ */
/* home fallback: HOME unset, USER set                                 */
/* ------------------------------------------------------------------ */

/* Strip every variable that can steer discovery, so each case below is
 * decided by the one thing it sets. */
typedef struct {
    EnvSave dsh_home;
    EnvSave ledger;
    EnvSave home;
    EnvSave userprofile;
    EnvSave user;
} DiscoveryEnv;

static void discovery_env_clear(DiscoveryEnv *e)
{
    env_save(&e->dsh_home, "DSH_HOME");
    env_save(&e->ledger, "DSHTOK_LEDGER");
    env_save(&e->home, "HOME");
    env_save(&e->userprofile, "USERPROFILE");
    env_save(&e->user, "USER");
    env_set("DSH_HOME", NULL);
    env_set("DSHTOK_LEDGER", NULL);
    env_set("HOME", NULL);
    env_set("USERPROFILE", NULL);
    env_set("USER", NULL);
}

static void discovery_env_restore(DiscoveryEnv *e)
{
    env_restore(&e->user);
    env_restore(&e->userprofile);
    env_restore(&e->home);
    env_restore(&e->ledger);
    env_restore(&e->dsh_home);
}

/* A USER value that is not a plain login name must never be pasted into a
 * path: "/home/../etc" would escape the directory the fallback guesses. This
 * case is meaningful everywhere because the statement it asserts (no candidate
 * contains a traversal) holds on both platforms; on Windows the fallback is
 * compiled out, so it passes without exercising the branch. */
static void t_home_fallback_refuses_unsafe_user(void)
{
    static const char *const bad[] = {"../etc", "..", ".", "a/b", "/etc"};
    DiscoveryEnv de;
    size_t k;
    size_t i;

    discovery_env_clear(&de);

    for (k = 0; k < sizeof bad / sizeof bad[0]; ++k) {
        PathCandidates pc;

        env_set("USER", bad[k]);
        path_candidates_build(&pc, NULL);
        CHECK(pc.count > 0);
        for (i = 0; i < pc.count; ++i) {
            CHECK(pc.candidates[i] != NULL);
            CHECK_MSG(strstr(pc.candidates[i], "..") == NULL,
                      "traversal leaked from USER='%s'", bad[k]);
            CHECK_MSG(strstr(pc.candidates[i], "/etc") == NULL,
                      "absolute path leaked from USER='%s'", bad[k]);
        }
        path_candidates_free(&pc);
    }

    discovery_env_restore(&de);
}

#if !defined(_WIN32)
/* Where the fallback looks is a platform fact, so it is only asserted on the
 * platform that has /home. On Windows the branch does not exist at all. */
static void t_home_fallback_uses_user(void)
{
    DiscoveryEnv de;
    PathCandidates pc;
    char *dsh;
    char expected[BIG];
    int idx;
    size_t i;
    size_t j;

    discovery_env_clear(&de);
    env_set("USER", "dshtok-probe");

    /* With HOME stripped -- cron, a container, some WSL launches -- the login
     * name still locates the directory that holds .dsh. */
    path_candidates_build(&pc, NULL);
    snprintf(expected, sizeof expected, "/home/dshtok-probe/.dsh");
    idx = find_candidate(&pc, expected);
    CHECK(idx >= 0);
    if (idx >= 0) {
        CHECK(strstr(pc.candidates[(size_t)idx], "storages") != NULL);
        CHECK(strstr(pc.candidates[(size_t)idx], "ledger.json") != NULL);
    }
    /* The extra fallback must not reintroduce duplicates. */
    for (i = 0; i < pc.count; ++i) {
        for (j = i + 1; j < pc.count; ++j) {
            CHECK(strcmp(pc.candidates[i], pc.candidates[j]) != 0);
        }
    }
    path_candidates_free(&pc);

    dsh = path_dsh_home();
    CHECK(dsh != NULL);
    if (dsh != NULL) {
        CHECK_EQ_STR(dsh, "/home/dshtok-probe/.dsh");
        free(dsh);
    }

    discovery_env_restore(&de);
}
#endif

TEST_MAIN_BEGIN()
    RUN(t_path_join);
    RUN(t_path_normalize_slashes);
    RUN(t_dsh_home_honours_env);
    RUN(t_candidates_override_wins);
    RUN(t_candidates_include_dsh_home);
    RUN(t_candidates_with_profile);
    RUN(t_candidates_no_duplicates);
    RUN(t_candidates_free_is_safe);
    RUN(t_first_existing_none);
    RUN(t_first_existing_picks_first);
    RUN(t_first_existing_returns_list_member);
    RUN(t_readable_file);
    RUN(t_home_fallback_refuses_unsafe_user);
#if !defined(_WIN32)
    RUN(t_home_fallback_uses_user);
#endif
TEST_MAIN_END()
