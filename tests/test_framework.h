/* =============================================================================
 * test_framework.h -- A deliberately tiny test harness.
 * =============================================================================
 *
 * WHY NOT A REAL FRAMEWORK?
 * -------------------------
 * Because firmware unit tests usually run on the target, or in a build with no
 * dynamic allocation and no standard library beyond the basics, and dragging in
 * a framework that wants malloc and exceptions defeats the point. Roughly forty
 * lines of macros gives you named suites, per-check reporting with file and
 * line, and a non-zero exit status on failure -- which is everything CI
 * actually needs from a test runner.
 *
 * The style here is: CHECK keeps going after a failure (so one run tells you
 * about several problems), REQUIRE stops the current test (for when continuing
 * would just produce noise or crash).
 * ========================================================================== */

#ifndef TEST_FRAMEWORK_H
#define TEST_FRAMEWORK_H

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

extern int g_tests_run;
extern int g_tests_failed;
extern int g_checks_run;
extern int g_checks_failed;
extern int g_current_failed;

#define TEST_CASE(fn) static void fn(void)

/* Non-fatal assertion. */
#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks_run++;                                                        \
        if (!(cond)) {                                                         \
            g_checks_failed++;                                                 \
            g_current_failed++;                                                \
            printf("      FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);\
        }                                                                      \
    } while (0)

/* Assertion with a printed value, for when "it was not equal" is not enough. */
#define CHECK_EQ_U(actual, expect)                                             \
    do {                                                                       \
        unsigned long long a_ = (unsigned long long)(actual);                  \
        unsigned long long e_ = (unsigned long long)(expect);                  \
        g_checks_run++;                                                        \
        if (a_ != e_) {                                                        \
            g_checks_failed++;                                                 \
            g_current_failed++;                                                \
            printf("      FAIL %s:%d  %s == %s  (got %llu, want %llu)\n",      \
                   __FILE__, __LINE__, #actual, #expect, a_, e_);              \
        }                                                                      \
    } while (0)

/* Fatal assertion: abandon this test case. */
#define REQUIRE(cond)                                                          \
    do {                                                                       \
        g_checks_run++;                                                        \
        if (!(cond)) {                                                         \
            g_checks_failed++;                                                 \
            g_current_failed++;                                                \
            printf("      ABORT %s:%d  REQUIRE(%s)\n", __FILE__, __LINE__, #cond);\
            return;                                                            \
        }                                                                      \
    } while (0)

#define RUN_TEST(fn)                                                           \
    do {                                                                       \
        g_tests_run++;                                                         \
        g_current_failed = 0;                                                  \
        printf("    %-52s", #fn);                                              \
        fflush(stdout);                                                        \
        printf("\n");                                                          \
        fn();                                                                  \
        if (g_current_failed) {                                                \
            g_tests_failed++;                                                  \
            printf("      -> FAILED (%d checks)\n", g_current_failed);         \
        }                                                                      \
    } while (0)

#define SUITE_BEGIN(name) printf("\n  [%s]\n", name)

/* Suites, defined one per file and called from test_main.c. */
void suite_nand(void);
void suite_mapping(void);
void suite_gc(void);
void suite_wear(void);
void suite_badblock(void);
void suite_journal(void);
void suite_ftl(void);

/* Shared helpers, defined in test_main.c. */
void     test_fill_pattern(uint8_t *buf, uint32_t lpn, uint32_t version);
bool     test_check_pattern(const uint8_t *buf, uint32_t lpn, uint32_t version);

#endif /* TEST_FRAMEWORK_H */
