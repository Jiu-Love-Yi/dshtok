/* mini_test.h — a header-only assertion harness.
 *
 * Every test binary includes this file, writes test cases with TEST(name), and
 * ends with TEST_MAIN(). The runner prints one line per failure and a one line
 * summary, and exits non-zero when anything failed, so `make test` works.
 *
 * Deliberately dependency free and C99: no global constructors, no fork, no
 * threads. Tests may use CHECK_* macros on the way through, no teardown needed.
 */
#ifndef DSHTOK_MINI_TEST_H
#define DSHTOK_MINI_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Every test binary includes this header whole, so a helper that only some tests
 * use would otherwise trip -Wunused-function. Mark them instead of asking each
 * test file to remember which macros it must reference. */
#if defined(__GNUC__) || defined(__clang__)
#define DSHTOK_TEST_UNUSED __attribute__((unused))
#else
#define DSHTOK_TEST_UNUSED
#endif

static int g_test_failures = 0;
static int g_test_checks = 0;
static const char *g_test_current = "";

#define CHECK(cond)                                                        \
    do {                                                                   \
        ++g_test_checks;                                                   \
        if (!(cond)) {                                                     \
            ++g_test_failures;                                             \
            fprintf(stderr, "  FAIL %s:%d [%s] %s\n",                      \
                    __FILE__, __LINE__, g_test_current, #cond);            \
        }                                                                  \
    } while (0)

#define CHECK_MSG(cond, fmt, ...)                                          \
    do {                                                                   \
        ++g_test_checks;                                                   \
        if (!(cond)) {                                                     \
            ++g_test_failures;                                             \
            fprintf(stderr, "  FAIL %s:%d [%s] %s — ",                     \
                    __FILE__, __LINE__, g_test_current, #cond);            \
            fprintf(stderr, fmt, __VA_ARGS__);                             \
            fputc('\n', stderr);                                           \
        }                                                                  \
    } while (0)

#define CHECK_EQ_INT(actual, expected)                                     \
    do {                                                                   \
        long long a_ = (long long)(actual);                                \
        long long e_ = (long long)(expected);                              \
        ++g_test_checks;                                                   \
        if (a_ != e_) {                                                    \
            ++g_test_failures;                                             \
            fprintf(stderr, "  FAIL %s:%d [%s] %s == %lld, want %lld\n",   \
                    __FILE__, __LINE__, g_test_current, #actual, a_, e_);  \
        }                                                                  \
    } while (0)

#define CHECK_EQ_STR(actual, expected)                                     \
    do {                                                                   \
        const char *a_ = (actual);                                         \
        const char *e_ = (expected);                                       \
        ++g_test_checks;                                                   \
        if (a_ == NULL || e_ == NULL || strcmp(a_, e_) != 0) {             \
            ++g_test_failures;                                             \
            fprintf(stderr, "  FAIL %s:%d [%s] %s == \"%s\", want \"%s\"\n", \
                    __FILE__, __LINE__, g_test_current, #actual,           \
                    a_ ? a_ : "(null)", e_ ? e_ : "(null)");               \
        }                                                                  \
    } while (0)

static DSHTOK_TEST_UNUSED int g_test_approx_ok_(double actual, double expected,
                                                double eps, const char *file,
                                                int line, const char *expr)
{
    double d = actual - expected;
    ++g_test_checks;
    if (!(d <= eps && d >= -eps)) {
        ++g_test_failures;
        fprintf(stderr, "  FAIL %s:%d [%s] %s ~= %g, got %g\n",
                file, line, g_test_current, expr, expected, actual);
        return 0;
    }
    return 1;
}

#define CHECK_NEAR(actual, expected, eps)                                  \
    g_test_approx_ok_((double)(actual), (double)(expected), (double)(eps),  \
                      __FILE__, __LINE__, #actual)

#define TEST(name) static void name(void)
#define RUN(name)                                                          \
    do {                                                                   \
        g_test_current = #name;                                            \
        name();                                                            \
    } while (0)

#define TEST_MAIN_BEGIN()                                                  \
    int main(void)                                                         \
    {

#define TEST_MAIN_END()                                                    \
        if (g_test_failures == 0) {                                        \
            printf("ok — %d checks passed\n", g_test_checks);              \
            return 0;                                                      \
        }                                                                  \
        printf("FAILED — %d of %d checks failed\n",                        \
               g_test_failures, g_test_checks);                            \
        return 1;                                                          \
    }

#endif /* DSHTOK_MINI_TEST_H */
