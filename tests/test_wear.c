/* =============================================================================
 * test_wear.c -- Wear levelling: does the erase-count spread actually shrink?
 * =============================================================================
 *
 * The claim wear levelling makes is not "fewer erases" -- it causes slightly
 * MORE erases. The claim is "the same erases, spread evenly", and the metric
 * that captures it is the spread (max minus min) and the standard deviation.
 * These tests measure exactly that, by running identical workloads under
 * different policies and comparing.
 * ========================================================================== */

#include "test_framework.h"
#include "ftl_internal.h"

static uint8_t wbuf[NAND_PAGE_SIZE_BYTES];
static uint32_t ver[FTL_LOGICAL_PAGES];

static void setup(ftl_wear_policy_t policy)
{
    ftl_params_t p;

    ftl_params_defaults(&p);
    p.mapping              = FTL_MAP_PAGE;
    p.gc                   = FTL_GC_GREEDY;
    p.wear                 = policy;
    p.checkpointing        = false;
    p.read_disturb_refresh = false;
    p.seed                 = 17u;
    (void)ftl_format(&p);
    memset(ver, 0, sizeof(ver));
}

static void wr(uint32_t lpn)
{
    ver[lpn]++;
    test_fill_pattern(wbuf, lpn, ver[lpn]);
    CHECK(ftl_write(lpn, wbuf) == FTL_OK);
}

/* A workload with a genuinely cold region: the top half of the address space is
 * written once and never touched again, while the bottom half is hammered.
 * Without static levelling the blocks holding the cold half never return to the
 * free pool and never age. */
static void run_hot_cold_workload(uint32_t rounds)
{
    uint32_t i, lpn;
    uint64_t rng = 99u;

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);                                    /* fill everything once */
    }
    for (i = 0u; i < rounds; i++) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        wr((uint32_t)((rng >> 33) % (FTL_LOGICAL_PAGES / 2u)));   /* hot half only */
    }
}

/* ---------------------------------------------------------------------------
 * Dynamic levelling must beat no levelling at all.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_dynamic_beats_none)
{
    uint32_t none_min, none_max, dyn_min, dyn_max;
    double   none_sd, dyn_sd, mean;

    setup(FTL_WEAR_NONE);
    run_hot_cold_workload(FTL_LOGICAL_PAGES * 4u);
    ftl_erase_distribution(&none_min, &none_max, &mean, &none_sd);
    CHECK(ftl_check_invariants());

    setup(FTL_WEAR_DYNAMIC);
    run_hot_cold_workload(FTL_LOGICAL_PAGES * 4u);
    ftl_erase_distribution(&dyn_min, &dyn_max, &mean, &dyn_sd);
    CHECK(ftl_check_invariants());

    printf("        none:    spread %u  sd %.2f\n", none_max - none_min, none_sd);
    printf("        dynamic: spread %u  sd %.2f\n", dyn_max - dyn_min, dyn_sd);

    /* The whole point, in one assertion. */
    CHECK(dyn_sd < none_sd);
    CHECK((dyn_max - dyn_min) < (none_max - none_min));
}

/* ---------------------------------------------------------------------------
 * Static levelling must do something dynamic levelling cannot: raise the
 * MINIMUM erase count, by dragging cold data off the blocks that are stuck
 * holding it.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_static_levelling_moves_cold_data)
{
    uint32_t dyn_min, dyn_max, st_min, st_max;
    double   sd, mean;
    uint64_t migrations;

    setup(FTL_WEAR_DYNAMIC);
    run_hot_cold_workload(FTL_LOGICAL_PAGES * 8u);
    ftl_erase_distribution(&dyn_min, &dyn_max, &mean, &sd);

    setup(FTL_WEAR_STATIC);
    run_hot_cold_workload(FTL_LOGICAL_PAGES * 8u);
    ftl_erase_distribution(&st_min, &st_max, &mean, &sd);
    migrations = ftl_stats()->wear_migrations;

    printf("        dynamic: min %u  max %u  spread %u\n",
           dyn_min, dyn_max, dyn_max - dyn_min);
    printf("        static:  min %u  max %u  spread %u  (%llu migrations)\n",
           st_min, st_max, st_max - st_min, (unsigned long long)migrations);

    CHECK(migrations > 0u);
    CHECK((st_max - st_min) <= (dyn_max - dyn_min));
    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
}

/* ---------------------------------------------------------------------------
 * The allocator must hand out the least-worn free block. Checking the policy
 * directly makes a broken comparison obvious.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_allocator_prefers_least_worn_block)
{
    uint32_t pbn, chosen, chosen_ec, ec;

    setup(FTL_WEAR_DYNAMIC);

    /* Age a few blocks artificially by erasing them directly. */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < FTL_DATA_BLOCK_FIRST + 5u; pbn++) {
        uint32_t k;
        for (k = 0u; k < 10u; k++) {
            (void)nand_erase_block(pbn);
        }
    }

    chosen = wear_pick_free_block();
    REQUIRE(chosen != PBN_INVALID);
    chosen_ec = nand_block_erase_count(chosen);

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (g_ftl.blk[pbn].state == BLK_FREE && !nand_block_is_bad(pbn)) {
            ec = nand_block_erase_count(pbn);
            CHECK(chosen_ec <= ec);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Static levelling must not cost so much that it defeats the purpose. If the
 * migration traffic dominates the host traffic, the threshold is wrong.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_static_levelling_overhead_is_bounded)
{
    const ftl_stats_t *s;

    setup(FTL_WEAR_STATIC);
    run_hot_cold_workload(FTL_LOGICAL_PAGES * 8u);
    s = ftl_stats();

    printf("        wear-levelling pages moved: %llu of %llu host writes\n",
           (unsigned long long)s->pgm_wear, (unsigned long long)s->host_writes);

    /* Under 15% of host traffic is the design intent at the default threshold. */
    CHECK((double)s->pgm_wear < 0.15 * (double)s->host_writes);
}

void suite_wear(void)
{
    SUITE_BEGIN("wear -- erase-count levelling");
    RUN_TEST(test_dynamic_beats_none);
    RUN_TEST(test_static_levelling_moves_cold_data);
    RUN_TEST(test_allocator_prefers_least_worn_block);
    RUN_TEST(test_static_levelling_overhead_is_bounded);
}
