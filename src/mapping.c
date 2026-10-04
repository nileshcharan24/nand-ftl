/* =============================================================================
 * mapping.c -- Logical-to-physical address translation: all three schemes.
 * =============================================================================
 *
 * This is the heart of the FTL. Everything else -- garbage collection, wear
 * levelling, recovery -- exists to support what happens in this file.
 *
 * THE ONE IDEA
 * ------------
 * The host says "overwrite logical page 42". Flash cannot overwrite. So the FTL
 * writes the new data to a DIFFERENT, erased physical page, points logical
 * page 42 at the new location, and marks the old physical page invalid. This is
 * called an OUT-OF-PLACE UPDATE, and every consequence in this project follows
 * from it:
 *
 *   - you need a table to remember where each logical page went   -> mapping
 *   - invalid pages accumulate and must eventually be reclaimed   -> GC
 *   - reclaiming means erasing, which wears the block             -> wear levelling
 *   - the table must survive a power cut                          -> journaling
 *   - one host write causes more than one flash write             -> write amplification
 *
 * The three schemes below differ only in the GRANULARITY of that table, and
 * every difference in their behaviour comes from that one choice.
 *
 * BUFFER DISCIPLINE
 * -----------------
 * The only scratch memory in this file is a handful of page-sized static
 * buffers. Not one code path -- not the block-mapped read-modify-write, not a
 * full merge -- ever needs to hold a whole block in RAM. That is deliberate: a
 * controller with 256 KiB of SRAM cannot buffer a 4 MiB block, so any algorithm
 * that needs to is unimplementable on real hardware regardless of how well it
 * benchmarks in a simulator.
 * ========================================================================== */

#include <string.h>

#include "ftl_internal.h"

/* ---------------------------------------------------------------------------
 * SCRATCH BUFFERS -- and a re-entrancy hazard worth understanding.
 * ---------------------------------------------------------------------------
 * The obvious implementation is one static page buffer shared by every copy
 * path. It is also wrong, and the way it is wrong is instructive.
 *
 * Consider relocating a block: read page P into the buffer, then program it
 * somewhere else. If that program FAILS, the failure handler retires the
 * destination block -- which means reading ITS pages into a buffer and moving
 * them. With a single shared buffer, the nested relocation overwrites the page
 * we were in the middle of moving, and when the outer call resumes it programs
 * the wrong data under the right logical address. Silent corruption, only under
 * media failure, therefore essentially impossible to find without fault
 * injection.
 *
 * The fix is a small pool indexed by nesting depth. The depth is bounded by the
 * bad-block manager's own recursion limit, so the pool is a fixed size known at
 * compile time -- no dynamic allocation, and the bound is checkable.
 *
 * Cost: 5 x 4 KiB = 20 KiB. Still nothing holds a whole block.
 * ------------------------------------------------------------------------- */
#define MAPPING_BUF_DEPTH 5
static uint8_t s_buf_pool[MAPPING_BUF_DEPTH][NAND_PAGE_SIZE_BYTES];
static int     s_buf_depth;

#define COPY_BUF() (s_buf_pool[s_buf_depth])

/* A dedicated all-zero buffer for padding pages, so that writing padding can
 * never disturb a copy in progress. */
static uint8_t s_pad_buf[NAND_PAGE_SIZE_BYTES];

/* How many times a single logical operation may be retried after the media
 * fails a program. Each retry costs a retired block, so the pool shrinks
 * monotonically and this cannot loop forever. */
#define MAPPING_MAX_RETRY 4

/* ===========================================================================
 * SECTION 1 -- Validity bookkeeping
 * ---------------------------------------------------------------------------
 * "Valid" means: this physical page holds the current contents of some logical
 * page, i.e. the mapping table points at it. It is an FTL-level fact; the die
 * has no idea (see the layering note at the top of nand.h).
 *
 * We track it two ways, on purpose:
 *
 *   - a bitmap over physical pages, and a per-block valid count.
 *     O(1) to update, O(1) to query, and it is what garbage collection uses to
 *     pick a victim without touching the media at all.
 *
 *   - the LPN written into each page's spare area, which lets anyone
 *     RECONSTRUCT validity by scanning: page P is valid if and only if
 *     lookup(spare(P).lpn) == P.
 *
 * The first is the fast path; the second is the truth. ftl_check_invariants()
 * cross-checks one against the other, which is how a bug in the bookkeeping
 * gets caught in a unit test instead of in the field.
 * ========================================================================= */

void mapping_validate_ppn(uint32_t ppn)
{
    uint32_t pbn = PPN_TO_BLOCK(ppn);

    if (!BITMAP_GET(g_ftl.page_valid, ppn)) {
        BITMAP_SET(g_ftl.page_valid, ppn);
        g_ftl.blk[pbn].valid_pages++;
    }
}

void mapping_invalidate_ppn(uint32_t ppn)
{
    uint32_t pbn;

    if (ppn == PPN_INVALID || ppn >= NAND_TOTAL_PAGES) {
        return;
    }
    pbn = PPN_TO_BLOCK(ppn);
    if (BITMAP_GET(g_ftl.page_valid, ppn)) {
        BITMAP_CLR(g_ftl.page_valid, ppn);
        g_ftl.blk[pbn].valid_pages--;
    }
}

/* ===========================================================================
 * SECTION 2 -- Programming a page
 * ========================================================================= */

/* Program into a specific block at ITS current write pointer.
 * Returns the resulting PPN. Does not update any mapping or validity state --
 * the caller owns that, because only the caller knows what the page means.
 *
 * Note the caller cannot choose the offset: the offset is wherever the block's
 * write pointer happens to be. That is not an API weakness, it is the hardware
 * rule from nand.h (sequential programming within a block) surfacing in the
 * interface, which is exactly where it should surface. */
static ftl_err_t program_into_block(uint32_t pbn, uint32_t lpn, const void *buf,
                                    uint32_t tag, uint32_t *ppn_out)
{
    nand_spare_t spare;
    nand_err_t   nerr;
    uint32_t     off, ppn;

    if (pbn == PBN_INVALID || pbn >= NAND_NUM_BLOCKS) {
        return FTL_ERR_NO_SPACE;
    }
    off = nand_block_write_pointer(pbn);
    if (off >= NAND_PAGES_PER_BLOCK) {
        return FTL_ERR_NO_SPACE;
    }
    ppn = BLOCK_OFFSET_TO_PPN(pbn, off);

    /* The spare area is what makes the media self-describing. Every page
     * carries the logical page it holds and a global sequence number, so a
     * recovery scan can rebuild the whole mapping table from the media alone.
     * See journal.c. */
    spare.lpn = lpn;
    spare.seq = ++g_ftl.seq;
    spare.crc = 0u;              /* the die fills this in */
    spare.tag = tag;

    nerr = nand_program_page(ppn, buf, &spare);
    if (nerr != NAND_OK) {
        return FTL_ERR_MEDIA;
    }

    g_ftl.blk[pbn].last_write_seq = g_ftl.seq;
    if (ppn_out != NULL) {
        *ppn_out = ppn;
    }
    return FTL_OK;
}

/* Program at the global write frontier, retrying past media failures.
 *
 * This is the page-mapped write path, and it is also used by garbage
 * collection, wear levelling and refresh, so that all of them get identical
 * failure handling for free. If a program fails, the block that failed is
 * retired (which relocates whatever else was living in it) and we try again at
 * a fresh frontier. */
ftl_err_t mapping_program_at_frontier(uint32_t lpn, const void *buf,
                                      uint32_t tag, uint32_t *ppn_out)
{
    int       attempt;
    uint32_t  ppn;
    ftl_err_t err;

    for (attempt = 0; attempt < MAPPING_MAX_RETRY; attempt++) {
        ppn = blockmgr_next_free_ppn();
        if (ppn == PPN_INVALID) {
            return FTL_ERR_NO_SPACE;
        }

        err = program_into_block(PPN_TO_BLOCK(ppn), lpn, buf, tag, &ppn);
        if (err == FTL_OK) {
            g_ftl.active_off++;
            if (ppn_out != NULL) {
                *ppn_out = ppn;
            }
            return FTL_OK;
        }
        if (err != FTL_ERR_MEDIA) {
            return err;
        }

        /* The die refused. Retire the block -- which reads out and relocates
         * every page still living in it -- and go round again. */
        g_ftl.stats.program_retries++;
        (void)badblock_retire(PPN_TO_BLOCK(ppn), "program failure");
    }
    return FTL_ERR_MEDIA;
}

/* ===========================================================================
 * SECTION 3 -- Reading a page from a known PPN, with the identity check.
 * ---------------------------------------------------------------------------
 * We always verify that the page we read claims to be the LPN we asked for.
 * That check costs nothing (the LPN is already in the spare area we just read)
 * and it turns an entire class of mapping bug -- the worst class, silently
 * returning the wrong user's data -- into an immediate, loud failure.
 * ========================================================================= */
static ftl_err_t read_verified(uint32_t ppn, uint32_t expect_lpn, void *buf)
{
    nand_spare_t spare;
    nand_err_t   nerr;

    nerr = nand_read_page(ppn, buf, &spare);
    if (nerr != NAND_OK) {
        return FTL_ERR_MEDIA;
    }
    if (expect_lpn != LPN_INVALID && spare.lpn != expect_lpn) {
        return FTL_ERR_CORRUPT;
    }
    return FTL_OK;
}

/* ===========================================================================
 * SECTION 4 -- PAGE MAPPING
 * ---------------------------------------------------------------------------
 * One table entry per logical page. Any logical page can live in any physical
 * page, so a write is always a pure append to the frontier: no read-modify-
 * write, no merge, no padding. Random writes cost exactly the same as
 * sequential ones, which is the whole reason modern SSDs do it this way.
 *
 * The bill is RAM: FTL_LOGICAL_PAGES * 4 bytes. That is ~1 MB of table per
 * 1 GB of flash at a 4 KiB page size, which is why full page mapping only
 * became normal once putting a DRAM die next to the controller got cheap.
 * ========================================================================= */

static ftl_err_t page_write(uint32_t lpn, const void *buf)
{
    uint32_t  new_ppn, old_ppn;
    ftl_err_t err;

    err = mapping_program_at_frontier(lpn, buf, NAND_TAG_USER_DATA, &new_ppn);
    if (err != FTL_OK) {
        return err;
    }
    g_ftl.stats.pgm_host++;

    /* Order matters. The new page is on the media BEFORE we drop the old one.
     * If power is lost between the two, recovery finds two pages claiming this
     * LPN and picks the one with the higher sequence number -- which is the new
     * one. Do it the other way round and there is a window in which the LPN has
     * no valid copy at all. */
    old_ppn = g_ftl.l2p.page[lpn];
    mapping_invalidate_ppn(old_ppn);

    g_ftl.l2p.page[lpn] = new_ppn;
    mapping_validate_ppn(new_ppn);

    return FTL_OK;
}

static ftl_err_t page_read(uint32_t lpn, void *buf)
{
    uint32_t ppn = g_ftl.l2p.page[lpn];

    if (ppn == PPN_INVALID) {
        /* Never written, or TRIMmed. A real drive returns zeros here rather
         * than an error; returning a distinct code makes the tests sharper. */
        return FTL_ERR_UNWRITTEN;
    }
    return read_verified(ppn, lpn, buf);
}

static ftl_err_t page_trim(uint32_t lpn)
{
    mapping_invalidate_ppn(g_ftl.l2p.page[lpn]);
    g_ftl.l2p.page[lpn] = PPN_INVALID;
    return FTL_OK;
}

/* ===========================================================================
 * SECTION 5 -- BLOCK MAPPING
 * ---------------------------------------------------------------------------
 * One table entry per logical BLOCK. Inside a block, a logical page's offset is
 * FIXED: logical page (lbn * PPB + k) always lives at physical offset k.
 *
 * That constraint is what makes the table 64x smaller, and it is also what
 * makes random writes catastrophic. Three cases arise on a write to offset k of
 * a block whose write pointer is at w:
 *
 *   k == w   Append. One page program. Perfect -- this is the sequential case,
 *            and it is why block mapping is not a stupid design: a workload
 *            that writes whole files start-to-finish never leaves this path.
 *
 *   k >  w   Pad offsets w..k-1 with filler pages, then append. The filler is
 *            wasted flash, but it is cheaper than the alternative.
 *            (Why pad at all? Because the hardware forbids programming page k
 *            while page w is still erased. See nand.h rule 2.)
 *
 *   k <  w   Offset k already holds data and cannot be overwritten. The only
 *            way forward is to allocate a fresh block and rebuild the entire
 *            logical block into it -- READ-MODIFY-WRITE. One 4 KiB host write
 *            turns into up to 64 page programs plus an erase.
 *
 * A uniform-random write workload lands in case 3 almost every time, so write
 * amplification tends towards PAGES_PER_BLOCK. The benchmark measures exactly
 * that, and the measured number is in DOCS/RESULTS.md.
 * ========================================================================= */

/* Highest offset in `pbn` that holds real (non-padding) data, or -1. */
static int block_last_data_offset(uint32_t pbn)
{
    int      i;
    uint32_t wp;

    if (pbn == PBN_INVALID) {
        return -1;
    }
    wp = nand_block_write_pointer(pbn);
    for (i = (int)wp - 1; i >= 0; i--) {
        if (BITMAP_GET(g_ftl.page_valid, BLOCK_OFFSET_TO_PPN(pbn, (uint32_t)i))) {
            return i;
        }
    }
    return -1;
}

/* Program a padding page: no logical owner, never valid, exists only to move
 * the write pointer past an offset the hardware will not let us skip. */
static ftl_err_t program_pad(uint32_t pbn)
{
    ftl_err_t err;

    err = program_into_block(pbn, LPN_INVALID, s_pad_buf, NAND_TAG_PADDING, NULL);
    if (err == FTL_OK) {
        g_ftl.stats.pgm_pad++;
    }
    return err;
}

/* Rebuild logical block `lbn` into a fresh physical block, substituting `buf`
 * at offset `new_off`. This is the expensive path described above. */
static ftl_err_t block_rmw(uint32_t lbn, uint32_t old_pbn, uint32_t new_off,
                           const void *buf)
{
    uint32_t  new_pbn, i, src_ppn, ppn;
    int       last;
    uint32_t  highest;
    ftl_err_t err;

    new_pbn = blockmgr_alloc();
    if (new_pbn == PBN_INVALID) {
        return FTL_ERR_NO_SPACE;
    }
    g_ftl.blk[new_pbn].state     = BLK_DATA;
    g_ftl.blk[new_pbn].owner_lbn = lbn;

    /* Only rebuild up to the highest offset that actually has data. Offsets
     * above that stay erased and can still be appended to later, which keeps
     * the sequential-fill fast path alive after an RMW. */
    last    = block_last_data_offset(old_pbn);
    highest = new_off;
    if (last >= 0 && (uint32_t)last > highest) {
        highest = (uint32_t)last;
    }

    for (i = 0u; i <= highest; i++) {
        if (i == new_off) {
            err = program_into_block(new_pbn, lbn * NAND_PAGES_PER_BLOCK + i,
                                     buf, NAND_TAG_USER_DATA, &ppn);
            if (err != FTL_OK) {
                goto fail;
            }
            g_ftl.stats.pgm_host++;
            mapping_validate_ppn(ppn);
            continue;
        }

        src_ppn = (old_pbn == PBN_INVALID) ? PPN_INVALID
                                           : BLOCK_OFFSET_TO_PPN(old_pbn, i);

        if (src_ppn != PPN_INVALID && BITMAP_GET(g_ftl.page_valid, src_ppn)) {
            if (read_verified(src_ppn, LPN_INVALID, COPY_BUF()) != FTL_OK) {
                /* The source page is unreadable. We cannot invent it, so the
                 * logical page is lost -- but the rest of the block is not, and
                 * that distinction is why we keep going instead of aborting. */
                g_ftl.stats.pages_lost++;
                if (program_pad(new_pbn) != FTL_OK) {
                    goto fail;
                }
                continue;
            }
            err = program_into_block(new_pbn, lbn * NAND_PAGES_PER_BLOCK + i,
                                     COPY_BUF(), NAND_TAG_USER_DATA, &ppn);
            if (err != FTL_OK) {
                goto fail;
            }
            g_ftl.stats.pgm_rmw++;
            mapping_validate_ppn(ppn);
        } else {
            if (program_pad(new_pbn) != FTL_OK) {
                goto fail;
            }
        }
    }

    /* Swap the block in, then dismantle the old one. Doing it in this order
     * means a power cut leaves the OLD block still referenced and intact; the
     * new block is orphaned, which recovery cleans up. Losing a block is
     * annoying. Losing user data is not acceptable. */
    g_ftl.l2p.block[lbn] = new_pbn;

    if (old_pbn != PBN_INVALID) {
        for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
            mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(old_pbn, i));
        }
        (void)blockmgr_release(old_pbn);
    }
    return FTL_OK;

fail:
    /* Unwind. The new block never became reachable -- the mapping still points
     * at the old one, which is untouched -- so nothing is lost by discarding it.
     *
     * We RETIRE rather than release it. A block that failed a program has told
     * us something about itself, and putting it back in the free pool would
     * mean the very next retry picks it again and fails again, forever. This is
     * the difference between a drive that degrades gracefully under media
     * failure and one that livelocks.
     *
     * Invalidating its pages first is what makes the retire cheap: the
     * bad-block manager then has nothing to rescue, because every page in this
     * block is a duplicate of one still sitting in the old block. */
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(new_pbn, i));
    }
    (void)badblock_retire(new_pbn, "program failure during read-modify-write");
    return FTL_ERR_MEDIA;
}

static ftl_err_t block_write(uint32_t lpn, const void *buf)
{
    uint32_t  lbn = LPN_TO_LBN(lpn);
    uint32_t  off = LPN_TO_LOFF(lpn);
    uint32_t  pbn = g_ftl.l2p.block[lbn];
    uint32_t  wp, ppn;
    ftl_err_t err;

    if (pbn == PBN_INVALID) {
        pbn = blockmgr_alloc();
        if (pbn == PBN_INVALID) {
            return FTL_ERR_NO_SPACE;
        }
        g_ftl.blk[pbn].state     = BLK_DATA;
        g_ftl.blk[pbn].owner_lbn = lbn;
        g_ftl.l2p.block[lbn]     = pbn;
    }

    wp = nand_block_write_pointer(pbn);

    if (off < wp) {
        /* Case 3: the offset is already programmed. Read-modify-write. */
        return block_rmw(lbn, pbn, off, buf);
    }

    /* Case 2: pad up to the target offset. */
    while (wp < off) {
        if (program_pad(pbn) != FTL_OK) {
            /* The block in service just failed. Retiring it relocates the live
             * data it holds into a replacement and repoints the mapping, so the
             * caller's retry finds a healthy block behind the same LBN. */
            (void)badblock_retire(pbn, "program failure while padding");
            return FTL_ERR_MEDIA;
        }
        wp++;
    }

    /* Case 1: append. */
    err = program_into_block(pbn, lpn, buf, NAND_TAG_USER_DATA, &ppn);
    if (err != FTL_OK) {
        if (err == FTL_ERR_MEDIA) {
            (void)badblock_retire(pbn, "program failure on append");
        }
        return err;
    }
    g_ftl.stats.pgm_host++;
    mapping_validate_ppn(ppn);
    return FTL_OK;
}

static ftl_err_t block_read(uint32_t lpn, void *buf)
{
    uint32_t lbn = LPN_TO_LBN(lpn);
    uint32_t off = LPN_TO_LOFF(lpn);
    uint32_t pbn = g_ftl.l2p.block[lbn];
    uint32_t ppn;

    if (pbn == PBN_INVALID) {
        return FTL_ERR_UNWRITTEN;
    }
    ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
    if (!BITMAP_GET(g_ftl.page_valid, ppn)) {
        return FTL_ERR_UNWRITTEN;
    }
    return read_verified(ppn, lpn, buf);
}

static ftl_err_t block_trim(uint32_t lpn)
{
    uint32_t lbn = LPN_TO_LBN(lpn);
    uint32_t pbn = g_ftl.l2p.block[lbn];

    if (pbn == PBN_INVALID) {
        return FTL_OK;
    }
    mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(pbn, LPN_TO_LOFF(lpn)));

    /* If TRIM emptied the whole block, hand it straight back. This is the
     * single clearest illustration of why TRIM is worth having: without the
     * host's promise that it will never read these LPNs again, the FTL would
     * be obliged to preserve every one of them forever. */
    if (g_ftl.blk[pbn].valid_pages == 0u) {
        g_ftl.l2p.block[lbn] = PBN_INVALID;
        (void)blockmgr_release(pbn);
    }
    return FTL_OK;
}

/* ===========================================================================
 * SECTION 6 -- HYBRID MAPPING (log-block scheme, BAST flavour)
 * ---------------------------------------------------------------------------
 * Block mapping is cheap in RAM and terrible at random writes. Page mapping is
 * the reverse. Hybrid mapping keeps block mapping for the bulk of the data and
 * bolts a small, page-mapped write buffer -- a LOG BLOCK -- in front of the
 * data block that is currently being updated.
 *
 * Updates are appended to the log block in whatever order they arrive. Later,
 * the log block and its data block are MERGED back into a single block-mapped
 * block. Merges come in three flavours, and knowing all three cold is the thing
 * that separates someone who has read about hybrid FTLs from someone who has
 * implemented one:
 *
 *   SWITCH MERGE   The log block happens to contain offsets 0,1,2,...,PPB-1 in
 *                  exactly that order -- i.e. the host wrote the whole logical
 *                  block sequentially. The log block IS a valid data block, so
 *                  we just repoint the mapping at it and erase the old data
 *                  block. Cost: one erase, ZERO page copies. The best case, and
 *                  the reason sequential workloads look fine on hybrid FTLs.
 *
 *   PARTIAL MERGE  The log holds offsets 0..m-1 in order, but stops short.
 *                  Copy the remaining offsets from the data block into the tail
 *                  of the log block, then switch. Cost: (PPB - m) copies.
 *
 *   FULL MERGE     Anything else -- the usual case under random writes. Allocate
 *                  a third block and copy the newest version of every offset
 *                  into it from wherever it currently lives. Cost: up to PPB
 *                  copies and TWO erases. This is the operation that dominates
 *                  hybrid-FTL write amplification.
 *
 * BAST vs FAST: this implementation is block-associative -- at most one log
 * block per data block. Its known failure mode is LOG BLOCK THRASHING: a
 * workload touching more distinct logical blocks than there are log blocks
 * forces a merge on nearly every write. FAST makes log blocks fully associative
 * (any log block can hold updates for any data block), which fixes thrashing
 * but makes each merge far more expensive because one log block's contents are
 * now spread across many data blocks. There is no free lunch; there is only a
 * choice about where to spend.
 * ========================================================================= */

static int hybrid_find_log(uint32_t lbn)
{
    int i;
    for (i = 0; i < (int)FTL_LOG_BLOCK_POOL; i++) {
        if (g_ftl.log[i].pbn != PBN_INVALID && g_ftl.log[i].lbn == lbn) {
            return i;
        }
    }
    return -1;
}

static void hybrid_reset_slot(int slot)
{
    uint32_t i;

    g_ftl.log[slot].pbn          = PBN_INVALID;
    g_ftl.log[slot].lbn          = PBN_INVALID;
    g_ftl.log[slot].used         = 0u;
    g_ftl.log[slot].last_use_seq = 0u;
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        g_ftl.log[slot].off_of_logpage[i] = -1;
        g_ftl.log[slot].logpage_of_off[i] = -1;
    }
}

/* Classify what a merge of this slot would cost, in page copies.
 * Also reports whether the identity/prefix condition for the cheap merges
 * holds. Used both to perform the merge and to choose which slot to evict. */
static uint32_t hybrid_merge_cost(int slot, bool *is_switch, bool *is_partial)
{
    const log_block_t *lb  = &g_ftl.log[slot];
    uint32_t           dpbn = g_ftl.l2p.block[lb->lbn];
    bool               prefix_in_order = true;
    uint32_t           i, highest = 0u;
    bool               any = false;

    /* Is the log a strictly in-order prefix 0,1,2,...,used-1? */
    for (i = 0u; i < lb->used; i++) {
        if (lb->off_of_logpage[i] != (int16_t)i) {
            prefix_in_order = false;
            break;
        }
    }

    /* Highest offset that has live data anywhere (log or data block). */
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        bool live = (lb->logpage_of_off[i] >= 0);
        if (!live && dpbn != PBN_INVALID) {
            live = BITMAP_GET(g_ftl.page_valid, BLOCK_OFFSET_TO_PPN(dpbn, i)) != 0u;
        }
        if (live) {
            highest = i;
            any = true;
        }
    }

    *is_switch  = false;
    *is_partial = false;

    if (prefix_in_order && lb->used == NAND_PAGES_PER_BLOCK) {
        *is_switch = true;
        return 0u;                       /* nothing to copy at all */
    }
    if (prefix_in_order && any && highest < lb->used) {
        /* The log already covers every live offset, in order: still a switch,
         * with the tail of the block simply left erased. */
        *is_switch = true;
        return 0u;
    }
    if (prefix_in_order) {
        *is_partial = true;
        return (highest + 1u) - lb->used; /* copy the tail across */
    }
    return any ? (highest + 1u) : 0u;     /* full merge */
}

/* Perform the merge for one slot, freeing it. */
static ftl_err_t hybrid_merge(int slot)
{
    log_block_t *lb    = &g_ftl.log[slot];
    uint32_t     lbn   = lb->lbn;
    uint32_t     lpbn  = lb->pbn;
    uint32_t     dpbn  = g_ftl.l2p.block[lbn];
    bool         is_switch, is_partial;
    uint32_t     copies, i, ppn, src_ppn, new_pbn, highest;
    ftl_err_t    err;

    copies = hybrid_merge_cost(slot, &is_switch, &is_partial);
    (void)copies;

    /* ---------------- SWITCH MERGE ---------------- */
    if (is_switch) {
        g_ftl.stats.switch_merges++;

        g_ftl.l2p.block[lbn]        = lpbn;
        g_ftl.blk[lpbn].state       = BLK_DATA;
        g_ftl.blk[lpbn].owner_lbn   = lbn;

        if (dpbn != PBN_INVALID) {
            for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
                mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(dpbn, i));
            }
            (void)blockmgr_release(dpbn);
        }
        hybrid_reset_slot(slot);
        return FTL_OK;
    }

    /* ---------------- PARTIAL MERGE ---------------- */
    if (is_partial) {
        g_ftl.stats.partial_merges++;

        highest = 0u;
        for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
            if (dpbn != PBN_INVALID &&
                BITMAP_GET(g_ftl.page_valid, BLOCK_OFFSET_TO_PPN(dpbn, i))) {
                highest = i;
            }
        }
        for (i = lb->used; i <= highest; i++) {
            src_ppn = BLOCK_OFFSET_TO_PPN(dpbn, i);
            if (BITMAP_GET(g_ftl.page_valid, src_ppn)) {
                if (read_verified(src_ppn, LPN_INVALID, COPY_BUF()) != FTL_OK) {
                    g_ftl.stats.pages_lost++;
                    if (program_pad(lpbn) != FTL_OK) {
                        goto partial_failed;
                    }
                    continue;
                }
                err = program_into_block(lpbn, lbn * NAND_PAGES_PER_BLOCK + i,
                                         COPY_BUF(), NAND_TAG_USER_DATA, &ppn);
                if (err != FTL_OK) {
                    goto partial_failed;
                }
                g_ftl.stats.pgm_merge++;
                mapping_validate_ppn(ppn);
            } else if (program_pad(lpbn) != FTL_OK) {
                goto partial_failed;
            }
        }

        g_ftl.l2p.block[lbn]      = lpbn;
        g_ftl.blk[lpbn].state     = BLK_DATA;
        g_ftl.blk[lpbn].owner_lbn = lbn;

        for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
            mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(dpbn, i));
        }
        (void)blockmgr_release(dpbn);
        hybrid_reset_slot(slot);
        return FTL_OK;

partial_failed:
        /* A program failed partway through appending the data block's tail onto
         * the log block. We cannot simply retry: a partial merge only works
         * because log page k holds logical offset k, and the failed programs
         * have pushed the write pointer past where the next offset belongs. The
         * alignment is gone and cannot be recovered.
         *
         * So we throw away the half-copied tail and fall through to a full
         * merge, which allocates a fresh block and does not care about
         * alignment. Slower, but it always works -- and having a strictly more
         * general fallback for every optimised path is what keeps a design
         * robust under conditions the optimised path did not anticipate. */
        for (i = lb->used; i < NAND_PAGES_PER_BLOCK; i++) {
            mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(lpbn, i));
        }
        g_ftl.stats.partial_merges--;
    }

    /* ---------------- FULL MERGE ---------------- */
    g_ftl.stats.full_merges++;

    new_pbn = blockmgr_alloc();
    if (new_pbn == PBN_INVALID) {
        return FTL_ERR_NO_SPACE;
    }
    g_ftl.blk[new_pbn].state     = BLK_DATA;
    g_ftl.blk[new_pbn].owner_lbn = lbn;

    highest = 0u;
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        bool live = (lb->logpage_of_off[i] >= 0);
        if (!live && dpbn != PBN_INVALID) {
            live = BITMAP_GET(g_ftl.page_valid, BLOCK_OFFSET_TO_PPN(dpbn, i)) != 0u;
        }
        if (live) {
            highest = i;
        }
    }

    for (i = 0u; i <= highest; i++) {
        src_ppn = PPN_INVALID;

        /* The log always wins: if an offset exists in both places, the log
         * copy is by construction the newer one. */
        if (lb->logpage_of_off[i] >= 0) {
            src_ppn = BLOCK_OFFSET_TO_PPN(lpbn, (uint32_t)lb->logpage_of_off[i]);
        } else if (dpbn != PBN_INVALID &&
                   BITMAP_GET(g_ftl.page_valid, BLOCK_OFFSET_TO_PPN(dpbn, i))) {
            src_ppn = BLOCK_OFFSET_TO_PPN(dpbn, i);
        }

        if (src_ppn == PPN_INVALID) {
            if (program_pad(new_pbn) != FTL_OK) {
                return FTL_ERR_MEDIA;
            }
            continue;
        }
        if (read_verified(src_ppn, LPN_INVALID, COPY_BUF()) != FTL_OK) {
            g_ftl.stats.pages_lost++;
            if (program_pad(new_pbn) != FTL_OK) {
                return FTL_ERR_MEDIA;
            }
            continue;
        }
        err = program_into_block(new_pbn, lbn * NAND_PAGES_PER_BLOCK + i,
                                 COPY_BUF(), NAND_TAG_USER_DATA, &ppn);
        if (err != FTL_OK) {
            return FTL_ERR_MEDIA;
        }
        g_ftl.stats.pgm_merge++;
        mapping_validate_ppn(ppn);
    }

    g_ftl.l2p.block[lbn] = new_pbn;

    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(lpbn, i));
        if (dpbn != PBN_INVALID) {
            mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(dpbn, i));
        }
    }
    (void)blockmgr_release(lpbn);
    if (dpbn != PBN_INVALID) {
        (void)blockmgr_release(dpbn);
    }
    hybrid_reset_slot(slot);
    return FTL_OK;
}

/* Get a log slot for `lbn`, merging the cheapest existing one if the pool is
 * full. "Cheapest" is the point: evicting the slot whose merge costs least is
 * the hybrid-FTL analogue of greedy GC victim selection. */
static int hybrid_acquire_log(uint32_t lbn)
{
    int      i, victim = -1, free_slot = -1;
    uint32_t best_cost = 0xFFFFFFFFu;
    bool     sw, pa;
    uint32_t cost, pbn;

    for (i = 0; i < (int)FTL_LOG_BLOCK_POOL; i++) {
        if (g_ftl.log[i].pbn == PBN_INVALID) {
            free_slot = i;
            break;
        }
    }

    if (free_slot < 0) {
        for (i = 0; i < (int)FTL_LOG_BLOCK_POOL; i++) {
            cost = hybrid_merge_cost(i, &sw, &pa);
            if (cost < best_cost ||
                (cost == best_cost && victim >= 0 &&
                 g_ftl.log[i].last_use_seq < g_ftl.log[victim].last_use_seq)) {
                best_cost = cost;
                victim    = i;
            }
        }
        if (victim < 0 || hybrid_merge(victim) != FTL_OK) {
            return -1;
        }
        free_slot = victim;
    }

    pbn = blockmgr_alloc();
    if (pbn == PBN_INVALID) {
        return -1;
    }
    hybrid_reset_slot(free_slot);
    g_ftl.log[free_slot].pbn          = pbn;
    g_ftl.log[free_slot].lbn          = lbn;
    g_ftl.log[free_slot].last_use_seq = g_ftl.seq;
    g_ftl.blk[pbn].state              = BLK_LOG;
    g_ftl.blk[pbn].owner_lbn          = lbn;

    return free_slot;
}

static ftl_err_t hybrid_write(uint32_t lpn, const void *buf)
{
    uint32_t  lbn  = LPN_TO_LBN(lpn);
    uint32_t  off  = LPN_TO_LOFF(lpn);
    uint32_t  dpbn = g_ftl.l2p.block[lbn];
    int       slot = hybrid_find_log(lbn);
    uint32_t  ppn, old_ppn;
    ftl_err_t err;

    /* Fast path: no log block yet, and this write lands exactly at the data
     * block's write pointer. Pure sequential fill -- one program, no log block
     * consumed, no merge ever needed. This path is why hybrid FTLs are fine on
     * sequential workloads and why benchmark numbers for them are so sensitive
     * to access pattern. */
    if (slot < 0) {
        if (dpbn == PBN_INVALID && off == 0u) {
            dpbn = blockmgr_alloc();
            if (dpbn == PBN_INVALID) {
                return FTL_ERR_NO_SPACE;
            }
            g_ftl.blk[dpbn].state     = BLK_DATA;
            g_ftl.blk[dpbn].owner_lbn = lbn;
            g_ftl.l2p.block[lbn]      = dpbn;
        }
        if (dpbn != PBN_INVALID && off == nand_block_write_pointer(dpbn)) {
            err = program_into_block(dpbn, lpn, buf, NAND_TAG_USER_DATA, &ppn);
            if (err != FTL_OK) {
                return err;
            }
            g_ftl.stats.pgm_host++;
            mapping_validate_ppn(ppn);
            return FTL_OK;
        }
    }

    /* Slow path: the update goes to a log block. */
    if (slot < 0) {
        slot = hybrid_acquire_log(lbn);
        if (slot < 0) {
            return FTL_ERR_NO_SPACE;
        }
    }
    if (g_ftl.log[slot].used >= NAND_PAGES_PER_BLOCK) {
        /* The log block is full; merge it and start a fresh one. */
        err = hybrid_merge(slot);
        if (err != FTL_OK) {
            return err;
        }
        slot = hybrid_acquire_log(lbn);
        if (slot < 0) {
            return FTL_ERR_NO_SPACE;
        }
    }

    err = program_into_block(g_ftl.log[slot].pbn, lpn, buf,
                             NAND_TAG_LOG_DATA, &ppn);
    if (err != FTL_OK) {
        if (err == FTL_ERR_MEDIA) {
            (void)badblock_retire(g_ftl.log[slot].pbn,
                                  "program failure in hybrid log");
        }
        return err;
    }
    g_ftl.stats.pgm_host++;

    /* Retire the previous copy of this offset, wherever it lived. */
    if (g_ftl.log[slot].logpage_of_off[off] >= 0) {
        old_ppn = BLOCK_OFFSET_TO_PPN(g_ftl.log[slot].pbn,
                                      (uint32_t)g_ftl.log[slot].logpage_of_off[off]);
        mapping_invalidate_ppn(old_ppn);
    } else if (dpbn != PBN_INVALID) {
        mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(dpbn, off));
    }

    g_ftl.log[slot].off_of_logpage[g_ftl.log[slot].used] = (int16_t)off;
    g_ftl.log[slot].logpage_of_off[off] = (int16_t)g_ftl.log[slot].used;
    g_ftl.log[slot].used++;
    g_ftl.log[slot].last_use_seq = g_ftl.seq;
    mapping_validate_ppn(ppn);

    return FTL_OK;
}

static ftl_err_t hybrid_read(uint32_t lpn, void *buf)
{
    uint32_t lbn  = LPN_TO_LBN(lpn);
    uint32_t off  = LPN_TO_LOFF(lpn);
    int      slot = hybrid_find_log(lbn);
    uint32_t dpbn, ppn;

    if (slot >= 0 && g_ftl.log[slot].logpage_of_off[off] >= 0) {
        ppn = BLOCK_OFFSET_TO_PPN(g_ftl.log[slot].pbn,
                                  (uint32_t)g_ftl.log[slot].logpage_of_off[off]);
        return read_verified(ppn, lpn, buf);
    }

    dpbn = g_ftl.l2p.block[lbn];
    if (dpbn == PBN_INVALID) {
        return FTL_ERR_UNWRITTEN;
    }
    ppn = BLOCK_OFFSET_TO_PPN(dpbn, off);
    if (!BITMAP_GET(g_ftl.page_valid, ppn)) {
        return FTL_ERR_UNWRITTEN;
    }
    return read_verified(ppn, lpn, buf);
}

static ftl_err_t hybrid_trim(uint32_t lpn)
{
    uint32_t lbn  = LPN_TO_LBN(lpn);
    uint32_t off  = LPN_TO_LOFF(lpn);
    int      slot = hybrid_find_log(lbn);
    uint32_t dpbn = g_ftl.l2p.block[lbn];

    if (slot >= 0 && g_ftl.log[slot].logpage_of_off[off] >= 0) {
        mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(
            g_ftl.log[slot].pbn, (uint32_t)g_ftl.log[slot].logpage_of_off[off]));
        g_ftl.log[slot].logpage_of_off[off] = -1;
    }
    if (dpbn != PBN_INVALID) {
        mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(dpbn, off));
    }
    return FTL_OK;
}

/* ===========================================================================
 * SECTION 7 -- Relocating an entire block
 * ---------------------------------------------------------------------------
 * Three different subsystems need to empty a block without losing anything:
 * static wear levelling (to free up a young block for reuse), read-disturb
 * refresh (to rewrite a block that has been read too many times), and the
 * bad-block manager (to rescue data off a block that just failed a program).
 *
 * They all call this. The scheme-awareness lives here once instead of three
 * times, which is the only realistic way to keep the block-mapped case correct.
 * ========================================================================= */

static ftl_err_t relocate_page_mapped(uint32_t pbn, uint64_t *bucket)
{
    uint32_t     off, ppn, new_ppn, wp;
    nand_spare_t spare;
    ftl_err_t    err;

    wp = nand_block_write_pointer(pbn);
    for (off = 0u; off < wp; off++) {
        ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
        if (!BITMAP_GET(g_ftl.page_valid, ppn)) {
            continue;
        }
        if (s_buf_depth + 1 >= MAPPING_BUF_DEPTH) {
            return FTL_ERR_MEDIA;
        }
        s_buf_depth++;
        if (nand_read_page(ppn, COPY_BUF(), &spare) != NAND_OK) {
            s_buf_depth--;
            g_ftl.stats.pages_lost++;
            mapping_invalidate_ppn(ppn);
            continue;
        }
        err = mapping_program_at_frontier(spare.lpn, COPY_BUF(),
                                          NAND_TAG_USER_DATA, &new_ppn);
        s_buf_depth--;
        if (err != FTL_OK) {
            return err;
        }
        (*bucket)++;

        g_ftl.l2p.page[spare.lpn] = new_ppn;
        mapping_validate_ppn(new_ppn);
        mapping_invalidate_ppn(ppn);
    }
    return FTL_OK;
}

/* Copy a block-mapped (or log) block wholesale into a replacement, preserving
 * page offsets exactly -- because under block mapping the offset IS part of the
 * address, so it cannot change. */
static ftl_err_t relocate_block_mapped(uint32_t pbn, uint64_t *bucket)
{
    uint32_t     new_pbn, off, ppn, src_ppn, wp;
    nand_spare_t spare;
    int          slot = -1, i;
    ftl_err_t    err;

    new_pbn = blockmgr_alloc();
    if (new_pbn == PBN_INVALID) {
        return FTL_ERR_NO_SPACE;
    }
    g_ftl.blk[new_pbn].state     = g_ftl.blk[pbn].state;
    g_ftl.blk[new_pbn].owner_lbn = g_ftl.blk[pbn].owner_lbn;

    wp = nand_block_write_pointer(pbn);
    for (off = 0u; off < wp; off++) {
        src_ppn = BLOCK_OFFSET_TO_PPN(pbn, off);

        if (!BITMAP_GET(g_ftl.page_valid, src_ppn)) {
            if (program_pad(new_pbn) != FTL_OK) {
                goto fail;
            }
            continue;
        }
        if (s_buf_depth + 1 >= MAPPING_BUF_DEPTH) {
            goto fail;
        }
        s_buf_depth++;
        if (nand_read_page(src_ppn, COPY_BUF(), &spare) != NAND_OK) {
            s_buf_depth--;
            g_ftl.stats.pages_lost++;
            mapping_invalidate_ppn(src_ppn);
            if (program_pad(new_pbn) != FTL_OK) {
                goto fail;
            }
            continue;
        }
        err = program_into_block(new_pbn, spare.lpn, COPY_BUF(), spare.tag, &ppn);
        s_buf_depth--;
        if (err != FTL_OK) {
            goto fail;
        }
        (*bucket)++;
        mapping_validate_ppn(ppn);
        mapping_invalidate_ppn(src_ppn);
    }

    /* Repoint whoever owned the old block at the new one. */
    if (g_ftl.blk[pbn].state == BLK_LOG) {
        for (i = 0; i < (int)FTL_LOG_BLOCK_POOL; i++) {
            if (g_ftl.log[i].pbn == pbn) {
                slot = i;
                break;
            }
        }
        if (slot >= 0) {
            g_ftl.log[slot].pbn = new_pbn;
        }
    } else if (g_ftl.blk[pbn].owner_lbn != PBN_INVALID) {
        g_ftl.l2p.block[g_ftl.blk[pbn].owner_lbn] = new_pbn;
    }
    return FTL_OK;

fail:
    for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
        mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(new_pbn, off));
    }
    (void)blockmgr_release(new_pbn);
    return FTL_ERR_MEDIA;
}

ftl_err_t mapping_relocate_block(uint32_t pbn, uint64_t *bucket)
{
    if (pbn >= NAND_NUM_BLOCKS) {
        return FTL_ERR_RANGE;
    }
    if (g_ftl.blk[pbn].valid_pages == 0u) {
        return FTL_OK;      /* nothing to move */
    }

    if (g_ftl.params.mapping == FTL_MAP_PAGE) {
        /* Never relocate into the block we are relocating out of. */
        if (g_ftl.active_pbn == pbn) {
            g_ftl.blk[pbn].state = BLK_DATA;
            g_ftl.active_pbn     = PBN_INVALID;
            g_ftl.active_off     = 0u;
        }
        return relocate_page_mapped(pbn, bucket);
    }
    return relocate_block_mapped(pbn, bucket);
}

/* ===========================================================================
 * SECTION 8 -- The scheme-independent entry points
 * ========================================================================= */

ftl_err_t mapping_init(void)
{
    uint32_t i;
    int      s;

    switch (g_ftl.params.mapping) {
    case FTL_MAP_PAGE:
        for (i = 0u; i < FTL_LOGICAL_PAGES; i++) {
            g_ftl.l2p.page[i] = PPN_INVALID;
        }
        break;
    case FTL_MAP_BLOCK:
    case FTL_MAP_HYBRID:
        for (i = 0u; i < FTL_LOGICAL_BLOCKS; i++) {
            g_ftl.l2p.block[i] = PBN_INVALID;
        }
        break;
    default:
        return FTL_ERR_UNSUPPORTED;
    }

    for (s = 0; s < (int)FTL_LOG_BLOCK_POOL; s++) {
        hybrid_reset_slot(s);
    }
    memset(g_ftl.page_valid, 0, sizeof(g_ftl.page_valid));
    return FTL_OK;
}

ftl_err_t mapping_write(uint32_t lpn, const void *buf)
{
    switch (g_ftl.params.mapping) {
    case FTL_MAP_PAGE:   return page_write(lpn, buf);
    case FTL_MAP_BLOCK:  return block_write(lpn, buf);
    case FTL_MAP_HYBRID: return hybrid_write(lpn, buf);
    default:             return FTL_ERR_UNSUPPORTED;
    }
}

ftl_err_t mapping_read(uint32_t lpn, void *buf)
{
    switch (g_ftl.params.mapping) {
    case FTL_MAP_PAGE:   return page_read(lpn, buf);
    case FTL_MAP_BLOCK:  return block_read(lpn, buf);
    case FTL_MAP_HYBRID: return hybrid_read(lpn, buf);
    default:             return FTL_ERR_UNSUPPORTED;
    }
}

ftl_err_t mapping_trim(uint32_t lpn)
{
    switch (g_ftl.params.mapping) {
    case FTL_MAP_PAGE:   return page_trim(lpn);
    case FTL_MAP_BLOCK:  return block_trim(lpn);
    case FTL_MAP_HYBRID: return hybrid_trim(lpn);
    default:             return FTL_ERR_UNSUPPORTED;
    }
}

/* Where does this LPN currently live? PPN_INVALID if nowhere. */
uint32_t mapping_lookup(uint32_t lpn)
{
    uint32_t lbn, off, pbn, ppn;
    int      slot;

    if (lpn >= FTL_LOGICAL_PAGES) {
        return PPN_INVALID;
    }

    if (g_ftl.params.mapping == FTL_MAP_PAGE) {
        return g_ftl.l2p.page[lpn];
    }

    lbn = LPN_TO_LBN(lpn);
    off = LPN_TO_LOFF(lpn);

    if (g_ftl.params.mapping == FTL_MAP_HYBRID) {
        slot = hybrid_find_log(lbn);
        if (slot >= 0 && g_ftl.log[slot].logpage_of_off[off] >= 0) {
            return BLOCK_OFFSET_TO_PPN(g_ftl.log[slot].pbn,
                                       (uint32_t)g_ftl.log[slot].logpage_of_off[off]);
        }
    }

    pbn = g_ftl.l2p.block[lbn];
    if (pbn == PBN_INVALID) {
        return PPN_INVALID;
    }
    ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
    return BITMAP_GET(g_ftl.page_valid, ppn) ? ppn : PPN_INVALID;
}
