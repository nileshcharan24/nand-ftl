/* =============================================================================
 * badblock.c -- Retiring blocks that have failed, without losing user data.
 * =============================================================================
 *
 * WHY BAD BLOCK MANAGEMENT IS NOT OPTIONAL
 * ----------------------------------------
 * Two separate facts about NAND force this module to exist.
 *
 * 1. FACTORY BAD BLOCKS. Every die ships with some blocks already unusable.
 *    Guaranteeing a perfect die would mean throwing away most of the wafer, so
 *    manufacturers instead guarantee "no more than N% bad" and mark the bad ones
 *    before shipping. Firmware must find those marks on first power-on and never
 *    touch them again -- and crucially must never ERASE them, because the mark
 *    itself lives in the block's spare area and erasing destroys it. That is
 *    handled at init in blockmgr.c.
 *
 * 2. GROWN BAD BLOCKS. Blocks also fail during the drive's life. A program or
 *    erase that does not complete means the block can no longer be trusted, and
 *    the drive has to keep working anyway.
 *
 * THE PART THAT IS EASY TO GET WRONG
 * ----------------------------------
 * When a program fails partway through a block, the pages ALREADY written in
 * that block usually still read back fine. Firmware must read them out and
 * relocate them BEFORE writing the bad-block marker -- because once the block is
 * retired, nothing will ever read it again. Get the order wrong and every retire
 * silently eats up to a block's worth of user data.
 *
 * That is why nand.c does not retire blocks by itself (see the comment on
 * nand_mark_bad): the decision, and the ordering, belong to firmware.
 * ========================================================================== */

#include "ftl_internal.h"

/* Retiring a block can require writing its contents elsewhere, and that write
 * can itself fail, which would recurse back into here. The pool shrinks on
 * every retire so the recursion is finite, but "finite" is not "shallow": a
 * cascade during a burst of injected failures could otherwise nest deeply.
 * The guard turns that into a bounded, observable condition. */
static int s_retire_depth;
#define BADBLOCK_MAX_RETIRE_DEPTH 3

ftl_err_t badblock_retire(uint32_t pbn, const char *reason)
{
    ftl_err_t err = FTL_OK;

    (void)reason;   /* a product would log this; the tests assert on counters */

    if (pbn >= NAND_NUM_BLOCKS) {
        return FTL_ERR_RANGE;
    }
    if (g_ftl.blk[pbn].state == BLK_BAD) {
        return FTL_OK;                 /* already retired; nothing to do */
    }
    if (s_retire_depth >= BADBLOCK_MAX_RETIRE_DEPTH) {
        /* Too many failures cascading at once. Mark the block bad and give up
         * on rescuing it rather than recursing further -- losing one block's
         * data is bad, but an unbounded cascade during a write storm is worse. */
        g_ftl.stats.pages_lost += g_ftl.blk[pbn].valid_pages;
        goto mark_bad;
    }

    s_retire_depth++;

    /* STEP 1: rescue whatever is still readable. mapping_relocate_block knows
     * how to do this for each mapping scheme -- appending to the write frontier
     * under page mapping, or copying into a fresh block at identical offsets
     * under block/hybrid mapping where the offset is part of the address. */
    err = mapping_relocate_block(pbn, &g_ftl.stats.pgm_recover);
    if (err != FTL_OK) {
        /* We could not find anywhere to put the data. Count what we are about
         * to lose instead of pretending it did not happen -- an honest counter
         * is worth more than an optimistic one. */
        g_ftl.stats.pages_lost += g_ftl.blk[pbn].valid_pages;
    }

    s_retire_depth--;

mark_bad:
    /* STEP 2: forget any remaining validity for this block. Whatever was not
     * rescued is gone; leaving stale valid bits set would corrupt every later
     * invariant check and every GC decision. */
    {
        uint32_t i;
        for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
            mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(pbn, i));
        }
    }

    /* STEP 3: if this block was in the free pool, it is not any more. */
    if (g_ftl.blk[pbn].state == BLK_FREE && g_ftl.free_blocks > 0u) {
        g_ftl.free_blocks--;
    }

    /* STEP 4: if it was the write frontier, drop it so the next write opens a
     * fresh block instead of trying to program into a corpse. */
    if (g_ftl.active_pbn == pbn) {
        g_ftl.active_pbn = PBN_INVALID;
        g_ftl.active_off = 0u;
    }

    /* STEP 5: if it was serving as a hybrid log block, release the slot. */
    {
        int i;
        for (i = 0; i < (int)FTL_LOG_BLOCK_POOL; i++) {
            if (g_ftl.log[i].pbn == pbn) {
                g_ftl.log[i].pbn = PBN_INVALID;
                g_ftl.log[i].lbn = PBN_INVALID;
                g_ftl.log[i].used = 0u;
            }
        }
    }

    /* STEP 6: and only NOW write the durable marker. */
    g_ftl.blk[pbn].state       = BLK_BAD;
    g_ftl.blk[pbn].valid_pages = 0u;
    nand_mark_bad(pbn);
    g_ftl.stats.blocks_retired++;

    return err;
}

uint32_t badblock_count(void)
{
    uint32_t pbn, n = 0u;

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_bad(pbn)) {
            n++;
        }
    }
    return n;
}
