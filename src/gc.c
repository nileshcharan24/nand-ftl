/* =============================================================================
 * gc.c -- Garbage collection: turning invalid pages back into free blocks.
 * =============================================================================
 *
 * THE PROBLEM
 * -----------
 * Out-of-place updates mean every overwrite leaves a dead page behind. Dead
 * pages cannot be reused individually -- the only way to reclaim them is to
 * erase the whole block they sit in, and a block almost always contains a mix
 * of dead and live pages. So reclaiming space means:
 *
 *      1. pick a victim block
 *      2. copy its still-live pages somewhere else
 *      3. erase it
 *
 * Step 2 is the whole story. Those copies are flash writes the host never asked
 * for -- they ARE write amplification. If a victim block is 90% dead, we copy
 * 10% of a block to free a whole block: excellent. If it is 90% live, we copy
 * 90% of a block to free 10% of one: ruinous. Victim selection is therefore not
 * a detail; it is the algorithm.
 *
 * WHEN TO COLLECT
 * ---------------
 * We collect when the free pool falls to FTL_GC_LOW_WATERMARK and stop when it
 * recovers to FTL_GC_HIGH_WATERMARK. The gap is hysteresis. Without it, the FTL
 * would reclaim exactly one block, immediately drop back below the trigger on
 * the next write, and collect again -- turning a background activity into a
 * per-write tax.
 *
 * Real drives also collect during host idle time, which hides the cost
 * completely until the workload gets busy enough that idle time disappears.
 * That "the drive was fast for ten minutes and then fell off a cliff" behaviour
 * is exactly this: idle-time GC running out of idle. This simulator collects
 * only on demand, which measures the worst case honestly.
 *
 * WHY THIS FILE ONLY MATTERS FOR PAGE MAPPING
 * -------------------------------------------
 * Under block mapping, a read-modify-write reclaims the old block on the spot;
 * under hybrid mapping, merges do. There is nothing left over to collect later.
 * That is not a gap in this implementation -- it is a property of those designs,
 * and it is why their write amplification shows up as pgm_rmw / pgm_merge in
 * the statistics instead of pgm_gc.
 * ========================================================================== */

#include "ftl_internal.h"

/* Read-disturb scanning walks every block, so we do not want it on the critical
 * path of every single write. Once every this-many host operations is often
 * enough given the disturb limit is tens of thousands of reads. */
#define GC_REFRESH_SCAN_INTERVAL 256u

static uint32_t s_ops_since_refresh_scan;

/* ---------------------------------------------------------------------------
 * Is this block a legal garbage-collection victim?
 * We never collect the block we are currently writing into (it is not full, and
 * collecting it would mean relocating pages into itself), nor free, bad or
 * metadata blocks.
 * ------------------------------------------------------------------------- */
static bool is_collectable(uint32_t pbn)
{
    if (pbn == g_ftl.active_pbn) {
        return false;
    }
    return g_ftl.blk[pbn].state == BLK_DATA;
}

/* ---------------------------------------------------------------------------
 * gc_select_victim -- the policy.
 * ------------------------------------------------------------------------- */
uint32_t gc_select_victim(void)
{
    uint32_t pbn, best = PBN_INVALID;
    uint32_t best_valid = 0xFFFFFFFFu;
    uint32_t best_seq   = 0xFFFFFFFFu;
    uint32_t candidates = 0u, pick;
    double   best_score = -1.0;

    switch (g_ftl.params.gc) {

    /* ---- GREEDY -----------------------------------------------------------
     * Fewest valid pages wins. This directly minimises the copy cost of THIS
     * collection, and for a uniform-random workload it is very close to
     * optimal. Its weakness is workloads with distinct hot and cold data:
     * greedy will cheerfully collect a cold block that is 40% valid when a hot
     * block at 45% valid would have decayed to 5% valid if left alone for a few
     * more seconds. It cannot see that, because it has no notion of time. */
    case FTL_GC_GREEDY:
        for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
            if (!is_collectable(pbn)) {
                continue;
            }
            if (g_ftl.blk[pbn].valid_pages < best_valid) {
                best_valid = g_ftl.blk[pbn].valid_pages;
                best       = pbn;
            }
        }
        break;

    /* ---- COST-BENEFIT -----------------------------------------------------
     *              (1 - u) * age
     *   score  =   -------------
     *                   2u
     *
     *   u    = fraction of the block still valid
     *   age  = how long since the block was last written to
     *
     * Numerator: (1 - u) is the space we get back; multiplying by age says a
     * block that has sat untouched for a long time is COLD, so the space we
     * free from it will stay free rather than being re-dirtied immediately.
     * Denominator: collecting costs u page reads plus u page writes, hence 2u.
     *
     * So we are maximising "space freed, weighted by how long it stays freed,
     * per unit of work done". This came out of the Sprite log-structured file
     * system cleaner and flash firmware adopted it nearly unchanged, because
     * the problem is identical: a log that must be cleaned, with hot and cold
     * regions mixed together.
     *
     * u == 0 is the free lunch: a block with nothing valid in it costs zero to
     * collect, so it always wins outright. */
    case FTL_GC_COST_BENEFIT:
        for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
            double u, age, score;

            if (!is_collectable(pbn)) {
                continue;
            }
            if (g_ftl.blk[pbn].valid_pages == 0u) {
                return pbn;          /* free lunch */
            }
            u     = (double)g_ftl.blk[pbn].valid_pages / (double)NAND_PAGES_PER_BLOCK;
            age   = (double)(g_ftl.seq - g_ftl.blk[pbn].last_write_seq);
            score = ((1.0 - u) * age) / (2.0 * u);

            if (score > best_score) {
                best_score = score;
                best       = pbn;
            }
        }
        break;

    /* ---- FIFO -------------------------------------------------------------
     * Collect whatever was written longest ago. Costs nothing to evaluate and
     * is what a pure log-structured design does by default. It ignores validity
     * entirely, so it will happily collect a block that is 100% live. */
    case FTL_GC_FIFO:
        for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
            if (!is_collectable(pbn)) {
                continue;
            }
            if (g_ftl.blk[pbn].last_write_seq < best_seq) {
                best_seq = g_ftl.blk[pbn].last_write_seq;
                best     = pbn;
            }
        }
        break;

    /* ---- RANDOM -----------------------------------------------------------
     * The control group. Reservoir-free two-pass selection: count candidates,
     * then pick the n-th. Included because a policy that cannot beat random is
     * not earning its complexity, and because having the baseline in the
     * results table is what makes the other numbers mean anything. */
    case FTL_GC_RANDOM:
    default:
        for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
            if (is_collectable(pbn)) {
                candidates++;
            }
        }
        if (candidates == 0u) {
            return PBN_INVALID;
        }
        pick = (uint32_t)(ftl_rng_next() % candidates);
        for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
            if (is_collectable(pbn)) {
                if (pick == 0u) {
                    return pbn;
                }
                pick--;
            }
        }
        break;
    }

    return best;
}

/* ---------------------------------------------------------------------------
 * Collect exactly one block.
 * ------------------------------------------------------------------------- */
static ftl_err_t gc_collect_one(void)
{
    uint32_t  victim;
    ftl_err_t err;

    victim = gc_select_victim();
    if (victim == PBN_INVALID) {
        return FTL_ERR_NO_SPACE;
    }

    g_ftl.stats.gc_victims++;

    /* Relocate the survivors. Under page mapping they go to the write
     * frontier, which means GC's own writes share the open block with host
     * writes. Real firmware usually gives GC a SEPARATE open block, so that
     * relocated (by definition, cold-ish) data does not get interleaved with
     * fresh host data that is about to be overwritten -- keeping them apart
     * makes future blocks more uniformly hot or cold and cuts future GC cost.
     * That separation is the highest-value single improvement left on the
     * table here, and it is listed as such in DOCS/INTERVIEW_QA.md. */
    err = mapping_relocate_block(victim, &g_ftl.stats.pgm_gc);
    if (err != FTL_OK) {
        return err;
    }

    if (g_ftl.blk[victim].valid_pages != 0u) {
        return FTL_ERR_CORRUPT;      /* relocation lied; never erase in this case */
    }
    return blockmgr_release(victim);
}

ftl_err_t gc_force_once(void)
{
    g_ftl.stats.gc_invocations++;
    return gc_collect_one();
}

/* ---------------------------------------------------------------------------
 * Read-disturb refresh.
 * ---------------------------------------------------------------------------
 * Reading a page applies a voltage to the whole word line; the cells in the
 * OTHER pages of that block get a small unwanted nudge each time. Read a block
 * enough times without erasing it and neighbouring pages start returning bit
 * errors that ECC eventually cannot correct.
 *
 * The fix is unglamorous: rewrite the block somewhere else, which resets its
 * disturb budget. Note the consequence -- a purely READ-ONLY workload can still
 * cause flash writes and erases. That surprises people, and it is a good thing
 * to be able to explain.
 * ------------------------------------------------------------------------- */
static ftl_err_t refresh_scan(void)
{
    uint32_t  pbn;
    ftl_err_t err;

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (g_ftl.blk[pbn].state != BLK_DATA && g_ftl.blk[pbn].state != BLK_LOG) {
            continue;
        }
        if (!nand_block_read_disturb_exceeded(pbn)) {
            continue;
        }

        g_ftl.stats.refresh_events++;
        err = mapping_relocate_block(pbn, &g_ftl.stats.pgm_refresh);
        if (err != FTL_OK) {
            return err;
        }
        if (g_ftl.blk[pbn].valid_pages == 0u) {
            (void)blockmgr_release(pbn);   /* the erase is what resets the count */
        }
    }
    return FTL_OK;
}

/* ---------------------------------------------------------------------------
 * gc_maybe_run -- called before every host write.
 * ------------------------------------------------------------------------- */
ftl_err_t gc_maybe_run(void)
{
    uint32_t  before;
    ftl_err_t err;

    if (g_ftl.params.read_disturb_refresh) {
        if (++s_ops_since_refresh_scan >= GC_REFRESH_SCAN_INTERVAL) {
            s_ops_since_refresh_scan = 0u;
            (void)refresh_scan();
        }
    }

    /* Block and hybrid mapping reclaim space synchronously; there is no
     * deferred garbage for this collector to find. */
    if (g_ftl.params.mapping != FTL_MAP_PAGE) {
        return FTL_OK;
    }
    if (g_ftl.free_blocks > FTL_GC_LOW_WATERMARK) {
        return FTL_OK;
    }

    g_ftl.stats.gc_invocations++;

    while (g_ftl.free_blocks < FTL_GC_HIGH_WATERMARK) {
        before = g_ftl.free_blocks;

        err = gc_collect_one();
        if (err != FTL_OK) {
            /* No victim, or nowhere to put the survivors. Not necessarily
             * fatal: if we freed anything at all this round the write can still
             * proceed. Reporting NO_SPACE only when we are genuinely stuck is
             * the difference between a drive that degrades and one that dies. */
            return (g_ftl.free_blocks > 0u) ? FTL_OK : err;
        }

        if (g_ftl.free_blocks <= before) {
            /* We collected a block and the pool did not grow: every survivor we
             * relocated consumed exactly as much space as we freed, which means
             * the device is genuinely full of live data. Stop, rather than spin
             * burning erase cycles for nothing.
             *
             * This is the state that over-provisioning exists to prevent, and
             * hitting it in a benchmark is the signal that OP is set too low. */
            break;
        }
    }
    return FTL_OK;
}
