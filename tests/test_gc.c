/* =============================================================================
 * test_gc.c -- Garbage collection: does it reclaim, and does the policy matter?
 * ========================================================================== */

#include "test_framework.h"
#include "ftl_internal.h"

static uint8_t wbuf[NAND_PAGE_SIZE_BYTES];
static uint8_t rbuf[NAND_PAGE_SIZE_BYTES];
static uint32_t ver[FTL_LOGICAL_PAGES];

static void setup(ftl_gc_policy_t policy)
{
    ftl_params_t p;

    ftl_params_defaults(&p);
    p.mapping              = FTL_MAP_PAGE;   /* GC only applies to page mapping */
    p.gc                   = policy;
    p.wear                 = FTL_WEAR_NONE;  /* isolate the GC effect           */
    p.checkpointing        = false;
    p.read_disturb_refresh = false;
    p.seed                 = 5u;
    (void)ftl_format(&p);
    memset(ver, 0, sizeof(ver));
}

static void wr(uint32_t lpn)
{
    ver[lpn]++;
    test_fill_pattern(wbuf, lpn, ver[lpn]);
    CHECK(ftl_write(lpn, wbuf) == FTL_OK);
}

/* Fill the exposed capacity once. After this the drive is "full" and any
 * further write must displace something -- the only regime where GC runs. */
static void precondition(void)
{
    uint32_t lpn;
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }
}

/* ---------------------------------------------------------------------------
 * On a fresh drive, GC must not run at all: there is nothing to reclaim.
 * A collector that fires early is burning erase cycles for nothing.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_no_gc_on_a_fresh_drive)
{
    uint32_t lpn;

    setup(FTL_GC_GREEDY);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES / 4u; lpn++) {
        wr(lpn);
    }
    CHECK_EQ_U(ftl_stats()->gc_victims, 0u);
    CHECK(ftl_write_amplification(false) == 1.0);
}

/* ---------------------------------------------------------------------------
 * Once the drive is full and being overwritten, GC must run, must free blocks,
 * and must not lose a single byte.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_gc_reclaims_without_losing_data)
{
    uint32_t i, lpn;

    setup(FTL_GC_GREEDY);
    precondition();

    for (i = 0u; i < FTL_LOGICAL_PAGES * 2u; i++) {
        wr((uint32_t)(i * 7919u % FTL_LOGICAL_PAGES));   /* scattered overwrite */
    }

    CHECK(ftl_stats()->gc_victims > 0u);
    CHECK(ftl_free_block_count() > 0u);
    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());

    /* Every logical page must still read back at its newest version. */
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        CHECK(ftl_read(lpn, rbuf) == FTL_OK);
        if (!test_check_pattern(rbuf, lpn, ver[lpn])) {
            CHECK_EQ_U(lpn, 0xDEADBEEFu);   /* force a labelled failure */
            break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Write amplification must exceed 1 once the drive is in steady state -- and
 * must stay bounded. An unbounded WA means the collector is thrashing.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_steady_state_write_amplification_is_sane)
{
    uint32_t i;
    double   wa;

    setup(FTL_GC_GREEDY);
    precondition();
    ftl_stats_reset();

    for (i = 0u; i < FTL_LOGICAL_PAGES * 3u; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }

    wa = ftl_write_amplification(false);
    CHECK(wa > 1.0);
    CHECK(wa < (double)NAND_PAGES_PER_BLOCK);

    /* At 25% over-provisioning under uniform random writes, the classic
     * approximation  WA ~ 1 / (2 * OP_fraction)  puts the expected value around
     * 2. Being within a wide band of that is the sanity check; the exact number
     * is reported in DOCS/RESULTS.md rather than pinned here, because pinning a
     * measured constant in a test makes the test fragile for no benefit. */
    CHECK(wa < 6.0);
}

/* ---------------------------------------------------------------------------
 * Greedy must genuinely pick the block with the fewest valid pages.
 * Testing the policy directly, rather than inferring it from the WA number,
 * means a broken comparison shows up immediately.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_greedy_picks_the_emptiest_block)
{
    uint32_t i, victim, pbn, min_valid = 0xFFFFFFFFu;

    setup(FTL_GC_GREEDY);
    precondition();
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }

    victim = gc_select_victim();
    REQUIRE(victim != PBN_INVALID);

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (g_ftl.blk[pbn].state == BLK_DATA && pbn != g_ftl.active_pbn) {
            if (g_ftl.blk[pbn].valid_pages < min_valid) {
                min_valid = g_ftl.blk[pbn].valid_pages;
            }
        }
    }
    CHECK_EQ_U(g_ftl.blk[victim].valid_pages, min_valid);
}

/* ---------------------------------------------------------------------------
 * All four policies must be correct, whatever their cost. Correctness is not
 * negotiable; efficiency is what the benchmark is for.
 * ------------------------------------------------------------------------- */
static void policy_is_correct(ftl_gc_policy_t policy)
{
    uint32_t i, lpn;

    setup(policy);
    precondition();
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }
    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn += 37u) {
        CHECK(ftl_read(lpn, rbuf) == FTL_OK);
        CHECK(test_check_pattern(rbuf, lpn, ver[lpn]));
    }
}

TEST_CASE(test_policy_greedy)       { policy_is_correct(FTL_GC_GREEDY);       }
TEST_CASE(test_policy_cost_benefit) { policy_is_correct(FTL_GC_COST_BENEFIT); }
TEST_CASE(test_policy_fifo)         { policy_is_correct(FTL_GC_FIFO);         }
TEST_CASE(test_policy_random)       { policy_is_correct(FTL_GC_RANDOM);       }

/* ---------------------------------------------------------------------------
 * A block that has become entirely invalid must cost nothing to collect: zero
 * page copies, one erase. If this is not true, the accounting is wrong.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_collecting_a_dead_block_is_free)
{
    uint32_t i, lpn, victim;
    uint64_t copies_before;

    setup(FTL_GC_GREEDY);

    /* Write one block's worth of pages, then overwrite all of them, which makes
     * the first physical block entirely invalid. */
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        wr(i);
    }
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        wr(i);
    }

    victim = gc_select_victim();
    REQUIRE(victim != PBN_INVALID);
    CHECK_EQ_U(g_ftl.blk[victim].valid_pages, 0u);

    copies_before = ftl_stats()->pgm_gc;
    CHECK(gc_force_once() == FTL_OK);
    CHECK_EQ_U(ftl_stats()->pgm_gc, copies_before);   /* nothing was copied */

    for (lpn = 0u; lpn < NAND_PAGES_PER_BLOCK; lpn++) {
        CHECK(ftl_read(lpn, rbuf) == FTL_OK);
        CHECK(test_check_pattern(rbuf, lpn, ver[lpn]));
    }
    CHECK(ftl_check_invariants());
}

/* ---------------------------------------------------------------------------
 * TRIM must reduce the amount of work GC has to do. This is the measurable
 * justification for the whole TRIM command.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_trim_reduces_gc_work)
{
    uint32_t i;
    uint64_t copies_without, copies_with;

    /* Run A: no TRIM. The FTL is obliged to preserve every page. */
    setup(FTL_GC_GREEDY);
    precondition();
    ftl_stats_reset();
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        wr((uint32_t)(i % (FTL_LOGICAL_PAGES / 4u)) * 4u);
    }
    copies_without = ftl_stats()->pgm_gc;

    /* Run B: identical, but the host first tells the FTL that three quarters of
     * the address space is dead. */
    setup(FTL_GC_GREEDY);
    precondition();
    for (i = FTL_LOGICAL_PAGES / 4u; i < FTL_LOGICAL_PAGES; i++) {
        CHECK(ftl_trim(i) == FTL_OK);
    }
    ftl_stats_reset();
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        wr((uint32_t)(i % (FTL_LOGICAL_PAGES / 4u)) * 4u);
    }
    copies_with = ftl_stats()->pgm_gc;

    CHECK(copies_with < copies_without);
    CHECK(ftl_check_invariants());
}

void suite_gc(void)
{
    SUITE_BEGIN("gc -- garbage collection");
    RUN_TEST(test_no_gc_on_a_fresh_drive);
    RUN_TEST(test_gc_reclaims_without_losing_data);
    RUN_TEST(test_steady_state_write_amplification_is_sane);
    RUN_TEST(test_greedy_picks_the_emptiest_block);
    RUN_TEST(test_policy_greedy);
    RUN_TEST(test_policy_cost_benefit);
    RUN_TEST(test_policy_fifo);
    RUN_TEST(test_policy_random);
    RUN_TEST(test_collecting_a_dead_block_is_free);
    RUN_TEST(test_trim_reduces_gc_work);
}
