/* =============================================================================
 * test_badblock.c -- Blocks fail. The drive keeps working.
 * =============================================================================
 *
 * The interesting assertion in this file is not "the block got retired". It is
 * "the block got retired AND the data that was living in it is still readable".
 * Retiring a block is easy; retiring it without eating the user's data is the
 * part that takes care, and it is what these tests pin down.
 * ========================================================================== */

#include "test_framework.h"
#include "ftl_internal.h"

static uint8_t wbuf[NAND_PAGE_SIZE_BYTES];
static uint8_t rbuf[NAND_PAGE_SIZE_BYTES];
static uint32_t ver[FTL_LOGICAL_PAGES];

static void setup(ftl_map_scheme_t scheme)
{
    ftl_params_t p;

    ftl_params_defaults(&p);
    p.mapping              = scheme;
    p.gc                   = FTL_GC_GREEDY;
    p.wear                 = FTL_WEAR_DYNAMIC;
    p.checkpointing        = false;
    p.read_disturb_refresh = false;
    p.seed                 = 23u;
    (void)ftl_format(&p);
    memset(ver, 0, sizeof(ver));
}

static void wr(uint32_t lpn)
{
    ftl_err_t err;
    uint32_t next_version = ver[lpn] + 1u;

    test_fill_pattern(wbuf, lpn, next_version);
    err = ftl_write(lpn, wbuf);
    if (err == FTL_OK) {
        ver[lpn] = next_version;
    }
}

/* ---------------------------------------------------------------------------
 * Factory bad blocks must be discovered and excluded from the free pool, and
 * must never be erased -- erasing one destroys the marker that says it is bad.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_factory_bad_blocks_are_excluded)
{
    uint32_t pbn, bad = 0u;

    setup(FTL_MAP_PAGE);

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_factory_bad(pbn)) {
            bad++;
            CHECK(g_ftl.blk[pbn].state == BLK_BAD);
            CHECK_EQ_U(nand_block_erase_count(pbn), 0u);
        }
    }
    CHECK_EQ_U(bad, NAND_FACTORY_BAD_BLOCKS);
    CHECK_EQ_U(ftl_bad_block_count(), NAND_FACTORY_BAD_BLOCKS);

    /* The free pool holds every usable data block and nothing else. */
    CHECK_EQ_U(ftl_free_block_count(), FTL_DATA_BLOCKS - NAND_FACTORY_BAD_BLOCKS);
}

/* ---------------------------------------------------------------------------
 * Retiring a block by hand must rescue everything that was living in it.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_retire_rescues_the_data)
{
    uint32_t lpn, victim, i;

    setup(FTL_MAP_PAGE);
    for (lpn = 0u; lpn < NAND_PAGES_PER_BLOCK * 3u; lpn++) {
        wr(lpn);
    }

    /* Pick a block that is genuinely holding live data. */
    victim = PBN_INVALID;
    for (i = FTL_DATA_BLOCK_FIRST; i < NAND_NUM_BLOCKS; i++) {
        if (g_ftl.blk[i].valid_pages > 0u && i != g_ftl.active_pbn) {
            victim = i;
            break;
        }
    }
    REQUIRE(victim != PBN_INVALID);
    CHECK(g_ftl.blk[victim].valid_pages > 0u);

    CHECK(badblock_retire(victim, "test") == FTL_OK);

    CHECK(g_ftl.blk[victim].state == BLK_BAD);
    CHECK(nand_block_is_bad(victim));
    CHECK_EQ_U(g_ftl.blk[victim].valid_pages, 0u);
    CHECK_EQ_U(ftl_stats()->pages_lost, 0u);        /* nothing was thrown away */
    CHECK(ftl_stats()->pgm_recover > 0u);           /* it was moved, not lost  */

    /* Everything still reads back. */
    for (lpn = 0u; lpn < NAND_PAGES_PER_BLOCK * 3u; lpn++) {
        CHECK(ftl_read(lpn, rbuf) == FTL_OK);
        CHECK(test_check_pattern(rbuf, lpn, ver[lpn]));
    }
    CHECK(ftl_check_invariants());
}

/* ---------------------------------------------------------------------------
 * Now the realistic version: let the die fail programs at random during a long
 * workload, and require that the drive survives with all data intact.
 * ------------------------------------------------------------------------- */
static void survives_injected_failures(ftl_map_scheme_t scheme)
{
    uint32_t lpn, i, checked = 0u, mismatches = 0u;

    setup(scheme);

    /* 500 ppm is far worse than any real part, chosen so that a short test
     * still sees dozens of failures. */
    nand_set_failure_rates(500u, 200u);

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }
    nand_set_failure_rates(0u, 0u);

    printf("        retired %llu blocks, %llu retries, %llu pages lost\n",
           (unsigned long long)ftl_stats()->blocks_retired,
           (unsigned long long)ftl_stats()->program_retries,
           (unsigned long long)ftl_stats()->pages_lost);

    CHECK(ftl_stats()->blocks_retired > 0u);
    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        if (ver[lpn] == 0u) {
            continue;
        }
        checked++;
        if (ftl_read(lpn, rbuf) != FTL_OK ||
            !test_check_pattern(rbuf, lpn, ver[lpn])) {
            mismatches++;
        }
    }
    printf("        verified %u pages, %u mismatches\n", checked, mismatches);

    /* Any page the FTL reported as lost is accounted for; anything beyond that
     * is a bug. Being precise about the difference between "we know we lost it
     * and said so" and "it silently went wrong" is the whole point. */
    CHECK((uint64_t)mismatches <= ftl_stats()->pages_lost);
}

TEST_CASE(test_page_survives_failures)   { survives_injected_failures(FTL_MAP_PAGE);   }
TEST_CASE(test_block_survives_failures)  { survives_injected_failures(FTL_MAP_BLOCK);  }
TEST_CASE(test_hybrid_survives_failures) { survives_injected_failures(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * A retired block must never come back. If a bad block could be reallocated,
 * the drive would keep writing into a hole.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_retired_blocks_never_return_to_the_pool)
{
    uint32_t victim = FTL_DATA_BLOCK_FIRST + 3u;
    uint32_t i, allocated;

    setup(FTL_MAP_PAGE);
    CHECK(badblock_retire(victim, "test") == FTL_OK);

    for (i = 0u; i < FTL_DATA_BLOCKS; i++) {
        allocated = blockmgr_alloc();
        if (allocated == PBN_INVALID) {
            break;
        }
        CHECK(allocated != victim);
    }
    CHECK(ftl_check_invariants());
}

void suite_badblock(void)
{
    SUITE_BEGIN("badblock -- factory and grown bad blocks");
    RUN_TEST(test_factory_bad_blocks_are_excluded);
    RUN_TEST(test_retire_rescues_the_data);
    RUN_TEST(test_page_survives_failures);
    RUN_TEST(test_block_survives_failures);
    RUN_TEST(test_hybrid_survives_failures);
    RUN_TEST(test_retired_blocks_never_return_to_the_pool);
}
