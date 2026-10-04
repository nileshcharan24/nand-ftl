/* =============================================================================
 * ftl_internal.h -- State shared between the FTL's modules.
 * =============================================================================
 *
 * This header is NOT part of the public interface. It exists because the FTL is
 * one cooperating system split across several .c files (mapping, garbage
 * collection, wear levelling, bad-block handling, journaling), and they all
 * operate on the same tables.
 *
 * WHY ONE GLOBAL CONTEXT STRUCT AND NOT A HANDLE PASSED AROUND?
 * -------------------------------------------------------------
 * A product with several NAND channels would want a handle per channel. This
 * one models a single die, and a single global has three concrete advantages
 * that matter here:
 *
 *   1. It is one contiguous object, so `sizeof(ftl_ctx_t)` printed at start-up
 *      IS the firmware's RAM budget for state. No hunting through the map file.
 *   2. It can be checkpointed to flash with one memcpy of a known extent.
 *   3. It lives in .bss, so the linker -- not the runtime -- tells you if it
 *      does not fit. That is exactly the failure mode you want on a controller
 *      with 256 KiB of SRAM.
 * ========================================================================== */

#ifndef FTL_INTERNAL_H
#define FTL_INTERNAL_H

#include "ftl.h"

/* ---------------------------------------------------------------------------
 * What a physical block is currently being used for.
 * ------------------------------------------------------------------------- */
typedef enum {
    BLK_FREE = 0,   /* erased and in the free pool, ready to be allocated     */
    BLK_ACTIVE,     /* the block we are currently appending host writes to    */
    BLK_DATA,       /* holds data; full, or block-mapped and in service       */
    BLK_LOG,        /* hybrid mapping: a log block absorbing updates          */
    BLK_META,       /* metadata region: checkpoints                           */
    BLK_BAD         /* retired, never to be used again                        */
} blk_state_t;

/* ---------------------------------------------------------------------------
 * Per-physical-block bookkeeping held in controller RAM.
 *
 * Erase counts and bad-block marks are deliberately NOT duplicated here -- they
 * are read from the die through nand_block_erase_count() / nand_block_is_bad().
 * Keeping one authoritative copy of a fact is worth more than saving the
 * function call: two copies of the erase count is two things to get out of sync
 * across a power cut.
 *
 * 12 bytes per block. At the default 128 blocks that is 1.5 KiB. Worth keeping
 * in mind how that scales: a 1 TB drive has ~256k blocks, so the same table
 * would be 3 MB, which is why real firmware compresses this hard (bitfields,
 * and valid counts stored as 16-bit even when blocks hold 1024 pages).
 * ------------------------------------------------------------------------- */
typedef struct {
    uint16_t valid_pages;     /* pages in this block still reachable via the map */
    uint8_t  state;           /* blk_state_t                                     */
    uint8_t  pad;
    uint32_t owner_lbn;       /* block/hybrid mapping: which LBN this serves     */
    uint32_t last_write_seq;  /* global seq when this block was last programmed;
                               * cost-benefit GC uses (now - this) as "age"      */
} blk_meta_t;

/* ---------------------------------------------------------------------------
 * HYBRID MAPPING: one log block.
 * ---------------------------------------------------------------------------
 * A log block is a physical block that temporarily absorbs updates aimed at one
 * logical block. Inside it, pages are page-mapped (they can hold any offset in
 * any order); the data block behind it stays block-mapped.
 *
 * Two little arrays make both directions O(1):
 *
 *   off_of_logpage[i]  = which logical offset the i-th page of this log block
 *                        holds. This is the REVERSE map, and it is what merges
 *                        need. It is also redundantly stored in the page's
 *                        spare area, so it survives power loss.
 *
 *   logpage_of_off[o]  = the NEWEST log page holding logical offset o, or -1.
 *                        This is the FORWARD map, and it is what reads need.
 *                        Purely a RAM accelerator -- it is rebuilt from
 *                        off_of_logpage at mount.
 *
 * This is the BAST design (Block-Associative Sector Translation): at most one
 * log block per data block. Its weakness is exactly that associativity -- a
 * workload touching more distinct logical blocks than there are log blocks
 * causes a merge per write ("log block thrashing"). FAST fixes it with fully
 * associative log blocks at the cost of much more expensive merges. Naming that
 * trade-off is a good thing to be able to do out loud.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t pbn;                                   /* PBN_INVALID if slot free */
    uint32_t lbn;                                   /* logical block served     */
    uint16_t used;                                  /* log pages programmed     */
    uint32_t last_use_seq;                          /* for LRU victim choice    */
    int16_t  off_of_logpage[NAND_PAGES_PER_BLOCK];  /* reverse map              */
    int16_t  logpage_of_off[NAND_PAGES_PER_BLOCK];  /* forward map, -1 = absent */
} log_block_t;

/* ---------------------------------------------------------------------------
 * THE LOGICAL-TO-PHYSICAL TABLE.
 * ---------------------------------------------------------------------------
 * A union, not a struct, and that is the point: a real product picks ONE
 * mapping scheme and pays for exactly one table. Writing it as a union means
 * sizeof(ftl_ctx_t) reflects the worst case rather than the sum, and it makes
 * it impossible to accidentally read the block table while running page-mapped.
 * ------------------------------------------------------------------------- */
typedef union {
    uint32_t page[FTL_LOGICAL_PAGES];    /* LPN -> PPN   (FTL_MAP_PAGE)        */
    uint32_t block[FTL_LOGICAL_BLOCKS];  /* LBN -> PBN   (BLOCK and HYBRID)    */
} l2p_table_t;

/* ---------------------------------------------------------------------------
 * Bitmap helpers for the page-validity bitmap.
 * One bit per physical page: 8192 pages -> 1 KiB, versus 8 KiB for a byte
 * array. On a controller that difference is real money.
 * ------------------------------------------------------------------------- */
#define BITMAP_WORDS(n)      (((n) + 31u) / 32u)
#define BITMAP_SET(a, i)     ((a)[(i) >> 5] |=  (1u << ((i) & 31u)))
#define BITMAP_CLR(a, i)     ((a)[(i) >> 5] &= ~(1u << ((i) & 31u)))
#define BITMAP_GET(a, i)     (((a)[(i) >> 5] >> ((i) & 31u)) & 1u)

/* ---------------------------------------------------------------------------
 * THE CONTEXT
 * ------------------------------------------------------------------------- */
typedef struct {
    ftl_params_t params;
    bool         mounted;

    /* --- address translation --- */
    l2p_table_t  l2p;
    log_block_t  log[FTL_LOG_BLOCK_POOL];

    /* --- physical block bookkeeping --- */
    blk_meta_t   blk[NAND_NUM_BLOCKS];
    uint32_t     page_valid[BITMAP_WORDS(NAND_TOTAL_PAGES)];
    uint32_t     free_blocks;

    /* --- the write frontier (page mapping) ---
     * Host writes append here. When the block fills we retire it to BLK_DATA
     * and allocate a new one. Keeping a single open block is what makes
     * out-of-place updates sequential from the die's point of view, which is
     * what keeps program latency predictable. */
    uint32_t     active_pbn;
    uint32_t     active_off;

    /* --- global sequence number ---
     * Stamped into every programmed page's spare area. Monotonic, never reset,
     * and the sole tie-breaker used by recovery to decide which of two copies
     * of an LPN is the newer one. */
    uint32_t     seq;

    /* --- metadata region --- */
    uint32_t     meta_pbn;        /* metadata block currently being appended  */
    uint32_t     meta_off;
    uint32_t     ckpt_seq;        /* seq stamped on the last checkpoint       */
    uint64_t     writes_since_ckpt;

    /* --- randomness for the RANDOM gc policy (kept separate from the die's) --- */
    uint64_t     rng;

    ftl_stats_t  stats;
} ftl_ctx_t;

/* The one instance. Defined in ftl.c. */
extern ftl_ctx_t g_ftl;

/* ===========================================================================
 * Cross-module internal API.
 * Each group is implemented in the .c file named in the comment.
 * ========================================================================= */

/* ---- blockmgr.c : the free-block pool and allocation policy ------------- */
void      blockmgr_init(void);
uint32_t  blockmgr_alloc(void);              /* PBN_INVALID when exhausted   */
ftl_err_t blockmgr_release(uint32_t pbn);    /* erase and return to the pool */
void      blockmgr_open_active_block(void);  /* ensure a write frontier exists*/
uint32_t  blockmgr_next_free_ppn(void);      /* frontier page; PPN_INVALID if none */

/* ---- badblock.c : retiring blocks that fail ---------------------------- */
ftl_err_t badblock_retire(uint32_t pbn, const char *reason);
uint32_t  badblock_count(void);

/* ---- mapping.c : the three translation schemes ------------------------- */
ftl_err_t mapping_init(void);
ftl_err_t mapping_write(uint32_t lpn, const void *buf);
ftl_err_t mapping_read(uint32_t lpn, void *buf);
ftl_err_t mapping_trim(uint32_t lpn);
uint32_t  mapping_lookup(uint32_t lpn);
/* Program one page at the frontier with full retry-on-media-failure handling.
 * Shared by the host write path, GC, merges and wear levelling, so that every
 * one of them handles a failed program identically. */
ftl_err_t mapping_program_at_frontier(uint32_t lpn, const void *buf,
                                      uint32_t tag, uint32_t *ppn_out);
void      mapping_invalidate_ppn(uint32_t ppn);
void      mapping_validate_ppn(uint32_t ppn);
/* Move everything a block still holds somewhere else, leaving it with zero
 * valid pages. Scheme-aware: under page mapping the pages go to the write
 * frontier, under block/hybrid mapping the block is copied wholesale into a
 * replacement so that page offsets are preserved.
 *
 * This one routine is shared by all three callers that need to empty a block --
 * static wear levelling, read-disturb refresh, and retiring a block that failed
 * a program. Writing it once is not just DRY: it is the only way to be sure all
 * three of them handle the block-mapped case identically. */
ftl_err_t mapping_relocate_block(uint32_t pbn, uint64_t *program_bucket);

/* ---- gc.c : reclaiming space ------------------------------------------ */
ftl_err_t gc_maybe_run(void);        /* called before every host write       */
ftl_err_t gc_force_once(void);       /* collect exactly one block; for tests */
uint32_t  gc_select_victim(void);    /* exposed so the tests can check policy */

/* ---- wear.c : levelling erase counts ----------------------------------- */
ftl_err_t wear_maybe_level(void);
uint32_t  wear_pick_free_block(void);     /* allocation policy hook          */
ftl_err_t wear_relocate_block(uint32_t pbn, uint64_t *pages_moved);

/* ---- journal.c : checkpoints and power-loss recovery ------------------- */
void      journal_init(void);
ftl_err_t journal_checkpoint(void);
ftl_err_t journal_recover(ftl_recover_mode_t mode);

/* ---- shared helper ----------------------------------------------------- */
uint64_t  ftl_rng_next(void);

#endif /* FTL_INTERNAL_H */
