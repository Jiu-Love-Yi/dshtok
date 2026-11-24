#ifndef DSHTOK_UTIL_H
#define DSHTOK_UTIL_H

#include <stddef.h>

/* Allocation helpers. All of them terminate the process with a message on
 * failure instead of returning NULL; the tool handles a few megabytes at most
 * and an out-of-memory condition is not recoverable in a useful way. */
void *xmalloc(size_t n);
void *xcalloc(size_t count, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* Copy at most dstsz-1 bytes from src and always NUL terminate.
 * Returns dst. src may be NULL, which stores an empty string. */
char *str_copy(char *dst, size_t dstsz, const char *src);

/* Read a whole file into a NUL terminated heap buffer.
 * On success returns the buffer and stores its length (excluding the NUL) in
 * *out_len; *out_len may be NULL. On failure returns NULL and writes a short
 * human readable reason into err (never more than errsz bytes). */
char *read_entire_file(const char *path, size_t *out_len, char *err, size_t errsz);

/* True when path names an existing, readable regular file. */
int path_is_readable_file(const char *path);

/* Join two path fragments with the platform separator. The result is heap
 * allocated. Returns NULL when either argument is NULL or the buffer would
 * overflow. */
char *path_join(const char *dir, const char *leaf);

/* Replace every backslash with a forward slash in place. Used only for display
 * so that output is stable across platforms. */
void path_normalize_slashes(char *path);

/* Minimum / maximum helpers with predictable types. */
int imin(int a, int b);
size_t smin(size_t a, size_t b);

#endif /* DSHTOK_UTIL_H */
