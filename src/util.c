/* util.c — allocation, file reading and small path helpers.
 *
 * Everything here is deliberately boring. The tool links against the C standard
 * library and nothing else, so the pieces that a POSIX system would hand us for
 * free (strdup, realpath) are written out once, here, with a Windows and a POSIX
 * branch where the platform genuinely differs.
 */

#include "dshtok/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <direct.h>
#  define DSHTOK_SEP '/'
#  define dsh_stat_is_file(p) _dsh_is_file_win(p)
#else
#  include <sys/stat.h>
#  include <unistd.h>
#  define DSHTOK_SEP '/'
#  define dsh_stat_is_file(p) _dsh_is_file_posix(p)
#endif

static void oom(void)
{
    /* Nothing sensible can be done: the tool holds a few megabytes at most, so
     * reaching this means the machine is in real trouble. Report and leave. */
    fputs("dshtok: out of memory\n", stderr);
    exit(1);
}

void *xmalloc(size_t n)
{
    void *p;
    if (n == 0) {
        n = 1; /* malloc(0) may legitimately return NULL */
    }
    p = malloc(n);
    if (p == NULL) {
        oom();
    }
    return p;
}

void *xcalloc(size_t count, size_t size)
{
    void *p;
    if (count == 0 || size == 0) {
        count = 1;
        size = 1;
    }
    p = calloc(count, size);
    if (p == NULL) {
        oom();
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    void *q;
    if (n == 0) {
        n = 1;
    }
    q = realloc(p, n);
    if (q == NULL) {
        oom();
    }
    return q;
}

char *xstrdup(const char *s)
{
    size_t n;
    char *p;
    if (s == NULL) {
        s = "";
    }
    n = strlen(s) + 1;
    p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

char *xstrndup(const char *s, size_t n)
{
    char *p;
    if (s == NULL) {
        return xstrdup("");
    }
    p = (char *)xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *str_copy(char *dst, size_t dstsz, const char *src)
{
    size_t i;
    if (dst == NULL || dstsz == 0) {
        return dst;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return dst;
    }
    for (i = 0; i + 1 < dstsz && src[i] != '\0'; ++i) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
    return dst;
}

#if defined(_WIN32)
static int _dsh_is_file_win(const char *path)
{
    FILE *f;
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    fclose(f);
    return 1;
}
#else
static int _dsh_is_file_posix(const char *path)
{
    struct stat st;
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISREG(st.st_mode) ? 1 : 0;
}
#endif

int path_is_readable_file(const char *path)
{
    return dsh_stat_is_file(path);
}

char *read_entire_file(const char *path, size_t *out_len, char *err, size_t errsz)
{
    FILE *f;
    char *buf;
    size_t capacity;
    size_t length = 0;

    if (err != NULL && errsz > 0) {
        err[0] = '\0';
    }
    if (path == NULL || path[0] == '\0') {
        str_copy(err, errsz, "empty file name");
        return NULL;
    }

    f = fopen(path, "rb");
    if (f == NULL) {
        char msg[256];
        /* Keep the reason short: the caller prints the path itself. */
        snprintf(msg, sizeof msg, "cannot open file");
        str_copy(err, errsz, msg);
        return NULL;
    }

    /* Start at 64 KiB — a real ledger is megabytes, but a fixture is not, and
     * doubling from a small base costs at most a handful of reallocs. */
    capacity = 65536;
    buf = (char *)xmalloc(capacity);

    for (;;) {
        size_t got;
        if (length + 1 >= capacity) {
            /* +1 keeps room for the terminating NUL we always append. */
            size_t lower = capacity * 2;
            if (lower <= capacity) {
                fclose(f);
                free(buf);
                str_copy(err, errsz, "file is too large");
                return NULL;
            }
            capacity = lower;
            buf = (char *)xrealloc(buf, capacity);
        }
        got = fread(buf + length, 1, capacity - length - 1, f);
        length += got;
        if (got == 0) {
            if (ferror(f)) {
                fclose(f);
                free(buf);
                str_copy(err, errsz, "read error");
                return NULL;
            }
            break; /* EOF */
        }
    }
    fclose(f);

    buf[length] = '\0';
    if (out_len != NULL) {
        *out_len = length;
    }
    return buf;
}

char *path_join(const char *dir, const char *leaf)
{
    size_t dlen;
    size_t llen;
    char *out;
    size_t pos = 0;

    if (dir == NULL || leaf == NULL) {
        return NULL;
    }
    dlen = strlen(dir);
    llen = strlen(leaf);
    if (dlen > 32768 || llen > 32768) {
        return NULL;
    }

    out = (char *)xmalloc(dlen + llen + 2);
    memcpy(out, dir, dlen);
    pos = dlen;
    if (pos > 0 && out[pos - 1] != '/' && out[pos - 1] != '\\') {
        out[pos++] = DSHTOK_SEP;
    }
    memcpy(out + pos, leaf, llen);
    pos += llen;
    out[pos] = '\0';
    return out;
}

void path_normalize_slashes(char *path)
{
    size_t i;
    if (path == NULL) {
        return;
    }
    for (i = 0; path[i] != '\0'; ++i) {
        if (path[i] == '\\') {
            path[i] = '/';
        }
    }
}

int imin(int a, int b)
{
    return a < b ? a : b;
}

size_t smin(size_t a, size_t b)
{
    return a < b ? a : b;
}
