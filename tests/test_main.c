/* =============================================================================
 * test_main.c -- Test runner and shared helpers.
 * ========================================================================== */

#include <stdlib.h>

#include "test_framework.h"
#include "ftl.h"

int g_tests_run;
int g_tests_failed;
int g_checks_run;
int g_checks_failed;
int g_current_failed;

/* The page-content pattern used everywhere. Encoding both the LPN and a version
 * number into the page is what lets a test prove not merely that a read
 * succeeded, but that it returned the RIGHT page at the RIGHT version -- the
 * two ways an FTL can be wrong while appearing to work. */
void test_fill_pattern(uint8_t *buf, uint32_t lpn, uint32_t version)
{
    uint32_t i;
    uint32_t *w = (uint32_t *)(void *)buf;

    for (i = 0u; i < NAND_PAGE_SIZE_BYTES / sizeof(uint32_t); i += 2u) {
        w[i]      = lpn ^ (i << 16);
        w[i + 1u] = version;
    }
}

bool test_check_pattern(const uint8_t *buf, uint32_t lpn, uint32_t version)
{
    uint32_t i;
    const uint32_t *w = (const uint32_t *)(const void *)buf;

    for (i = 0u; i < NAND_PAGE_SIZE_BYTES / sizeof(uint32_t); i += 2u) {
        if (w[i] != (lpn ^ (i << 16)) || w[i + 1u] != version) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    printf("================================================================\n");
    printf(" NAND FTL simulator -- unit tests\n");
    printf("================================================================\n");
    printf(" geometry: %u blocks x %u pages x %u B  =  %.1f MiB physical\n",
           (unsigned)NAND_NUM_BLOCKS, (unsigned)NAND_PAGES_PER_BLOCK,
           (unsigned)NAND_PAGE_SIZE_BYTES, (double)NAND_TOTAL_BYTES / 1048576.0);
    printf(" exposed:  %u logical pages (%u logical blocks), OP %u%%\n",
           (unsigned)FTL_LOGICAL_PAGES, (unsigned)FTL_LOGICAL_BLOCKS,
           (unsigned)FTL_OP_PERCENT);

    suite_nand();
    suite_mapping();
    suite_gc();
    suite_wear();
    suite_badblock();
    suite_journal();
    suite_ftl();

    printf("\n================================================================\n");
    printf(" %d tests run, %d failed   |   %d checks run, %d failed\n",
           g_tests_run, g_tests_failed, g_checks_run, g_checks_failed);
    printf(" %s\n", g_checks_failed == 0 ? "ALL TESTS PASSED" : "*** FAILURES ***");
    printf("================================================================\n");

    return (g_checks_failed == 0) ? 0 : 1;
}
