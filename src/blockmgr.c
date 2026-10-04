/* =============================================================================
 * blockmgr.c -- The free-block pool, block allocation, and the write frontier.
 * =============================================================================
 *
 * Everything in the FTL that needs somewhere to put a page comes through here.
 * There are exactly three consumers:
 *
 *   - the host write path        (mapping.c)
 *   - garbage collection         (gc.c)
 *   - wear levelling and refresh (wear.c)
 *
 * Funnelling all of them through one allocator is deliberate. It means the
 * wear-levelling policy is applied to every block that ever gets used, not just
 * to host writes -- a mistake that is easy to make and that shows up months
 * later as an erase-count distribution with a long ugly tail.
 *
 * ALLOCATION POLICY
 * -----------------
 * Implemented as a linear scan of all NAND_NUM_BLOCKS blocks. That is O(N) per
 * allocation, which at 128 blocks is nothing, and at 256k blocks would be
 * unacceptable. Real firmware keeps free blocks bucketed by erase count (an
 * array of small lists indexed by erase_count >> k), giving O(1) "cheapest free
 * block". The honest reason it is a linear scan here is that the bucketed
 * version obscures the policy without changing any measured result -- and this
 * limitation is named explicitly in DOCS/INTERVIEW_QA.md rather than hidden.
 *
 * ERASE TIMING
 * ------------
 * Blocks are erased when they are RELEASED, not when they are allocated. The
 * alternative -- keeping a pool of dirty blocks and erasing lazily on demand --
 * is what most real firmware does, because a 3 ms erase can then be hidden in
 * host idle time instead of appearing in the middle of a write burst. Erasing
 * eagerly here buys one strong invariant: a block in state BLK_FREE is always
 * genuinely erased, so ftl_check_invariants() can assert it, and no code path
 * can ever program into a block that only *looks* free.
 * ========================================================================== */

#include <string.h>

#include "ftl_internal.h"

/* ---------------------------------------------------------------------------
 * blockmgr_init -- classify every physical block at format time.
 * ------------------------------------------------------------------------- */
void blockmgr_init(void)
{
    uint32_t pbn;

    g_ftl.free_blocks = 0u;

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {
        g_ftl.blk[pbn].valid_pages    = 0u;
        g_ftl.blk[pbn].owner_lbn      = PBN_INVALID;
        g_ftl.blk[pbn].last_write_seq = 0u;

        if (nand_block_is_bad(pbn)) {
            /* Factory-bad blocks are discovered, not decided. Firmware must
             * scan for them at first power-on and never erase them -- the
             * factory mark itself lives in the spare area of the block's first
             * page, and erasing the block would destroy the only record that
             * the block is unusable. */
            g_ftl.blk[pbn].state = BLK_BAD;
        } else if (pbn < FTL_META_BLOCKS) {
            /* The metadata region sits at a fixed low address precisely because
             * at mount time we have no mapping table with which to find it. */
            g_ftl.blk[pbn].state = BLK_META;
        } else {
            g_ftl.blk[pbn].state = BLK_FREE;
            g_ftl.free_blocks++;
        }
    }

    g_ftl.active_pbn = PBN_INVALID;
    g_ftl.active_off = 0u;
}

/* ---------------------------------------------------------------------------
 * blockmgr_alloc -- take a block out of the free pool.
 *
 * Returns PBN_INVALID when the pool is empty. Callers must handle that: on a
 * real drive, running out of blocks is not a crash, it is the moment the drive
 * transitions to read-only and reports it to the host. Never assert on it.
 * ------------------------------------------------------------------------- */
uint32_t blockmgr_alloc(void)
{
    uint32_t pbn = wear_pick_free_block();

    if (pbn == PBN_INVALID) {
        return PBN_INVALID;
    }

    /* Paranoia that has caught real bugs: a block we are about to program into
     * must have its write pointer at 0. If it does not, something released it
     * without erasing it, and we are one program away from silent corruption. */
    if (nand_block_write_pointer(pbn) != 0u) {
        /* Recover rather than corrupt: try to erase it now. */
        if (nand_erase_block(pbn) != NAND_OK) {
            (void)badblock_retire(pbn, "erase failed during allocation");
            return blockmgr_alloc();   /* bounded: the pool only shrinks */
        }
    }

    g_ftl.blk[pbn].state          = BLK_ACTIVE;
    g_ftl.blk[pbn].valid_pages    = 0u;
    g_ftl.blk[pbn].owner_lbn      = PBN_INVALID;
    g_ftl.blk[pbn].last_write_seq = g_ftl.seq;
    g_ftl.free_blocks--;

    return pbn;
}

/* ---------------------------------------------------------------------------
 * blockmgr_release -- erase a block and put it back in the free pool.
 *
 * PRECONDITION, and it is load-bearing: the block must hold no valid pages.
 * Every caller either verified that or moved the valid pages out first. If this
 * ever fires, someone lost user data, and it is far better to find out here
 * than three thousand operations later when a read returns the wrong page.
 * ------------------------------------------------------------------------- */
ftl_err_t blockmgr_release(uint32_t pbn)
{
    nand_err_t nerr;
    uint32_t   base, i;

    if (pbn >= NAND_NUM_BLOCKS) {
        return FTL_ERR_RANGE;
    }
    if (g_ftl.blk[pbn].state == BLK_BAD) {
        return FTL_ERR_MEDIA;
    }
    if (g_ftl.blk[pbn].valid_pages != 0u) {
        /* Refuse loudly. Returning an error here rather than erasing anyway is
         * the difference between a failing test and a data-loss bug. */
        return FTL_ERR_CORRUPT;
    }

    nerr = nand_erase_block(pbn);
    if (nerr != NAND_OK) {
        /* An erase that fails means the block is finished. Because we only ever
         * release blocks with zero valid pages, no user data is lost -- which is
         * exactly why that precondition is worth enforcing. */
        (void)badblock_retire(pbn, nand_strerror(nerr));
        return FTL_ERR_MEDIA;
    }

    g_ftl.stats.erases++;

    /* Every page in the block is now erased, so nothing in it can be valid. */
    base = pbn * NAND_PAGES_PER_BLOCK;
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        BITMAP_CLR(g_ftl.page_valid, base + i);
    }

    g_ftl.blk[pbn].state       = BLK_FREE;
    g_ftl.blk[pbn].valid_pages = 0u;
    g_ftl.blk[pbn].owner_lbn   = PBN_INVALID;
    g_ftl.free_blocks++;

    return FTL_OK;
}

/* ---------------------------------------------------------------------------
 * The write frontier.
 * ---------------------------------------------------------------------------
 * Page-mapped writing is append-only: there is one open block, and every page
 * -- host data, GC copies, wear migrations -- lands at its write pointer. When
 * it fills, we close it and open another.
 *
 * Why a single frontier and not one per data "temperature"? Because separating
 * hot and cold data into different open blocks is a real and effective
 * optimisation (it makes blocks become entirely-invalid or entirely-valid
 * rather than a mix, which is the single biggest lever on GC cost after
 * over-provisioning). It is left out on purpose so the baseline is honest, and
 * it is written up in DOCS/INTERVIEW_QA.md as the first thing to add next.
 * ------------------------------------------------------------------------- */
void blockmgr_open_active_block(void)
{
    if (g_ftl.active_pbn != PBN_INVALID &&
        g_ftl.active_off < NAND_PAGES_PER_BLOCK) {
        return;   /* still room in the current one */
    }

    /* Close the full one. It is now an ordinary data block and becomes a
     * candidate for garbage collection. */
    if (g_ftl.active_pbn != PBN_INVALID) {
        g_ftl.blk[g_ftl.active_pbn].state = BLK_DATA;
    }

    g_ftl.active_pbn = blockmgr_alloc();
    g_ftl.active_off = 0u;
}

uint32_t blockmgr_next_free_ppn(void)
{
    blockmgr_open_active_block();

    if (g_ftl.active_pbn == PBN_INVALID) {
        return PPN_INVALID;
    }
    return BLOCK_OFFSET_TO_PPN(g_ftl.active_pbn, g_ftl.active_off);
}
