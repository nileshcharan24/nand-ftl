/* =============================================================================
 * journal.c -- Checkpoints, and rebuilding the FTL after an unexpected power loss.
 * =============================================================================
 *
 * THE PROBLEM
 * -----------
 * The mapping table lives in volatile RAM. Pull the power and it is gone. The
 * data is all still on the flash -- but without the table, the drive has no
 * idea which physical page holds logical page 12345, which is the same as
 * having lost everything.
 *
 * A drive that cannot survive a power cut is not a storage device. And "power
 * cut" is not an exotic event: it is a user pulling a USB stick out, a laptop
 * battery dying, a data-centre rack losing a feed. Firmware is expected to come
 * back from it EVERY time, not usually.
 *
 * THE TWO MECHANISMS
 * ------------------
 * 1. THE SPARE AREA IS THE JOURNAL.
 *    Every data page is programmed together with the logical page number it
 *    holds and a globally monotonic sequence number (see nand.h). That means the
 *    media is SELF-DESCRIBING: you can rebuild the entire mapping table by
 *    reading every page's spare area and, for each logical page, keeping the
 *    physical copy with the highest sequence number.
 *
 *    This costs nothing extra -- the spare area was going to be programmed
 *    anyway -- and it is the reason this design does not need a separate
 *    write-ahead log. A separate log would double the metadata write traffic
 *    for information that is already implicit in the data writes.
 *
 * 2. CHECKPOINTS BOUND THE SCAN.
 *    Rebuilding by full scan is O(capacity). On a 4 TB drive that is minutes,
 *    and nobody waits minutes for a disk to appear. So we periodically write a
 *    snapshot of the whole FTL state into a fixed metadata region. On the next
 *    mount we load the snapshot and only have to examine the pages written
 *    AFTER it.
 *
 *    The bound is what matters: mount time becomes a function of the checkpoint
 *    interval, not of capacity.
 *
 * WHY THE HEADER PAGE IS WRITTEN LAST
 * -----------------------------------
 * A checkpoint spans several pages. If power is lost halfway through writing
 * one, we must not mount from the half we got. So the payload pages go down
 * first and the header -- which carries the magic number, the sequence number
 * and a CRC over the payload -- goes last. A header on the media is therefore
 * proof that everything it describes made it. A torn checkpoint has no header,
 * is invisible to recovery, and the previous checkpoint is used instead.
 *
 * This "commit record written last" pattern is the same one journalling file
 * systems and databases use, for exactly the same reason.
 *
 * WHAT THIS DESIGN DOES NOT PROTECT, STATED HONESTLY
 * --------------------------------------------------
 * TRIM is not journalled. If the host TRIMs a page and power is lost before the
 * next checkpoint, recovery will find the old data still on the media, still
 * carrying the highest sequence number for that LPN, and will resurrect it.
 * That is not laziness -- NVMe explicitly permits it (Dataset Management
 * Deallocate is non-deterministic unless the drive advertises otherwise) -- but
 * it is a real behaviour and it is named again in DOCS/INTERVIEW_QA.md rather
 * than glossed over.
 * ========================================================================== */

#include <string.h>

#include "ftl_internal.h"

#define CKPT_MAGIC        0x46544C43u   /* "FTLC" */
#define CKPT_VERSION      1u
#define CKPT_HEADER_LPN   0xFFFFFFFEu   /* sentinel in the spare area's lpn field */

/* ---------------------------------------------------------------------------
 * The commit record.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t ckpt_seq;        /* global sequence number as of the snapshot     */
    uint32_t payload_bytes;
    uint32_t payload_crc;
    uint32_t first_ppn;       /* PPN of payload chunk 0 (chunks are contiguous)*/
    uint32_t chunk_count;
    uint32_t mapping_scheme;  /* the snapshot is only meaningful for this scheme*/
    uint32_t header_crc;      /* over every field above                        */
} ckpt_header_t;

/* ---------------------------------------------------------------------------
 * Static scratch. All of it is used only during checkpoint/recovery.
 *
 * Being straight about the cost: s_ckpt_buf holds a whole copy of the FTL
 * context (~29 KiB at the default geometry) so that the snapshot is consistent
 * while it is being written out. Real firmware does not double-buffer its
 * mapping table -- it streams the table out page by page and relies on the
 * journal to reconcile entries that changed mid-flush. Buffering is simpler and
 * provably consistent; the trade is RAM, and this is exactly the sort of trade
 * that gets argued about in a firmware design review.
 * ------------------------------------------------------------------------- */
#define CKPT_CHUNKS   (((sizeof(ftl_ctx_t)) + NAND_PAGE_SIZE_BYTES - 1u) / NAND_PAGE_SIZE_BYTES)

static uint8_t  s_ckpt_buf[CKPT_CHUNKS * NAND_PAGE_SIZE_BYTES];
static uint8_t  s_page_buf[NAND_PAGE_SIZE_BYTES];

/* Highest sequence number seen so far for each logical page, during recovery. */
static uint32_t s_best_seq[FTL_LOGICAL_PAGES];

/* Per-block scratch used by the block/hybrid reconstruction pass. */
static uint8_t  s_role[NAND_NUM_BLOCKS];       /* 0 empty, 1 data, 2 log */
static uint32_t s_role_lbn[NAND_NUM_BLOCKS];
static uint32_t s_role_seq[NAND_NUM_BLOCKS];
static uint8_t  s_dirty[NAND_NUM_BLOCKS];      /* contains pages newer than ckpt */

#define ROLE_EMPTY 0u
#define ROLE_DATA  1u
#define ROLE_LOG   2u

/* ===========================================================================
 * Metadata region management
 * ---------------------------------------------------------------------------
 * Checkpoints are appended to the metadata blocks round-robin. When the current
 * block cannot fit another whole checkpoint we move to the next one and erase
 * it. Because a checkpoint always fits inside a single block, the newest
 * checkpoint is always in the block we are currently appending to -- so erasing
 * the NEXT block in rotation can never destroy the only good copy.
 * ========================================================================= */
void journal_init(void)
{
    g_ftl.meta_pbn          = FTL_META_BLOCK_FIRST;
    g_ftl.meta_off          = 0u;
    g_ftl.ckpt_seq          = 0u;
    g_ftl.writes_since_ckpt = 0u;
}

static ftl_err_t meta_reserve(uint32_t pages_needed)
{
    uint32_t next;

    if (g_ftl.meta_off + pages_needed <= NAND_PAGES_PER_BLOCK) {
        return FTL_OK;
    }

    next = g_ftl.meta_pbn + 1u;
    if (next > FTL_META_BLOCK_LAST) {
        next = FTL_META_BLOCK_FIRST;
    }
    if (nand_erase_block(next) != NAND_OK) {
        (void)badblock_retire(next, "metadata erase failure");
        return FTL_ERR_MEDIA;
    }
    g_ftl.stats.erases++;

    g_ftl.meta_pbn = next;
    g_ftl.meta_off = 0u;
    return FTL_OK;
}

static ftl_err_t meta_program(uint32_t lpn_field, const void *data, uint32_t *ppn_out)
{
    nand_spare_t spare;
    uint32_t     ppn;

    ppn = BLOCK_OFFSET_TO_PPN(g_ftl.meta_pbn, g_ftl.meta_off);

    spare.lpn = lpn_field;
    spare.seq = g_ftl.seq;          /* NOT ++: a checkpoint records state, it
                                     * does not create a newer version of data */
    spare.crc = 0u;
    spare.tag = NAND_TAG_CHECKPOINT;

    if (nand_program_page(ppn, data, &spare) != NAND_OK) {
        return FTL_ERR_MEDIA;
    }
    g_ftl.meta_off++;
    g_ftl.stats.pgm_meta++;

    if (ppn_out != NULL) {
        *ppn_out = ppn;
    }
    return FTL_OK;
}

/* After a mount we have to work out where in the metadata region the next
 * checkpoint should go. We cannot just start at page 0 -- those pages are
 * already programmed, and program-once means writing there would fail. The
 * write pointers on the die tell us exactly where we left off. */
static void meta_resync(void)
{
    uint32_t     pbn, wp, best = PBN_INVALID, best_seq = 0u, next;
    nand_spare_t spare;

    for (pbn = FTL_META_BLOCK_FIRST; pbn <= FTL_META_BLOCK_LAST; pbn++) {
        if (nand_block_is_bad(pbn)) {
            continue;
        }
        wp = nand_block_write_pointer(pbn);
        if (wp == 0u) {
            continue;
        }
        if (nand_read_spare(BLOCK_OFFSET_TO_PPN(pbn, wp - 1u), &spare) != NAND_OK) {
            continue;
        }
        if (best == PBN_INVALID || spare.seq >= best_seq) {
            best     = pbn;
            best_seq = spare.seq;
        }
    }

    if (best != PBN_INVALID &&
        nand_block_write_pointer(best) + CKPT_CHUNKS + 1u <= NAND_PAGES_PER_BLOCK) {
        g_ftl.meta_pbn = best;
        g_ftl.meta_off = nand_block_write_pointer(best);
        return;
    }

    /* The newest metadata block is full (or there is none). Move to the next
     * one in rotation and erase it. Safe, because a checkpoint never spans
     * blocks, so the newest good checkpoint is entirely inside `best`. */
    next = (best == PBN_INVALID) ? FTL_META_BLOCK_FIRST : best + 1u;
    if (next > FTL_META_BLOCK_LAST) {
        next = FTL_META_BLOCK_FIRST;
    }
    if (nand_block_write_pointer(next) != 0u) {
        if (nand_erase_block(next) != NAND_OK) {
            (void)badblock_retire(next, "metadata erase failure at mount");
        } else {
            g_ftl.stats.erases++;
        }
    }
    g_ftl.meta_pbn = next;
    g_ftl.meta_off = nand_block_write_pointer(next);
}

/* ===========================================================================
 * Writing a checkpoint
 * ========================================================================= */
ftl_err_t journal_checkpoint(void)
{
    ckpt_header_t hdr;
    uint32_t      i, first_ppn = PPN_INVALID, ppn;
    ftl_err_t     err;

    if (!g_ftl.params.checkpointing) {
        return FTL_OK;
    }

    err = meta_reserve((uint32_t)CKPT_CHUNKS + 1u);
    if (err != FTL_OK) {
        return err;
    }

    /* Snapshot first, so nothing shifts under us while we write it out. */
    memset(s_ckpt_buf, 0, sizeof(s_ckpt_buf));
    memcpy(s_ckpt_buf, &g_ftl, sizeof(ftl_ctx_t));

    for (i = 0u; i < (uint32_t)CKPT_CHUNKS; i++) {
        err = meta_program(i, s_ckpt_buf + (size_t)i * NAND_PAGE_SIZE_BYTES, &ppn);
        if (err != FTL_OK) {
            return err;
        }
        if (i == 0u) {
            first_ppn = ppn;
        }
    }

    hdr.magic          = CKPT_MAGIC;
    hdr.version        = CKPT_VERSION;
    hdr.ckpt_seq       = g_ftl.seq;
    hdr.payload_bytes  = (uint32_t)sizeof(ftl_ctx_t);
    hdr.payload_crc    = nand_crc32(s_ckpt_buf, sizeof(ftl_ctx_t));
    hdr.first_ppn      = first_ppn;
    hdr.chunk_count    = (uint32_t)CKPT_CHUNKS;
    hdr.mapping_scheme = (uint32_t)g_ftl.params.mapping;
    hdr.header_crc     = nand_crc32(&hdr, sizeof(hdr) - sizeof(uint32_t));

    /* THE COMMIT. Everything the header describes is already durable. */
    memset(s_page_buf, 0, sizeof(s_page_buf));
    memcpy(s_page_buf, &hdr, sizeof(hdr));
    err = meta_program(CKPT_HEADER_LPN, s_page_buf, NULL);
    if (err != FTL_OK) {
        return err;
    }

    g_ftl.ckpt_seq          = hdr.ckpt_seq;
    g_ftl.writes_since_ckpt = 0u;
    g_ftl.stats.checkpoints_written++;
    return FTL_OK;
}

/* ===========================================================================
 * Finding and loading the newest checkpoint
 * ========================================================================= */
static bool ckpt_find_latest(ckpt_header_t *out)
{
    uint32_t      pbn, off, ppn, wp;
    nand_spare_t  spare;
    ckpt_header_t hdr;
    bool          found = false;

    for (pbn = FTL_META_BLOCK_FIRST; pbn <= FTL_META_BLOCK_LAST; pbn++) {
        if (nand_block_is_bad(pbn)) {
            continue;
        }
        wp = nand_block_write_pointer(pbn);
        for (off = 0u; off < wp; off++) {
            ppn = BLOCK_OFFSET_TO_PPN(pbn, off);

            /* Cheap first: only full-read a page whose spare area says it is a
             * header. Everything else is payload we do not need yet. */
            if (nand_read_spare(ppn, &spare) != NAND_OK) {
                continue;
            }
            if (spare.tag != NAND_TAG_CHECKPOINT || spare.lpn != CKPT_HEADER_LPN) {
                continue;
            }
            if (nand_read_page(ppn, s_page_buf, NULL) != NAND_OK) {
                continue;       /* torn or corrupt: ignore, try an older one */
            }
            memcpy(&hdr, s_page_buf, sizeof(hdr));

            if (hdr.magic != CKPT_MAGIC || hdr.version != CKPT_VERSION) {
                continue;
            }
            if (hdr.header_crc != nand_crc32(&hdr, sizeof(hdr) - sizeof(uint32_t))) {
                continue;
            }
            if (!found || hdr.ckpt_seq > out->ckpt_seq) {
                *out  = hdr;
                found = true;
            }
        }
    }
    return found;
}

static ftl_err_t ckpt_load(const ckpt_header_t *hdr)
{
    uint32_t i;

    if (hdr->payload_bytes != (uint32_t)sizeof(ftl_ctx_t) ||
        hdr->chunk_count   != (uint32_t)CKPT_CHUNKS) {
        /* The firmware that wrote this had a different context layout. A real
         * product handles this with a versioned, explicitly serialised format
         * so that a firmware update can still read the old drive's metadata.
         * Refusing is the honest behaviour here rather than reading garbage. */
        return FTL_ERR_CORRUPT;
    }

    for (i = 0u; i < hdr->chunk_count; i++) {
        if (nand_read_page(hdr->first_ppn + i,
                           s_ckpt_buf + (size_t)i * NAND_PAGE_SIZE_BYTES,
                           NULL) != NAND_OK) {
            return FTL_ERR_MEDIA;
        }
    }
    if (nand_crc32(s_ckpt_buf, sizeof(ftl_ctx_t)) != hdr->payload_crc) {
        return FTL_ERR_CORRUPT;
    }

    memcpy(&g_ftl, s_ckpt_buf, sizeof(ftl_ctx_t));
    return FTL_OK;
}

/* ===========================================================================
 * Roll-forward for PAGE mapping
 * ---------------------------------------------------------------------------
 * Only pages with seq > ckpt_seq can change anything, and here is the trick
 * that makes finding them cheap:
 *
 *   Pages inside a block are programmed in strictly increasing offset order
 *   (the hardware rule from nand.h), and the sequence number is globally
 *   monotonic. Therefore the LAST programmed page of a block always carries the
 *   HIGHEST sequence number in that block.
 *
 * So one spare-area read per block -- 128 cheap reads at the default geometry,
 * and only a few thousand even on a real drive -- tells us which blocks are
 * worth scanning at all. Everything else is skipped without touching it.
 * ========================================================================= */
static void recover_pagemap_rollforward(uint32_t ckpt_seq)
{
    uint32_t     pbn, off, ppn, wp, lpn;
    nand_spare_t spare;
    uint32_t     newest_open = PBN_INVALID, newest_open_seq = 0u;

    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        s_best_seq[lpn] = ckpt_seq;
    }
    memset(s_dirty, 0, sizeof(s_dirty));

    /* A checkpoint records which blocks were in use, but not future erases.
     * Treat every block that was occupied at checkpoint time as dirty before
     * inspecting post-checkpoint pages. A later GC may have erased such a
     * block entirely, leaving no last page whose sequence could mark it. */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        s_dirty[pbn] = (g_ftl.blk[pbn].state != BLK_FREE) ? 1u : 0u;
    }

    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_bad(pbn)) {
            continue;
        }
        wp = nand_block_write_pointer(pbn);
        if (wp == 0u) {
            continue;
        }

        /* The one-read probe. */
        if (nand_read_spare(BLOCK_OFFSET_TO_PPN(pbn, wp - 1u), &spare) != NAND_OK) {
            continue;
        }
        g_ftl.stats.recovery_pages_scanned++;
        if (spare.seq <= ckpt_seq) {
            continue;               /* nothing here postdates the checkpoint */
        }
        s_dirty[pbn] = 1u;
        /* Cost-benefit GC needs a block's "last written" time. For blocks the
         * checkpoint already knew about it came across in the snapshot; for
         * these we take it from the newest page we just probed. */
        g_ftl.blk[pbn].last_write_seq = spare.seq;

        /* This block was being written after the checkpoint. If it is not full,
         * it was the open block when the power went -- resume appending to it
         * rather than stranding its remaining pages. */
        if (wp < NAND_PAGES_PER_BLOCK && spare.seq > newest_open_seq) {
            newest_open     = pbn;
            newest_open_seq = spare.seq;
        }

        for (off = 0u; off < wp; off++) {
            ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
            if (nand_read_spare(ppn, &spare) != NAND_OK) {
                continue;
            }
            g_ftl.stats.recovery_pages_scanned++;

            if (spare.tag != NAND_TAG_USER_DATA || spare.lpn >= FTL_LOGICAL_PAGES) {
                continue;
            }
            if (spare.seq <= s_best_seq[spare.lpn]) {
                continue;
            }

            mapping_invalidate_ppn(g_ftl.l2p.page[spare.lpn]);
            g_ftl.l2p.page[spare.lpn] = ppn;
            mapping_validate_ppn(ppn);
            s_best_seq[spare.lpn] = spare.seq;
            g_ftl.stats.recovery_pages_applied++;

            if (spare.seq > g_ftl.seq) {
                g_ftl.seq = spare.seq;
            }
        }
    }

    /* Reconcile block bookkeeping with what the media actually holds. The
     * checkpoint's view of which blocks were free or in use can be stale: a
     * block it recorded as free may since have been filled, and one it recorded
     * as data may have been collected and erased. The write pointer on the die
     * is the truth, so we recompute from that. */
    g_ftl.free_blocks = 0u;
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_bad(pbn)) {
            g_ftl.blk[pbn].state = BLK_BAD;
            continue;
        }
        if (nand_block_write_pointer(pbn) == 0u) {
            g_ftl.blk[pbn].state       = BLK_FREE;
            g_ftl.blk[pbn].valid_pages = 0u;
            g_ftl.free_blocks++;
        } else {
            g_ftl.blk[pbn].state = BLK_DATA;
        }
    }

    /* Drop any mapping that still points into a block whose contents changed
     * under it -- for example an LPN that was TRIMmed and whose block was then
     * erased, leaving the checkpoint's entry dangling. Only pages in dirty
     * blocks can be in this state, so the check is nearly free. */
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        ppn = g_ftl.l2p.page[lpn];
        if (ppn == PPN_INVALID) {
            continue;
        }
        pbn = PPN_TO_BLOCK(ppn);
        if (!s_dirty[pbn] && nand_page_is_programmed(ppn)) {
            continue;
        }
        if (!nand_page_is_programmed(ppn) ||
            nand_read_spare(ppn, &spare) != NAND_OK ||
            spare.lpn != lpn) {
            mapping_invalidate_ppn(ppn);
            g_ftl.l2p.page[lpn] = PPN_INVALID;
        }
    }

    /* The checkpoint bitmap describes the world at checkpoint time. Pages in
     * a block erased or reused afterwards can leave stale bits behind even
     * though the forward map has been corrected above. Rebuild this derived
     * cache from the recovered map so free-block and valid-page invariants are
     * based on current media, not on historical bookkeeping. */
    memset(g_ftl.page_valid, 0, sizeof(g_ftl.page_valid));
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        g_ftl.blk[pbn].valid_pages = 0u;
    }
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        ppn = g_ftl.l2p.page[lpn];
        if (ppn == PPN_INVALID || !nand_page_is_programmed(ppn)) {
            continue;
        }
        if (nand_read_spare(ppn, &spare) == NAND_OK && spare.lpn == lpn) {
            mapping_validate_ppn(ppn);
        } else {
            g_ftl.l2p.page[lpn] = PPN_INVALID;
        }
    }

    if (newest_open != PBN_INVALID) {
        g_ftl.active_pbn     = newest_open;
        g_ftl.active_off     = nand_block_write_pointer(newest_open);
        g_ftl.blk[newest_open].state = BLK_ACTIVE;
    } else {
        g_ftl.active_pbn = PBN_INVALID;
        g_ftl.active_off = 0u;
    }
    g_ftl.seq++;
}

/* ===========================================================================
 * Reconstruction for BLOCK and HYBRID mapping
 * ---------------------------------------------------------------------------
 * These schemes cannot be rolled forward page by page, because a physical
 * block's identity (which logical block it serves, and whether it is a data
 * block or a log block) is a property of the WHOLE block. So we rebuild by
 * classifying blocks, not by replaying pages.
 *
 * Two blocks can legitimately claim the same logical block -- that is what an
 * interrupted read-modify-write or an interrupted merge leaves behind. The one
 * with the higher maximum sequence number is the newer, and the older one is an
 * orphan that gets erased. Because the new block is only ever *published* into
 * the mapping table after it is complete (see mapping.c), the orphan is always
 * the incomplete one, and no data is lost by discarding it.
 * ========================================================================= */
static void recover_blockmap_scan(void)
{
    uint32_t     pbn, off, wp, ppn, lbn, i, j;
    nand_spare_t spare;
    int          slot;

    (void)mapping_init();
    blockmgr_init();

    memset(s_role, 0, sizeof(s_role));

    /* ---- Pass 1: what is each block? ---- */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        s_role[pbn]     = ROLE_EMPTY;
        s_role_lbn[pbn] = PBN_INVALID;
        s_role_seq[pbn] = 0u;

        if (nand_block_is_bad(pbn)) {
            continue;
        }
        wp = nand_block_write_pointer(pbn);
        if (wp == 0u) {
            continue;
        }

        for (off = 0u; off < wp; off++) {
            ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
            if (nand_read_spare(ppn, &spare) != NAND_OK) {
                continue;
            }
            g_ftl.stats.recovery_pages_scanned++;

            if (spare.seq > s_role_seq[pbn]) {
                s_role_seq[pbn] = spare.seq;
            }
            if (spare.seq > g_ftl.seq) {
                g_ftl.seq = spare.seq;
            }
            if (spare.lpn >= FTL_LOGICAL_PAGES) {
                continue;                   /* padding page */
            }
            if (s_role[pbn] == ROLE_EMPTY) {
                s_role[pbn]     = (spare.tag == NAND_TAG_LOG_DATA) ? ROLE_LOG : ROLE_DATA;
                s_role_lbn[pbn] = LPN_TO_LBN(spare.lpn);
            }
        }
        if (s_role[pbn] == ROLE_EMPTY) {
            s_role[pbn] = ROLE_DATA;        /* nothing but padding: an orphan */
        }
    }

    /* ---- Pass 2: newest data block per logical block wins ---- */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (s_role[pbn] != ROLE_DATA || s_role_lbn[pbn] == PBN_INVALID) {
            continue;
        }
        lbn = s_role_lbn[pbn];
        if (g_ftl.l2p.block[lbn] == PBN_INVALID ||
            s_role_seq[pbn] > s_role_seq[g_ftl.l2p.block[lbn]]) {
            g_ftl.l2p.block[lbn] = pbn;
        }
    }

    /* ---- Pass 3: publish the winners, rebuild their validity ---- */
    for (lbn = 0u; lbn < FTL_LOGICAL_BLOCKS; lbn++) {
        pbn = g_ftl.l2p.block[lbn];
        if (pbn == PBN_INVALID) {
            continue;
        }
        g_ftl.blk[pbn].state          = BLK_DATA;
        g_ftl.blk[pbn].owner_lbn      = lbn;
        g_ftl.blk[pbn].valid_pages    = 0u;
        g_ftl.blk[pbn].last_write_seq = s_role_seq[pbn];
        g_ftl.free_blocks--;

        wp = nand_block_write_pointer(pbn);
        for (off = 0u; off < wp; off++) {
            ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
            if (nand_read_spare(ppn, &spare) != NAND_OK) {
                continue;
            }
            /* Under block mapping a page is valid only if it sits at the offset
             * its own logical page number implies. Anything else is padding or
             * debris. */
            if (spare.tag == NAND_TAG_USER_DATA &&
                spare.lpn == lbn * NAND_PAGES_PER_BLOCK + off) {
                mapping_validate_ppn(ppn);
                g_ftl.stats.recovery_pages_applied++;
            }
        }
    }

    /* ---- Pass 4: attach log blocks (hybrid only) ---- */
    if (g_ftl.params.mapping == FTL_MAP_HYBRID) {
        slot = 0;
        for (pbn = FTL_DATA_BLOCK_FIRST;
             pbn < NAND_NUM_BLOCKS && slot < (int)FTL_LOG_BLOCK_POOL; pbn++) {
            if (s_role[pbn] != ROLE_LOG || s_role_lbn[pbn] == PBN_INVALID) {
                continue;
            }
            lbn = s_role_lbn[pbn];

            g_ftl.log[slot].pbn          = pbn;
            g_ftl.log[slot].lbn          = lbn;
            g_ftl.log[slot].used         = 0u;
            g_ftl.log[slot].last_use_seq = s_role_seq[pbn];
            for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
                g_ftl.log[slot].off_of_logpage[i] = -1;
                g_ftl.log[slot].logpage_of_off[i] = -1;
            }
            g_ftl.blk[pbn].state     = BLK_LOG;
            g_ftl.blk[pbn].owner_lbn = lbn;
            g_ftl.blk[pbn].valid_pages = 0u;
            g_ftl.free_blocks--;

            /* Replay the log block in program order. Later entries for the same
             * offset supersede earlier ones -- which is precisely the invariant
             * the runtime code maintains, so recovery and runtime agree by
             * construction rather than by coincidence. */
            wp = nand_block_write_pointer(pbn);
            for (off = 0u; off < wp; off++) {
                ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
                if (nand_read_spare(ppn, &spare) != NAND_OK) {
                    continue;
                }
                if (spare.tag != NAND_TAG_LOG_DATA || spare.lpn >= FTL_LOGICAL_PAGES) {
                    continue;
                }
                j = LPN_TO_LOFF(spare.lpn);

                if (g_ftl.log[slot].logpage_of_off[j] >= 0) {
                    mapping_invalidate_ppn(BLOCK_OFFSET_TO_PPN(
                        pbn, (uint32_t)g_ftl.log[slot].logpage_of_off[j]));
                }
                g_ftl.log[slot].off_of_logpage[off] = (int16_t)j;
                g_ftl.log[slot].logpage_of_off[j]   = (int16_t)off;
                mapping_validate_ppn(ppn);
                g_ftl.stats.recovery_pages_applied++;

                /* The log copy supersedes whatever the data block held. */
                if (g_ftl.l2p.block[lbn] != PBN_INVALID) {
                    mapping_invalidate_ppn(
                        BLOCK_OFFSET_TO_PPN(g_ftl.l2p.block[lbn], j));
                }
            }
            g_ftl.log[slot].used = (uint16_t)wp;
            slot++;
        }
    }

    /* ---- Pass 5: erase the orphans ---- */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_bad(pbn)) {
            g_ftl.blk[pbn].state = BLK_BAD;
            continue;
        }
        if (g_ftl.blk[pbn].state != BLK_FREE) {
            continue;
        }
        if (nand_block_write_pointer(pbn) == 0u) {
            continue;                    /* genuinely erased already */
        }
        /* Written, but nothing points at it: debris from an interrupted
         * read-modify-write or merge. Reclaim it. */
        g_ftl.free_blocks--;             /* blockmgr_release will add it back */
        g_ftl.blk[pbn].valid_pages = 0u;
        (void)blockmgr_release(pbn);
    }

    g_ftl.active_pbn = PBN_INVALID;
    g_ftl.active_off = 0u;
    g_ftl.seq++;
}

/* ===========================================================================
 * Full-scan reconstruction for PAGE mapping (checkpoint ignored)
 * ========================================================================= */
static void recover_pagemap_full_scan(void)
{
    (void)mapping_init();
    blockmgr_init();
    g_ftl.seq = 0u;
    recover_pagemap_rollforward(0u);
}

/* ===========================================================================
 * journal_recover -- the mount path.
 * ========================================================================= */
static ftl_err_t journal_recover_inner(ftl_recover_mode_t mode)
{
    ckpt_header_t hdr;
    ftl_params_t  wanted = g_ftl.params;
    ftl_stats_t   carried;

    memset(&hdr, 0, sizeof(hdr));

    /* Block and hybrid mapping always rebuild by scanning: a block's role is a
     * whole-block property, so there is nothing to roll forward page by page.
     * The checkpoint still earns its keep on those schemes by carrying the
     * statistics and the erase-count history forward -- but correctness does
     * not depend on it, which is a reassuring place to be. */
    if (wanted.mapping != FTL_MAP_PAGE) {
        recover_blockmap_scan();
        return FTL_OK;
    }

    if (mode == FTL_RECOVER_FULL_SCAN) {
        recover_pagemap_full_scan();
        return FTL_OK;
    }

    if (!ckpt_find_latest(&hdr)) {
        /* No usable checkpoint: fall back to the slow path rather than fail.
         * A drive that refuses to mount because its metadata is damaged, when
         * the user data is all sitting right there on the media, is a drive
         * that has thrown away the customer's files for no reason. */
        recover_pagemap_full_scan();
        return FTL_OK;
    }
    if (hdr.mapping_scheme != (uint32_t)wanted.mapping) {
        return FTL_ERR_CORRUPT;
    }

    carried = g_ftl.stats;
    if (ckpt_load(&hdr) != FTL_OK) {
        g_ftl.stats = carried;
        recover_pagemap_full_scan();
        return FTL_OK;
    }

    /* The checkpoint restored the params it was written with. Honour the
     * caller's request only where it cannot invalidate the snapshot. */
    g_ftl.params.gc                   = wanted.gc;
    g_ftl.params.wear                 = wanted.wear;
    g_ftl.params.checkpointing        = wanted.checkpointing;
    g_ftl.params.read_disturb_refresh = wanted.read_disturb_refresh;

    recover_pagemap_rollforward(hdr.ckpt_seq);
    return FTL_OK;
}

ftl_err_t journal_recover(ftl_recover_mode_t mode)
{
    ftl_err_t err = journal_recover_inner(mode);

    /* Wherever recovery took us, the metadata append point has to be re-derived
     * from the die before anything tries to write another checkpoint. */
    meta_resync();
    return err;
}
