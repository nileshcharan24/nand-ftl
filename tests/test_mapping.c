/* =============================================================================
 * test_mapping.c -- All three translation schemes, same contract.
 * =============================================================================
 *
 * The point of this file is that the three schemes must be INDISTINGUISHABLE
 * from the host's point of view. They differ enormously in RAM cost and in how
 * much flash work they cause -- and not at all in what a read returns. Every
 * correctness test here is therefore run against all three.
 * ========================================================================== */

#include "test_framework.h"
#include "ftl_internal.h"

static uint8_t wbuf[NAND_PAGE_SIZE_BYTES];
static uint8_t rbuf[NAND_PAGE_SIZE_BYTES];

static void setup(ftl_map_scheme_t scheme)
{
    ftl_params_t p;

    ftl_params_defaults(&p);
    p.mapping              = scheme;
    p.checkpointing        = false;   /* keep the counters about data only */
    p.read_disturb_refresh = false;
    p.seed                 = 11u;
    (void)ftl_format(&p);
}

static void write_lpn(uint32_t lpn, uint32_t version)
{
    test_fill_pattern(wbuf, lpn, version);
    CHECK(ftl_write(lpn, wbuf) == FTL_OK);
}

static void expect_lpn(uint32_t lpn, uint32_t version)
{
    CHECK(ftl_read(lpn, rbuf) == FTL_OK);
    CHECK(test_check_pattern(rbuf, lpn, version));
}

/* ---------------------------------------------------------------------------
 * The basic contract, for each scheme.
 * ------------------------------------------------------------------------- */
static void basic_contract(ftl_map_scheme_t scheme)
{
    uint32_t lpn;

    setup(scheme);

    /* Reading something never written is an error, not stale data. */
    CHECK(ftl_read(0u, rbuf) == FTL_ERR_UNWRITTEN);

    /* Out-of-range addresses are rejected rather than wrapping. An FTL that
     * silently wraps an LPN corrupts a completely unrelated file. */
    CHECK(ftl_write(FTL_LOGICAL_PAGES, wbuf) == FTL_ERR_RANGE);
    CHECK(ftl_read(FTL_LOGICAL_PAGES, rbuf)  == FTL_ERR_RANGE);

    /* Write a page, read it back. */
    write_lpn(5u, 1u);
    expect_lpn(5u, 1u);

    /* Overwrite it several times: each read must see the newest version. This
     * is out-of-place update working -- physically the page moves every time. */
    for (lpn = 2u; lpn <= 10u; lpn++) {
        write_lpn(5u, lpn);
        expect_lpn(5u, lpn);
    }

    /* Neighbouring pages must be untouched by all of that. */
    CHECK(ftl_read(4u, rbuf) == FTL_ERR_UNWRITTEN);
    CHECK(ftl_read(6u, rbuf) == FTL_ERR_UNWRITTEN);

    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
}

TEST_CASE(test_page_basic)   { basic_contract(FTL_MAP_PAGE);   }
TEST_CASE(test_block_basic)  { basic_contract(FTL_MAP_BLOCK);  }
TEST_CASE(test_hybrid_basic) { basic_contract(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * A whole logical block, written out of order, then verified.
 * This is the case block mapping finds hardest and hybrid mapping was invented
 * for, so it is the one worth being exhaustive about.
 * ------------------------------------------------------------------------- */
static void out_of_order_block(ftl_map_scheme_t scheme)
{
    uint32_t i, off, base = 3u * NAND_PAGES_PER_BLOCK;
    static uint32_t ver[NAND_PAGES_PER_BLOCK];

    setup(scheme);
    memset(ver, 0, sizeof(ver));

    /* Write the offsets of one logical block in reverse order. */
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        off = NAND_PAGES_PER_BLOCK - 1u - i;
        ver[off] = 1u;
        write_lpn(base + off, 1u);
    }
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
        expect_lpn(base + off, 1u);
    }
    CHECK(ftl_check_invariants());

    /* Now overwrite every other offset twice and re-verify everything. */
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off += 2u) {
        ver[off] = 2u;
        write_lpn(base + off, 2u);
        ver[off] = 3u;
        write_lpn(base + off, 3u);
    }
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
        expect_lpn(base + off, ver[off]);
    }

    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
}

TEST_CASE(test_page_out_of_order)   { out_of_order_block(FTL_MAP_PAGE);   }
TEST_CASE(test_block_out_of_order)  { out_of_order_block(FTL_MAP_BLOCK);  }
TEST_CASE(test_hybrid_out_of_order) { out_of_order_block(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * TRIM
 * ------------------------------------------------------------------------- */
static void trim_contract(ftl_map_scheme_t scheme)
{
    setup(scheme);

    write_lpn(100u, 1u);
    expect_lpn(100u, 1u);

    CHECK(ftl_trim(100u) == FTL_OK);
    CHECK(ftl_read(100u, rbuf) == FTL_ERR_UNWRITTEN);

    /* And the space is genuinely reclaimable: writing it again works. */
    write_lpn(100u, 2u);
    expect_lpn(100u, 2u);

    CHECK(ftl_check_invariants());
}

TEST_CASE(test_page_trim)   { trim_contract(FTL_MAP_PAGE);   }
TEST_CASE(test_block_trim)  { trim_contract(FTL_MAP_BLOCK);  }
TEST_CASE(test_hybrid_trim) { trim_contract(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * Out-of-place update: the physical location must actually change.
 * If it did not, the FTL would be violating program-once and the die would have
 * rejected it -- but proving it explicitly documents the intent.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_updates_are_out_of_place)
{
    uint32_t first, second;

    setup(FTL_MAP_PAGE);
    write_lpn(42u, 1u);
    first = ftl_lookup(42u);
    write_lpn(42u, 2u);
    second = ftl_lookup(42u);

    CHECK(first != PPN_INVALID);
    CHECK(second != PPN_INVALID);
    CHECK(first != second);

    /* The old physical page still holds the OLD data -- it has not been erased,
     * it has merely been orphaned. That orphan is what garbage collection is
     * for, and it is exactly why write amplification exists. */
    CHECK(nand_page_is_programmed(first));
}

/* ---------------------------------------------------------------------------
 * Sequential writing is the case every scheme should handle cheaply.
 * The measured write amplification here should be almost exactly 1.
 * ------------------------------------------------------------------------- */
static void sequential_is_cheap(ftl_map_scheme_t scheme)
{
    uint32_t lpn;
    double   wa;

    setup(scheme);
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES / 2u; lpn++) {
        write_lpn(lpn, 1u);
    }
    wa = ftl_write_amplification(false);

    /* A little above 1.0 is fine (padding, the odd merge); anything near the
     * pages-per-block figure would mean the sequential fast path is broken. */
    CHECK(wa >= 1.0);
    CHECK(wa < 1.30);
    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
}

TEST_CASE(test_page_sequential_cheap)   { sequential_is_cheap(FTL_MAP_PAGE);   }
TEST_CASE(test_block_sequential_cheap)  { sequential_is_cheap(FTL_MAP_BLOCK);  }
TEST_CASE(test_hybrid_sequential_cheap) { sequential_is_cheap(FTL_MAP_HYBRID); }

/* ---------------------------------------------------------------------------
 * The RAM trade-off, asserted rather than merely described.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_mapping_table_sizes)
{
    size_t page_bytes, block_bytes, hybrid_bytes;

    setup(FTL_MAP_PAGE);
    page_bytes = ftl_mapping_table_bytes();
    setup(FTL_MAP_BLOCK);
    block_bytes = ftl_mapping_table_bytes();
    setup(FTL_MAP_HYBRID);
    hybrid_bytes = ftl_mapping_table_bytes();

    /* Page mapping costs exactly PAGES_PER_BLOCK times more than block mapping.
     * That factor is the entire reason the other two schemes exist. */
    CHECK_EQ_U(page_bytes, (size_t)FTL_LOGICAL_PAGES * 4u);
    CHECK_EQ_U(block_bytes, (size_t)FTL_LOGICAL_BLOCKS * 4u);
    CHECK_EQ_U(page_bytes / block_bytes, NAND_PAGES_PER_BLOCK);

    /* Hybrid sits between them: block table plus log descriptors. */
    CHECK(hybrid_bytes > block_bytes);
    CHECK(hybrid_bytes < page_bytes);
}

/* ---------------------------------------------------------------------------
 * A block-mapped random overwrite must trigger a read-modify-write, and it must
 * cost roughly a block's worth of programs. This is the pathology the scheme is
 * famous for, and measuring it is more convincing than asserting it.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_block_mapping_rmw_cost)
{
    const ftl_stats_t *s;
    uint32_t           off, base = 0u;
    uint64_t           rmw_before;

    setup(FTL_MAP_BLOCK);

    /* Fill one logical block sequentially: cheap, no RMW. */
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
        write_lpn(base + off, 1u);
    }
    s = ftl_stats();
    CHECK_EQ_U(s->pgm_rmw, 0u);

    /* Now overwrite ONE page in the middle. */
    rmw_before = s->pgm_rmw;
    write_lpn(base + 10u, 2u);

    /* Rebuilding the block copies every other live page across. */
    CHECK(s->pgm_rmw - rmw_before == NAND_PAGES_PER_BLOCK - 1u);
    expect_lpn(base + 10u, 2u);
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
        if (off != 10u) {
            expect_lpn(base + off, 1u);
        }
    }
    CHECK(ftl_check_invariants());
}

/* ---------------------------------------------------------------------------
 * Hybrid mapping must produce a SWITCH merge (the free one) when a logical
 * block is rewritten sequentially, and a FULL merge when it is rewritten
 * randomly. Distinguishing the two is the whole reason hybrid mapping works.
 * ------------------------------------------------------------------------- */
TEST_CASE(test_hybrid_switch_vs_full_merge)
{
    const ftl_stats_t *s = ftl_stats();
    uint32_t           i, off, base;
    uint64_t           sw_before, full_before;

    setup(FTL_MAP_HYBRID);

    /* Fill enough logical blocks to exhaust the log pool and force merges. */
    for (i = 0u; i <= FTL_LOG_BLOCK_POOL; i++) {
        base = i * NAND_PAGES_PER_BLOCK;
        for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
            write_lpn(base + off, 1u);
        }
    }

    /* Rewriting a whole logical block IN ORDER should merge by switching. */
    sw_before = s->switch_merges;
    base = 0u;
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
        write_lpn(base + off, 2u);
    }
    for (i = 1u; i <= FTL_LOG_BLOCK_POOL; i++) {
        base = i * NAND_PAGES_PER_BLOCK;
        write_lpn(base, 3u);          /* churn the pool so block 0 gets evicted */
    }
    CHECK(s->switch_merges > sw_before);

    /* Rewriting scattered offsets should force a full merge instead. */
    full_before = s->full_merges;
    base = 0u;
    for (off = NAND_PAGES_PER_BLOCK; off > 0u; off -= 2u) {
        write_lpn(base + off - 1u, 4u);
    }
    for (i = 1u; i <= FTL_LOG_BLOCK_POOL; i++) {
        write_lpn(i * NAND_PAGES_PER_BLOCK + 1u, 5u);
    }
    CHECK(s->full_merges > full_before);

    CHECK(ftl_check_invariants());
    CHECK(ftl_stats_selfcheck());
}

void suite_mapping(void)
{
    SUITE_BEGIN("mapping -- page, block and hybrid translation");
    RUN_TEST(test_page_basic);
    RUN_TEST(test_block_basic);
    RUN_TEST(test_hybrid_basic);
    RUN_TEST(test_page_out_of_order);
    RUN_TEST(test_block_out_of_order);
    RUN_TEST(test_hybrid_out_of_order);
    RUN_TEST(test_page_trim);
    RUN_TEST(test_block_trim);
    RUN_TEST(test_hybrid_trim);
    RUN_TEST(test_updates_are_out_of_place);
    RUN_TEST(test_page_sequential_cheap);
    RUN_TEST(test_block_sequential_cheap);
    RUN_TEST(test_hybrid_sequential_cheap);
    RUN_TEST(test_mapping_table_sizes);
    RUN_TEST(test_block_mapping_rmw_cost);
    RUN_TEST(test_hybrid_switch_vs_full_merge);
}
