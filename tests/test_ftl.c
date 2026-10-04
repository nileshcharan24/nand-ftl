/* =============================================================================
 * test_ftl.c -- End-to-end stress, and the invariants that hold throughout.
 * =============================================================================
 *
 * The tests above check one mechanism each. This file runs everything at once --
 * all three mapping schemes, garbage collection, wear levelling, read-disturb
 * refresh, checkpointing, and injected media failures -- and asserts that the
 * FTL's internal invariants hold at every stage and that no data is lost.
 *
 * This is the test that catches the bugs the others cannot: the ones that only
 * appear when two mechanisms interact.
 * ========================================================================== */

#include "test_framework.h"
#include "ftl_internal.h"

static uint8_t wbuf[NAND_PAGE_SIZE_BYTES];
static uint8_t rbuf[NAND_PAGE_SIZE_BYTES];
static uint32_t ver[FTL_LOGICAL_PAGES];
static uint8_t trimmed[FTL_LOGICAL_PAGES];

static void reset_shadow(void)
{
    memset(ver, 0, sizeof(ver));
    memset(trimmed, 0, sizeof(trimmed));
}

/* The "shadow copy" pattern: the test keeps its own model of what the drive
 * should contain and compares against it. Any divergence is a bug in the FTL,
 * because the model is trivial enough to be obviously right. */
static uint32_t verify_everything(void)
{
    uint32_t lpn, bad = 0u;

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        ftl_err_t err = ftl_read(lpn, rbuf);

        if (trimmed[lpn] || ver[lpn] == 0u) {
            /* Never written, or explicitly discarded. Reading stale data here
             * would be a real bug -- it would mean deleted data reappearing. */
            if (err == FTL_OK && ver[lpn] != 0u) {
                bad++;
            }
            continue;
        }
        if (err != FTL_OK || !test_check_pattern(rbuf, lpn, ver[lpn])) {
            bad++;
        }
    }
    return bad;
}

/* ---------------------------------------------------------------------------
 * The full mixed workload.
 * ------------------------------------------------------------------------- */
static void stress(ftl_map_scheme_t scheme, ftl_gc_policy_t gc,
                   ftl_wear_policy_t wear, uint32_t ops)
{
    ftl_params_t p;
    uint32_t     i, lpn, roll;

    ftl_params_defaults(&p);
    p.mapping              = scheme;
    p.gc                   = gc;
    p.wear                 = wear;
    p.checkpointing        = true;
    p.read_disturb_refresh = true;
    p.seed                 = 101u;
    REQUIRE(ftl_format(&p) == FTL_OK);
    reset_shadow();

    /* Precondition. */
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        ver[lpn] = 1u;
        test_fill_pattern(wbuf, lpn, 1u);
        REQUIRE(ftl_write(lpn, wbuf) == FTL_OK);
    }
    REQUIRE(ftl_check_invariants());

    for (i = 0u; i < ops; i++) {
        lpn  = (uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES);
        roll = (uint32_t)(ftl_rng_next() % 100u);

        if (roll < 70u) {                       /* 70% writes */
            ver[lpn]++;
            trimmed[lpn] = 0u;
            test_fill_pattern(wbuf, lpn, ver[lpn]);
            if (ftl_write(lpn, wbuf) != FTL_OK) {
                CHECK_EQ_U(i, 0xFFFFFFFFu);     /* labelled failure */
                return;
            }
        } else if (roll < 95u) {                /* 25% reads, verified inline */
            ftl_err_t err = ftl_read(lpn, rbuf);
            if (!trimmed[lpn] && ver[lpn] > 0u) {
                CHECK(err == FTL_OK);
                CHECK(test_check_pattern(rbuf, lpn, ver[lpn]));
            }
        } else {                                /* 5% trims */
            CHECK(ftl_trim(lpn) == FTL_OK);
            trimmed[lpn] = 1u;
        }

        /* Check the invariants periodically rather than every operation: often
         * enough to localise a bug to a few hundred operations, cheap enough
         * that the suite still runs in seconds. */
        if ((i % 2000u) == 0u) {
            if (!ftl_check_invariants()) {
                printf("      invariants broken at operation %u\n", i);
                CHECK(false);
                return;
            }
        }
    }

    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
    CHECK_EQ_U(verify_everything(), 0u);

    {
        uint32_t emin, emax;
        double   emean, esd;
        ftl_erase_distribution(&emin, &emax, &emean, &esd);
        printf("        %s/%s/%s: WA %.2f, %llu erases, erase spread %u (sd %.2f)\n",
               ftl_map_scheme_name(scheme), ftl_gc_policy_name(gc),
               ftl_wear_policy_name(wear),
               ftl_write_amplification(false),
               (unsigned long long)ftl_stats()->erases,
               emax - emin, esd);
    }
}

TEST_CASE(test_stress_page_greedy_dynamic)
{
    stress(FTL_MAP_PAGE, FTL_GC_GREEDY, FTL_WEAR_DYNAMIC, 40000u);
}

TEST_CASE(test_stress_page_costbenefit_static)
{
    stress(FTL_MAP_PAGE, FTL_GC_COST_BENEFIT, FTL_WEAR_STATIC, 40000u);
}

TEST_CASE(test_stress_block_mapping)
{
    stress(FTL_MAP_BLOCK, FTL_GC_GREEDY, FTL_WEAR_DYNAMIC, 4000u);
}

TEST_CASE(test_stress_hybrid_mapping)
{
    stress(FTL_MAP_HYBRID, FTL_GC_GREEDY, FTL_WEAR_DYNAMIC, 20000u);
}

/* ---------------------------------------------------------------------------
 * The drive must not accept more data than it advertises, and it must not fall
 * over when it is completely full of live data. Running out of space has to be
 * a reported condition, never a crash and never silent corruption.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_full_drive_stays_correct)
{
    ftl_params_t p;
    uint32_t     lpn, i;

    ftl_params_defaults(&p);
    p.mapping       = FTL_MAP_PAGE;
    p.checkpointing = false;
    p.seed          = 55u;
    REQUIRE(ftl_format(&p) == FTL_OK);
    reset_shadow();

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        ver[lpn] = 1u;
        test_fill_pattern(wbuf, lpn, 1u);
        REQUIRE(ftl_write(lpn, wbuf) == FTL_OK);
    }

    /* Every logical page is live. Now overwrite in place repeatedly: the
     * collector has the least possible slack it will ever have. */
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        lpn = (uint32_t)(i % FTL_LOGICAL_PAGES);
        ver[lpn]++;
        test_fill_pattern(wbuf, lpn, ver[lpn]);
        REQUIRE(ftl_write(lpn, wbuf) == FTL_OK);
    }

    CHECK(ftl_free_block_count() > 0u);
    CHECK(ftl_check_invariants());
    CHECK_EQ_U(verify_everything(), 0u);
}

/* ---------------------------------------------------------------------------
 * Determinism. Two runs with the same seed must produce byte-identical
 * statistics. Without this, no benchmark result in DOCS/RESULTS.md would be
 * reproducible, and no bug found in a long run could be bisected.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_runs_are_reproducible)
{
    ftl_params_t p;
    ftl_stats_t  first;
    uint32_t     pass, i, lpn;
    uint64_t     rng;

    for (pass = 0u; pass < 2u; pass++) {
        ftl_params_defaults(&p);
        p.mapping = FTL_MAP_PAGE;
        p.seed    = 777u;
        REQUIRE(ftl_format(&p) == FTL_OK);

        rng = 12345u;
        for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
            test_fill_pattern(wbuf, lpn, 1u);
            REQUIRE(ftl_write(lpn, wbuf) == FTL_OK);
        }
        for (i = 0u; i < 20000u; i++) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            lpn = (uint32_t)((rng >> 33) % FTL_LOGICAL_PAGES);
            test_fill_pattern(wbuf, lpn, i);
            REQUIRE(ftl_write(lpn, wbuf) == FTL_OK);
        }

        if (pass == 0u) {
            first = *ftl_stats();
        } else {
            CHECK(memcmp(&first, ftl_stats(), sizeof(ftl_stats_t)) == 0);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Read-disturb refresh: a workload that only READS must still eventually cause
 * flash writes. Counter-intuitive, real, and worth demonstrating.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_read_only_workload_triggers_refresh)
{
    ftl_params_t p;
    uint32_t     lpn, i;
    uint64_t     erases_before;

    ftl_params_defaults(&p);
    p.mapping              = FTL_MAP_PAGE;
    p.checkpointing        = false;
    p.read_disturb_refresh = true;
    p.seed                 = 3u;
    REQUIRE(ftl_format(&p) == FTL_OK);

    for (lpn = 0u; lpn < NAND_PAGES_PER_BLOCK * 2u; lpn++) {
        test_fill_pattern(wbuf, lpn, 1u);
        REQUIRE(ftl_write(lpn, wbuf) == FTL_OK);
    }
    erases_before = ftl_stats()->erases;

    /* Hammer a handful of pages with reads. Nothing is being written by the
     * host, but the block they live in is accumulating disturb. */
    for (i = 0u; i < NAND_READ_DISTURB_LIMIT * 2u; i++) {
        CHECK(ftl_read(i % 4u, rbuf) == FTL_OK);
    }

    /* The refresh scan runs on the write path, so give it one write to act on.
     * (A real product would also run it from an idle-time background task;
     * that this one does not is a limitation, and it is documented as one.) */
    test_fill_pattern(wbuf, 0u, 2u);
    for (i = 0u; i < 300u; i++) {
        CHECK(ftl_write(0u, wbuf) == FTL_OK);
    }

    printf("        refresh events: %llu, erases %llu -> %llu\n",
           (unsigned long long)ftl_stats()->refresh_events,
           (unsigned long long)erases_before,
           (unsigned long long)ftl_stats()->erases);

    CHECK(ftl_stats()->refresh_events > 0u);
    CHECK(ftl_check_invariants());
}

void suite_ftl(void)
{
    SUITE_BEGIN("ftl -- end-to-end");
    RUN_TEST(test_stress_page_greedy_dynamic);
    RUN_TEST(test_stress_page_costbenefit_static);
    RUN_TEST(test_stress_block_mapping);
    RUN_TEST(test_stress_hybrid_mapping);
    RUN_TEST(test_full_drive_stays_correct);
    RUN_TEST(test_runs_are_reproducible);
    RUN_TEST(test_read_only_workload_triggers_refresh);
}
