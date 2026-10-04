/* =============================================================================
 * ftl.h -- The public interface of the Flash Translation Layer.
 * =============================================================================
 *
 * WHAT AN FTL IS, IN ONE SENTENCE
 * -------------------------------
 * The FTL is the piece of firmware that lets a host talk to flash as if it were
 * a disk -- "write 4 KiB to logical block 12345" -- despite flash being unable
 * to overwrite anything in place.
 *
 * The interface below is deliberately shaped like the storage commands a real
 * host issues, so that the mapping between this project and NVMe is obvious:
 *
 *      ftl_write()  <->  NVMe Write        (opcode 0x01)
 *      ftl_read()   <->  NVMe Read         (opcode 0x02)
 *      ftl_trim()   <->  NVMe Dataset Management / Deallocate (opcode 0x09)
 *      ftl_flush()  <->  NVMe Flush        (opcode 0x00)
 *
 * What this project does NOT implement is the transport: there is no PCIe link,
 * no submission/completion queue pair, no doorbell register. Those are covered
 * in DOCS/NVME_AND_PCIE.md, and the interface is drawn this way so the missing
 * piece is a thin shim rather than a redesign.
 * ========================================================================== */

#ifndef FTL_H
#define FTL_H

#include <stdint.h>
#include <stdbool.h>

#include "ftl_config.h"
#include "nand.h"

/* ---------------------------------------------------------------------------
 * ADDRESS MAPPING SCHEME
 * ---------------------------------------------------------------------------
 * This is the central design choice in any FTL, and it is a pure RAM-versus-
 * performance trade. See DOCS/CONCEPTS_EXPLAINED.md for the full derivation;
 * the short version, at the default 32 MiB geometry:
 *
 *   PAGE    one entry per logical PAGE.   5952 entries x 4 B = 23.2 KiB
 *           Any page can live anywhere. Random writes are as cheap as
 *           sequential ones. Costs the most RAM by a factor of PAGES_PER_BLOCK.
 *
 *   BLOCK   one entry per logical BLOCK.    93 entries x 4 B =  372 B
 *           64x less RAM. But a logical page's offset inside its block is
 *           FIXED, so overwriting one page in the middle of a block forces a
 *           read-modify-write of the entire block. Random-write write
 *           amplification approaches PAGES_PER_BLOCK.
 *
 *   HYBRID  block-mapped data blocks, plus a small pool of page-mapped "log"
 *           blocks that absorb updates and are periodically merged back.
 *           Nearly block-mapping's RAM cost with far better random-write
 *           behaviour -- the classic BAST/FAST family, and what real drives
 *           used before DRAM got cheap enough for full page mapping.
 *
 * Modern SSDs are page-mapped and keep the whole table in on-board DRAM at
 * roughly 1 MB of DRAM per 1 GB of flash. DRAM-less drives still use hybrid
 * schemes, or cache a slice of a page map in SRAM (a "demand-based" FTL).
 * ------------------------------------------------------------------------- */
typedef enum {
    FTL_MAP_PAGE = 0,
    FTL_MAP_BLOCK,
    FTL_MAP_HYBRID
} ftl_map_scheme_t;

/* ---------------------------------------------------------------------------
 * GARBAGE COLLECTION VICTIM-SELECTION POLICY
 * ---------------------------------------------------------------------------
 * Only meaningful for FTL_MAP_PAGE. Under block mapping, space is reclaimed
 * synchronously by the read-modify-write itself; under hybrid mapping it is
 * reclaimed by merges. That is not a shortcut -- it is how those designs work.
 *
 *   GREEDY        pick the block with the fewest valid pages. Minimises the
 *                 copy cost of THIS collection. Optimal for uniform-random
 *                 workloads, mediocre when the workload has hot and cold data,
 *                 because it will happily collect a cold block that would have
 *                 stayed valid anyway.
 *
 *   COST_BENEFIT  maximise  (1 - u) * age / (2u)  where u is the fraction of
 *                 pages still valid and `age` is time since the block was last
 *                 written. Numerator: space freed, times how long the block has
 *                 sat still (a long-idle block is cold, so the space we free
 *                 will STAY free). Denominator: 2u, because collecting costs u
 *                 page reads plus u page writes. Introduced by the Sprite LFS
 *                 cleaner and adopted almost verbatim by flash firmware.
 *
 *   FIFO          collect the block written longest ago. Costs nothing to
 *                 compute; included as a baseline.
 *
 *   RANDOM        the control group. If your clever policy cannot beat this,
 *                 it is not clever.
 * ------------------------------------------------------------------------- */
typedef enum {
    FTL_GC_GREEDY = 0,
    FTL_GC_COST_BENEFIT,
    FTL_GC_FIFO,
    FTL_GC_RANDOM
} ftl_gc_policy_t;

/* ---------------------------------------------------------------------------
 * WEAR LEVELLING POLICY
 * ---------------------------------------------------------------------------
 *   NONE     allocate the first free block found. Included to demonstrate how
 *            badly erase counts diverge without any levelling at all.
 *   DYNAMIC  always allocate the free block with the lowest erase count. Free,
 *            because we have to choose a block anyway. Levels the blocks that
 *            are in circulation -- but a block full of cold data never becomes
 *            free, so it never circulates and never wears.
 *   STATIC   dynamic, plus: when (max erase count - min erase count) exceeds
 *            FTL_WEAR_GAP_THRESHOLD, forcibly relocate the contents of the
 *            least-erased block so it joins the pool. Costs extra writes; it is
 *            the only thing that levels cold data.
 * ------------------------------------------------------------------------- */
typedef enum {
    FTL_WEAR_NONE = 0,
    FTL_WEAR_DYNAMIC,
    FTL_WEAR_STATIC
} ftl_wear_policy_t;

/* ---------------------------------------------------------------------------
 * Recovery strategy used by ftl_mount().
 * ------------------------------------------------------------------------- */
typedef enum {
    /* Load the newest valid checkpoint, then scan the data region for pages
     * newer than it and roll them forward. This is what a product does: the
     * checkpoint bounds how much media you have to scan, so mount time stays
     * roughly constant instead of growing with capacity. */
    FTL_RECOVER_CHECKPOINT = 0,
    /* Ignore checkpoints entirely and rebuild the whole mapping table from the
     * spare areas of every programmed page. Slow -- it is O(capacity) -- but it
     * proves the media is fully self-describing, and it is the last-resort path
     * every real product keeps for when the metadata region is unreadable. */
    FTL_RECOVER_FULL_SCAN
} ftl_recover_mode_t;

/* ---------------------------------------------------------------------------
 * Configuration handed to ftl_format() / ftl_mount().
 * ------------------------------------------------------------------------- */
typedef struct {
    ftl_map_scheme_t   mapping;
    ftl_gc_policy_t    gc;
    ftl_wear_policy_t  wear;
    bool               checkpointing;   /* write periodic metadata checkpoints */
    bool               read_disturb_refresh;
    uint64_t           seed;            /* seeds the die and any FTL randomness */
} ftl_params_t;

void ftl_params_defaults(ftl_params_t *p);

/* ---------------------------------------------------------------------------
 * Return codes
 * ------------------------------------------------------------------------- */
typedef enum {
    FTL_OK = 0,
    FTL_ERR_RANGE,        /* LPN outside the exposed capacity                 */
    FTL_ERR_UNWRITTEN,    /* read of an LPN that was never written or was TRIMmed */
    FTL_ERR_NO_SPACE,     /* out of usable blocks -- the drive is dead        */
    FTL_ERR_MEDIA,        /* an unrecoverable media error escaped retry       */
    FTL_ERR_CORRUPT,      /* metadata failed its integrity check              */
    FTL_ERR_UNSUPPORTED
} ftl_err_t;

const char *ftl_strerror(ftl_err_t e);

/* ---------------------------------------------------------------------------
 * Statistics. Every number the project reports comes from here.
 *
 * The write-amplification breakdown is split by CAUSE rather than reported as
 * one lump, because "WA is 3.4" tells you nothing actionable, while "WA is 3.4
 * and 2.1 of it is garbage collection" tells you to go look at over-
 * provisioning and victim selection.
 * ------------------------------------------------------------------------- */
typedef struct {
    /* Host-visible work */
    uint64_t host_writes;
    uint64_t host_reads;
    uint64_t host_trims;

    /* Physical page programs, attributed to their cause. These must sum to the
     * die's own program counter -- ftl_stats_selfcheck() asserts exactly that,
     * which is how we know the accounting is not quietly wrong. */
    uint64_t pgm_host;       /* the host's own data                            */
    uint64_t pgm_gc;         /* valid pages relocated by garbage collection    */
    uint64_t pgm_merge;      /* pages rewritten by hybrid-mapping merges       */
    uint64_t pgm_rmw;        /* pages rewritten by block-mapping read-modify-write */
    uint64_t pgm_pad;        /* filler pages block mapping must program to keep
                              * the sequential-programming rule               */
    uint64_t pgm_wear;       /* pages moved by static wear levelling           */
    uint64_t pgm_refresh;    /* pages moved because of read disturb            */
    uint64_t pgm_recover;    /* pages rescued off a block that went bad        */
    uint64_t pgm_meta;       /* checkpoints                                    */

    /* Block-level work */
    uint64_t erases;
    uint64_t gc_invocations;
    uint64_t gc_victims;
    uint64_t switch_merges;
    uint64_t partial_merges;
    uint64_t full_merges;
    uint64_t wear_migrations;
    uint64_t refresh_events;
    uint64_t blocks_retired;      /* grown bad blocks handled                  */
    uint64_t program_retries;     /* writes re-issued after a media failure    */
    uint64_t pages_lost;          /* pages an ECC error made unrecoverable     */

    /* Recovery */
    uint64_t checkpoints_written;
    uint64_t recovery_pages_scanned;
    uint64_t recovery_pages_applied;
} ftl_stats_t;

const ftl_stats_t *ftl_stats(void);

/* Zero the FTL counters and the die's counters together.
 *
 * This exists for one specific and important reason: PRECONDITIONING. Write
 * amplification measured on a freshly formatted drive is meaningless -- every
 * write goes to an erased block, nothing has to be collected, and WA reads as
 * 1.0 no matter how bad the design is. The industry-standard method (SNIA's
 * solid-state performance test spec says the same) is to fill the drive first,
 * then reset the counters, then measure the STEADY STATE. That is exactly what
 * the benchmark driver does, and this is the function that makes the second
 * step possible. Resetting both counter sets together keeps
 * ftl_stats_selfcheck() valid across the reset. */
void ftl_stats_reset(void);

/* Total physical page programs the FTL believes it caused. */
uint64_t ftl_total_page_programs(const ftl_stats_t *s);

/* Write amplification = physical page programs / host page writes.
 * `include_metadata` decides whether checkpoint writes are counted. Vendors
 * quote the number without them; a firmware engineer should know both. */
double ftl_write_amplification(bool include_metadata);

/* Erase-count distribution across all usable blocks. Any pointer may be NULL. */
void ftl_erase_distribution(uint32_t *min_out, uint32_t *max_out,
                            double *mean_out, double *stddev_out);

/* Bytes of controller SRAM/DRAM the mapping structures cost under the CURRENT
 * scheme. This is the number that decides whether a design ships. */
size_t ftl_mapping_table_bytes(void);

/* Total size of the FTL's entire in-RAM state. On a real controller this is the
 * number the linker would report for the firmware's .bss, and it is the number
 * that decides whether the design fits the silicon. */
size_t ftl_context_bytes(void);

/* Verifies that the per-cause program counters add up to what the die actually
 * saw. Returns true if the books balance. Called by the tests and at the end of
 * every benchmark run. */
bool ftl_stats_selfcheck(void);

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Manufacture a fresh die AND a fresh FTL on top of it. Destroys everything. */
ftl_err_t ftl_format(const ftl_params_t *params);

/* Bring up the FTL over media that already contains data -- i.e. boot. Throws
 * away all in-RAM state first, so calling nand_power_cycle() then ftl_mount()
 * is a faithful model of pulling the plug and switching the drive back on. */
ftl_err_t ftl_mount(const ftl_params_t *params, ftl_recover_mode_t mode);

/* Push all metadata to the media so that the next mount is cheap. Corresponds
 * to NVMe Flush, and to what a well-behaved drive does when the host signals
 * an orderly shutdown. */
ftl_err_t ftl_flush(void);

/* ---------------------------------------------------------------------------
 * Data path. `buf` is always exactly NAND_PAGE_SIZE_BYTES.
 * ------------------------------------------------------------------------- */
ftl_err_t ftl_write(uint32_t lpn, const void *buf);
ftl_err_t ftl_read(uint32_t lpn, void *buf);

/* TRIM / deallocate. The host promising it will never read this LPN again is
 * worth a surprising amount: the FTL can immediately mark the physical page
 * invalid, which means garbage collection will not waste a copy preserving
 * data nobody wants. A drive that is never TRIMmed slowly fills with garbage
 * it is contractually obliged to keep. */
ftl_err_t ftl_trim(uint32_t lpn);

/* ---------------------------------------------------------------------------
 * Introspection, for tests and reporting.
 * ------------------------------------------------------------------------- */
uint32_t ftl_free_block_count(void);
uint32_t ftl_bad_block_count(void);
uint32_t ftl_lookup(uint32_t lpn);          /* current PPN, or PPN_INVALID */
const char *ftl_map_scheme_name(ftl_map_scheme_t s);
const char *ftl_gc_policy_name(ftl_gc_policy_t p);
const char *ftl_wear_policy_name(ftl_wear_policy_t p);

/* Walks every invariant the design relies on and returns false if any is
 * broken (valid-page counts matching the bitmap, no LPN mapped to an erased
 * page, no two LPNs sharing a PPN, free blocks genuinely erased, and so on).
 * Cheap enough to call from tests after every few hundred operations. */
bool ftl_check_invariants(void);

#endif /* FTL_H */
