#ifndef DSHTOK_PATH_H
#define DSHTOK_PATH_H

#include <stddef.h>

/* Where a ledger may live, in the order dshtok probes them. */
typedef struct {
    char *candidates[8];  /* heap allocated, NULL terminated list */
    size_t count;
} PathCandidates;

/* Build the list of places to look for ledger.json.
 *
 * Order:
 *   1. $DSHTOK_LEDGER              (explicit override, always wins)
 *   2. $DSH_HOME/storages/cost-meter/ledger.json
 *   3. %USERPROFILE%\.dsh/...      (Windows)
 *      $HOME/.dsh/...              (POSIX)
 *   4. <cwd>/.dsh/...              (a checkout with its own harness home)
 *   5. /home/<user>/.dsh/...       (a WSL or container view of a Windows home,
 *                                   built from $USER only when $HOME is unset)
 *
 * When profile is not NULL, "<home>/profiles/<profile>/storages/cost-meter/
 * ledger.json" is probed before the flat form, because a profile can carry its
 * own storage root.
 *
 * Free the result with path_candidates_free(). */
void path_candidates_build(PathCandidates *out, const char *profile);
void path_candidates_free(PathCandidates *out);

/* Return the first candidate that exists and is readable, or NULL. */
const char *path_first_existing(const PathCandidates *list);

/* The DSH home directory dshtok believes in: $DSH_HOME, else the home directory
 * joined with ".dsh". Heap allocated. Returns NULL when no home can be
 * discovered. */
char *path_dsh_home(void);

/* Home directory discovery: %USERPROFILE% then %HOMEDRIVE%%HOMEPATH% then
 * $HOME then getpwuid(). Heap allocated, NULL when none worked. */
char *path_home_dir(void);

#endif /* DSHTOK_PATH_H */
