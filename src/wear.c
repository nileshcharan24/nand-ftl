/* =============================================================================
 * wear.c -- Wear levelling: making all the blocks die at the same time.
 * =============================================================================
 *
 * THE PROBLEM, STATED PRECISELY
 * -----------------------------
 * A block survives about NAND_MAX_ERASE_CYCLES erases. A drive is dead when it
 * runs out of usable blocks -- not when the average block is worn out, but when
 * enough individual blocks are. So the enemy is not the mean erase count, it is
 * the SPREAD.
 *
 * Imagine 128 blocks rated for 3000 erases. If every block wears evenly, the
 * drive absorbs 128 * 3000 = 384,000 erases before anything fails. If the FTL
 * keeps reusing the same 10 blocks, those die after 30,000 erases total and the
 * other 118 blocks are still factory-fresh. Same media, an order of magnitude
 * less life, and it is purely an allocation-policy decision.
 *
 * This is why the metric reported in DOCS/RESULTS.md is the erase-count
 * standard deviation and max-minus-min spread, not the total erase count. The
 * total is set by write amplification; the spread is set by this file.
 *
 * TWO KINDS OF LEVELLING, AND WHY YOU NEED BOTH
 * ---------------------------------------------
 * DYNAMIC levelling is free: when you have to pick a free block anyway, pick
 * the one with the fewest erases. It costs nothing and it perfectly levels
 * every block that participates in the free pool.
 *
 * And that "participates" is the catch. Consider a block holding data written
 * once and never touched again -- an operating-system image, say. It never
 * becomes invalid, so it is never garbage-collected, so it never returns to the
 * free pool, so dynamic levelling never sees it. It sits at erase count 3 while
 * the rest of the drive climbs past 2000. Its endurance is simply wasted.
 *
 * STATIC levelling fixes that, and the only way to fix it is to do something
 * that feels wrong: deliberately move data that nobody asked you to move,
 * spending flash writes to buy endurance. When the gap between the most-erased
 * and least-erased block grows past FTL_WEAR_GAP_THRESHOLD, we pick the coldest
 * young block, relocate its contents, and let it join the pool.
 *
 * The threshold is the whole design. Too small and the drive burns itself out
 * shuffling cold data around; too large and cold data pins young blocks for
 * years. tools/run_experiments.py sweeps it so the trade-off is visible rather
 * than asserted.
 * ========================================================================== */

#include "ftl_internal.h"

/* Scanning every block is O(NAND_NUM_BLOCKS); doing it on every host write
 * would dominate the simulation and would be absurd on real firmware. Erase
 * counts move slowly, so checking periodically loses nothing. */
#define WEAR_CHECK_INTERVAL 256u

static uint32_t s_ops_since_check;

/* A block is a candidate for allocation if it is free and usable. */
static bool is_allocatable(uint32_t pbn)
{
    return g_ftl.blk[pbn].state == BLK_FREE && !nand_block_is_bad(pbn);
}

/* ---------------------------------------------------------------------------
 * wear_pick_free_block -- the allocation policy.
 * Called from blockmgr_alloc() for EVERY block that ever gets used, which is
 * what makes dynamic levelling actually work rather than only covering the
 * host write path.
 * ------------------------------------------------------------------------- */
uint32_t wear_pick_free_block(void)
{
    uint32_t pbn, best = PBN_INVALID, best_ec = 0xFFFFFFFFu, ec;

    if (g_ftl.params.wear == FTL_WEAR_NONE) {
        /* The naive allocator: first free block found. It looks harmless and it
         * is genuinely awful -- low-numbered blocks are released and re-taken
         * over and over while high-numbered blocks stay untouched. It exists
         * here so the benchmark can show exactly how awful, instead of the
         * documentation just claiming it. */
        for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
            if (is_allocatable(pbn)) {
                return pbn;
            }
        }
        return PBN_INVALID;
    }

    /* DYNAMIC (and STATIC, which is dynamic plus the migration below):
     * cheapest block wins. Ties go to the lowest index, which keeps the
     * behaviour deterministic and therefore reproducible. */
    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (!is_allocatable(pbn)) {
            continue;
        }
        ec = nand_block_erase_count(pbn);
        if (ec < best_ec) {
            best_ec = ec;
            best    = pbn;
        }
    }
    return best;
}

/* ---------------------------------------------------------------------------
 * wear_relocate_block -- empty a block and return it to the free pool.
 * ------------------------------------------------------------------------- */
ftl_err_t wear_relocate_block(uint32_t pbn, uint64_t *pages_moved)
{
    uint64_t  before = g_ftl.stats.pgm_wear;
    ftl_err_t err;

    err = mapping_relocate_block(pbn, &g_ftl.stats.pgm_wear);
    if (err != FTL_OK) {
        return err;
    }
    if (pages_moved != NULL) {
        *pages_moved = g_ftl.stats.pgm_wear - before;
    }
    if (g_ftl.blk[pbn].valid_pages != 0u) {
        return FTL_ERR_CORRUPT;
    }
    return blockmgr_release(pbn);
}

/* ---------------------------------------------------------------------------
 * wear_maybe_level -- static wear levelling.
 * ------------------------------------------------------------------------- */
ftl_err_t wear_maybe_level(void)
{
    uint32_t pbn, ec;
    uint32_t min_ec = 0xFFFFFFFFu, max_ec = 0u;
    uint32_t coldest = PBN_INVALID, coldest_ec = 0xFFFFFFFFu;

    if (g_ftl.params.wear != FTL_WEAR_STATIC) {
        return FTL_OK;
    }
    if (++s_ops_since_check < WEAR_CHECK_INTERVAL) {
        return FTL_OK;
    }
    s_ops_since_check = 0u;

    /* Pass 1: how wide is the spread, and which occupied block is youngest? */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_bad(pbn) || g_ftl.blk[pbn].state == BLK_BAD) {
            continue;
        }
        ec = nand_block_erase_count(pbn);
        if (ec < min_ec) {
            min_ec = ec;
        }
        if (ec > max_ec) {
            max_ec = ec;
        }

        /* Only OCCUPIED blocks are worth migrating. A young block that is
         * already free will be picked up by dynamic levelling on the next
         * allocation with no data movement at all -- migrating it would be
         * pure waste. We are specifically hunting for young blocks that are
         * stuck holding cold data. */
        if (g_ftl.blk[pbn].state == BLK_DATA && g_ftl.blk[pbn].valid_pages > 0u &&
            pbn != g_ftl.active_pbn && ec < coldest_ec) {
            coldest_ec = ec;
            coldest    = pbn;
        }
    }

    if (min_ec == 0xFFFFFFFFu || (max_ec - min_ec) <= FTL_WEAR_GAP_THRESHOLD) {
        return FTL_OK;                      /* the spread is acceptable */
    }
    if (coldest == PBN_INVALID) {
        return FTL_OK;                      /* nothing worth moving */
    }
    /* If the youngest occupied block is not meaningfully younger than the most
     * worn one, moving it buys nothing and costs writes. */
    if ((max_ec - coldest_ec) <= FTL_WEAR_GAP_THRESHOLD) {
        return FTL_OK;
    }
    /* Need somewhere to put the survivors before we start. */
    if (g_ftl.free_blocks <= 1u) {
        return FTL_OK;
    }

    g_ftl.stats.wear_migrations++;
    return wear_relocate_block(coldest, NULL);
}
