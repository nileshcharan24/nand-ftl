/* =============================================================================
 * test_journal.c -- Pull the plug and switch it back on.
 * =============================================================================
 *
 * The model of a power cut here is exact and worth stating:
 *
 *      nand_power_cycle();                    // volatile state is gone
 *      ftl_mount(&params, mode);              // firmware boots and rebuilds
 *
 * ftl_mount() memsets the entire FTL context before doing anything, so there is
 * no way for a test to pass by accident because some variable survived. The
 * only thing that carries information across the cut is the media -- which is
 * exactly the situation the real product is in.
 *
 * Two flavours of cut are tested, and they are genuinely different:
 *
 *   CLEAN   ftl_flush() was called first, so a checkpoint describes everything.
 *           Mount should be fast and lossless.
 *
 *   DIRTY   power went away mid-workload. There is a checkpoint, but thousands
 *           of writes happened after it. Mount has to find them by scanning and
 *           roll them forward. This is the case that actually matters.
 * ========================================================================== */

#include "test_framework.h"
#include "ftl_internal.h"

static uint8_t wbuf[NAND_PAGE_SIZE_BYTES];
static uint8_t rbuf[NAND_PAGE_SIZE_BYTES];
static uint32_t ver[FTL_LOGICAL_PAGES];
static ftl_params_t g_p;

static void setup(ftl_map_scheme_t scheme, bool checkpointing)
{
    ftl_params_defaults(&g_p);
    g_p.mapping              = scheme;
    g_p.gc                   = FTL_GC_GREEDY;
    g_p.wear                 = FTL_WEAR_DYNAMIC;
    g_p.checkpointing        = checkpointing;
    g_p.read_disturb_refresh = false;
    g_p.seed                 = 31u;
    (void)ftl_format(&g_p);
    memset(ver, 0, sizeof(ver));
}

static void wr(uint32_t lpn)
{
    ver[lpn]++;
    test_fill_pattern(wbuf, lpn, ver[lpn]);
    CHECK(ftl_write(lpn, wbuf) == FTL_OK);
}

/* Verify every logical page that was ever written. Returns the mismatch count
 * so a test can be precise about how much, if anything, was lost. */
static uint32_t verify_all(void)
{
    uint32_t lpn, bad = 0u;

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        if (ver[lpn] == 0u) {
            continue;
        }
        if (ftl_read(lpn, rbuf) != FTL_OK ||
            !test_check_pattern(rbuf, lpn, ver[lpn])) {
            bad++;
        }
    }
    return bad;
}

static void power_cut_and_mount(ftl_recover_mode_t mode)
{
    nand_power_cycle();
    CHECK(ftl_mount(&g_p, mode) == FTL_OK);
}

/* ---------------------------------------------------------------------------
 * Clean shutdown: flush, cut power, mount. Nothing may be lost.
 * ------------------------------------------------------------------------- */
static void clean_shutdown(ftl_map_scheme_t scheme)
{
    uint32_t lpn;

    setup(scheme, true);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }
    CHECK(ftl_flush() == FTL_OK);

    power_cut_and_mount(FTL_RECOVER_CHECKPOINT);

    CHECK_EQ_U(verify_all(), 0u);
    CHECK(ftl_check_invariants());

    /* And the drive must be usable afterwards, not merely readable. */
    wr(7u);
    CHECK(ftl_read(7u, rbuf) == FTL_OK);
    CHECK(test_check_pattern(rbuf, 7u, ver[7u]));
    CHECK(ftl_check_invariants());
}

TEST_CASE(test_clean_shutdown_page)   { clean_shutdown(FTL_MAP_PAGE);   }
TEST_CASE(test_clean_shutdown_block)  { clean_shutdown(FTL_MAP_BLOCK);  }
TEST_CASE(test_clean_shutdown_hybrid) { clean_shutdown(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * Unclean shutdown mid-workload. This is the real test.
 * ------------------------------------------------------------------------- */
static void dirty_shutdown(ftl_map_scheme_t scheme)
{
    uint32_t lpn, i, lost;

    setup(scheme, true);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }
    /* Enough overwrites to be well past the last checkpoint, with garbage
     * collection running throughout. No flush: the power just goes. */
    for (i = 0u; i < FTL_LOGICAL_PAGES * 2u; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }

    power_cut_and_mount(FTL_RECOVER_CHECKPOINT);

    lost = verify_all();
    printf("        %s: %llu pages scanned, %llu applied, %u lost\n",
           ftl_map_scheme_name(scheme),
           (unsigned long long)ftl_stats()->recovery_pages_scanned,
           (unsigned long long)ftl_stats()->recovery_pages_applied,
           lost);

    CHECK_EQ_U(lost, 0u);
    CHECK(ftl_check_invariants());

    /* Keep writing after recovery and make sure nothing breaks. */
    for (i = 0u; i < FTL_LOGICAL_PAGES / 2u; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }
    CHECK_EQ_U(verify_all(), 0u);
    CHECK(ftl_check_invariants());
}

TEST_CASE(test_dirty_shutdown_page)   { dirty_shutdown(FTL_MAP_PAGE);   }
TEST_CASE(test_dirty_shutdown_block)  { dirty_shutdown(FTL_MAP_BLOCK);  }
TEST_CASE(test_dirty_shutdown_hybrid) { dirty_shutdown(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * The media must be self-describing: with checkpointing switched OFF entirely,
 * a full scan of the spare areas has to rebuild the whole mapping table.
 *
 * This is the property that means the design does not depend on its own
 * metadata being intact -- and it is the last-resort path every real product
 * keeps for when the metadata region is unreadable.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_full_scan_rebuilds_from_spare_areas_alone)
{
    uint32_t lpn, i;

    setup(FTL_MAP_PAGE, false);          /* no checkpoints will ever be written */

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }
    for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
        wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
    }
    CHECK_EQ_U(ftl_stats()->checkpoints_written, 0u);

    power_cut_and_mount(FTL_RECOVER_FULL_SCAN);

    CHECK_EQ_U(verify_all(), 0u);
    CHECK(ftl_check_invariants());
    printf("        rebuilt from %llu spare-area reads\n",
           (unsigned long long)ftl_stats()->recovery_pages_scanned);
}

/* ---------------------------------------------------------------------------
 * A checkpoint should make mounting cheaper than a full scan. If it does not,
 * it is costing flash writes for nothing.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_checkpoint_bounds_the_mount_scan)
{
    uint32_t lpn;
    uint64_t scanned_with_ckpt, scanned_full;

    setup(FTL_MAP_PAGE, true);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }
    CHECK(ftl_flush() == FTL_OK);
    CHECK(ftl_stats()->checkpoints_written > 0u);

    power_cut_and_mount(FTL_RECOVER_CHECKPOINT);
    scanned_with_ckpt = ftl_stats()->recovery_pages_scanned;
    CHECK_EQ_U(verify_all(), 0u);

    power_cut_and_mount(FTL_RECOVER_FULL_SCAN);
    scanned_full = ftl_stats()->recovery_pages_scanned;
    CHECK_EQ_U(verify_all(), 0u);

    printf("        checkpoint mount scanned %llu pages, full scan %llu\n",
           (unsigned long long)scanned_with_ckpt,
           (unsigned long long)scanned_full);

    CHECK(scanned_with_ckpt < scanned_full);
    CHECK(ftl_check_invariants());
}

/* ---------------------------------------------------------------------------
 * A torn checkpoint -- one whose header never made it -- must be ignored, and
 * the previous good checkpoint used instead. This is why the header is written
 * last, and it is worth proving rather than asserting.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_torn_checkpoint_is_ignored)
{
    uint32_t lpn, meta_pbn, meta_off, i;
    nand_spare_t sp;

    setup(FTL_MAP_PAGE, true);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES / 2u; lpn++) {
        wr(lpn);
    }
    CHECK(ftl_flush() == FTL_OK);          /* checkpoint 1: complete */

    /* Now simulate power dying partway through checkpoint 2, by programming
     * some payload pages into the metadata region with no header after them. */
    meta_pbn = g_ftl.meta_pbn;
    meta_off = g_ftl.meta_off;
    sp.lpn = 0u; sp.seq = g_ftl.seq + 1000u; sp.crc = 0u; sp.tag = NAND_TAG_CHECKPOINT;
    memset(wbuf, 0xA5, sizeof(wbuf));
    for (i = 0u; i < 3u && meta_off + i < NAND_PAGES_PER_BLOCK; i++) {
        sp.lpn = i;
        CHECK(nand_program_page(BLOCK_OFFSET_TO_PPN(meta_pbn, meta_off + i),
                                wbuf, &sp) == NAND_OK);
    }

    power_cut_and_mount(FTL_RECOVER_CHECKPOINT);

    /* The torn payload has no header, so recovery never looks at it and falls
     * back to checkpoint 1 plus a roll-forward. Nothing is lost. */
    CHECK_EQ_U(verify_all(), 0u);
    CHECK(ftl_check_invariants());
}

/* ---------------------------------------------------------------------------
 * Repeated power cuts. One successful recovery could be luck; twenty in a row
 * with interleaved work is evidence.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_repeated_power_cuts)
{
    uint32_t round, i, lpn;

    setup(FTL_MAP_PAGE, true);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        wr(lpn);
    }

    for (round = 0u; round < 20u; round++) {
        for (i = 0u; i < 500u; i++) {
            wr((uint32_t)(ftl_rng_next() % FTL_LOGICAL_PAGES));
        }
        power_cut_and_mount(FTL_RECOVER_CHECKPOINT);
        if (verify_all() != 0u) {
            printf("      data loss after power cut %u\n", round);
            CHECK_EQ_U(verify_all(), 0u);
            return;
        }
        CHECK(ftl_check_invariants());
    }
    CHECK(true);
}

void suite_journal(void)
{
    SUITE_BEGIN("journal -- checkpoints and power-loss recovery");
    RUN_TEST(test_clean_shutdown_page);
    RUN_TEST(test_clean_shutdown_block);
    RUN_TEST(test_clean_shutdown_hybrid);
    RUN_TEST(test_dirty_shutdown_page);
    RUN_TEST(test_dirty_shutdown_block);
    RUN_TEST(test_dirty_shutdown_hybrid);
    RUN_TEST(test_full_scan_rebuilds_from_spare_areas_alone);
    RUN_TEST(test_checkpoint_bounds_the_mount_scan);
    RUN_TEST(test_torn_checkpoint_is_ignored);
    RUN_TEST(test_repeated_power_cuts);
}
