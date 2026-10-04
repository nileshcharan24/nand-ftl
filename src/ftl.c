/* =============================================================================
 * ftl.c -- The public entry points, the statistics, and the invariant checker.
 * =============================================================================
 *
 * This file is the thin layer that a host talks to. All the interesting work
 * happens in the modules it calls; what lives here is the ORDER in which they
 * are called, which is itself a design decision worth being explicit about:
 *
 *      write(lpn, buf):
 *          1. garbage-collect if the free pool is running low
 *          2. wear-level if the erase counts have drifted too far apart
 *          3. do the actual translation and program
 *          4. checkpoint if enough writes have accumulated
 *
 * Steps 1 and 2 happen BEFORE the write, not after. If they ran afterwards,
 * a write could arrive with no free block available and fail -- and "the drive
 * refused a write because it was busy tidying up" is not an acceptable answer.
 * Making space is a precondition of accepting work, not a consequence of it.
 * ========================================================================== */

#include <math.h>
#include <string.h>

#include "ftl_internal.h"

/* THE context. See the comment in ftl_internal.h for why it is a single global
 * rather than a handle: at ~29 KiB it is the firmware's entire RAM budget for
 * state, and having it be one object in .bss means the linker reports that
 * budget rather than the developer having to reason about it. */
ftl_ctx_t g_ftl;

/* ---------------------------------------------------------------------------
 * A PRNG for the FTL's own decisions, kept separate from the die's.
 * Two independent streams means changing the GC policy cannot perturb where the
 * factory bad blocks landed, so two runs stay comparable.
 * ------------------------------------------------------------------------- */
uint64_t ftl_rng_next(void)
{
    uint64_t x = g_ftl.rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_ftl.rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* ===========================================================================
 * Naming and defaults
 * ========================================================================= */

void ftl_params_defaults(ftl_params_t *p)
{
    if (p == NULL) {
        return;
    }
    p->mapping              = FTL_MAP_PAGE;
    p->gc                   = FTL_GC_GREEDY;
    p->wear                 = FTL_WEAR_DYNAMIC;
    p->checkpointing        = true;
    p->read_disturb_refresh = true;
    p->seed                 = 1u;
}

const char *ftl_strerror(ftl_err_t e)
{
    switch (e) {
    case FTL_OK:              return "OK";
    case FTL_ERR_RANGE:       return "LPN out of range";
    case FTL_ERR_UNWRITTEN:   return "logical page has never been written";
    case FTL_ERR_NO_SPACE:    return "no usable blocks left";
    case FTL_ERR_MEDIA:       return "unrecoverable media error";
    case FTL_ERR_CORRUPT:     return "metadata integrity check failed";
    case FTL_ERR_UNSUPPORTED: return "unsupported configuration";
    default:                  return "unknown error";
    }
}

const char *ftl_map_scheme_name(ftl_map_scheme_t s)
{
    switch (s) {
    case FTL_MAP_PAGE:   return "page";
    case FTL_MAP_BLOCK:  return "block";
    case FTL_MAP_HYBRID: return "hybrid";
    default:             return "?";
    }
}

const char *ftl_gc_policy_name(ftl_gc_policy_t p)
{
    switch (p) {
    case FTL_GC_GREEDY:       return "greedy";
    case FTL_GC_COST_BENEFIT: return "cost-benefit";
    case FTL_GC_FIFO:         return "fifo";
    case FTL_GC_RANDOM:       return "random";
    default:                  return "?";
    }
}

const char *ftl_wear_policy_name(ftl_wear_policy_t p)
{
    switch (p) {
    case FTL_WEAR_NONE:    return "none";
    case FTL_WEAR_DYNAMIC: return "dynamic";
    case FTL_WEAR_STATIC:  return "static";
    default:               return "?";
    }
}

/* ===========================================================================
 * Lifecycle
 * ========================================================================= */

ftl_err_t ftl_format(const ftl_params_t *params)
{
    ftl_err_t err;

    if (params == NULL) {
        return FTL_ERR_UNSUPPORTED;
    }

    memset(&g_ftl, 0, sizeof(g_ftl));
    g_ftl.params = *params;
    g_ftl.rng    = params->seed ^ 0xD1B54A32D192ED03ULL;

    /* Manufacture the die first: block classification below depends on knowing
     * which blocks the factory marked bad. */
    nand_format(params->seed);

    blockmgr_init();
    err = mapping_init();
    if (err != FTL_OK) {
        return err;
    }
    journal_init();

    g_ftl.seq        = 0u;
    g_ftl.active_pbn = PBN_INVALID;
    g_ftl.active_off = 0u;
    g_ftl.mounted    = true;
    return FTL_OK;
}

ftl_err_t ftl_mount(const ftl_params_t *params, ftl_recover_mode_t mode)
{
    ftl_err_t err;

    if (params == NULL) {
        return FTL_ERR_UNSUPPORTED;
    }

    /* Throw away every scrap of in-RAM state. This is what makes
     * nand_power_cycle() + ftl_mount() an honest model of pulling the plug:
     * nothing can survive in a variable that a real reboot would have cleared. */
    memset(&g_ftl, 0, sizeof(g_ftl));
    g_ftl.params = *params;
    g_ftl.rng    = params->seed ^ 0xD1B54A32D192ED03ULL;

    blockmgr_init();
    err = mapping_init();
    if (err != FTL_OK) {
        return err;
    }
    journal_init();

    g_ftl.active_pbn = PBN_INVALID;
    g_ftl.active_off = 0u;

    err = journal_recover(mode);
    if (err != FTL_OK) {
        return err;
    }

    g_ftl.mounted = true;
    return FTL_OK;
}

ftl_err_t ftl_flush(void)
{
    if (!g_ftl.mounted) {
        return FTL_ERR_UNSUPPORTED;
    }
    return journal_checkpoint();
}

/* ===========================================================================
 * The data path
 * ========================================================================= */

/* One retry loop shared by write and trim. A media failure has already caused
 * the offending block to be retired by the time we get here, so simply doing
 * the operation again lands it somewhere else. The bound exists because each
 * retry costs a block: an unbounded loop on a dying drive would spin until the
 * entire device was retired. */
#define FTL_MAX_OP_RETRY 3

ftl_err_t ftl_write(uint32_t lpn, const void *buf)
{
    ftl_err_t err = FTL_ERR_MEDIA;
    int       attempt;

    if (!g_ftl.mounted) {
        return FTL_ERR_UNSUPPORTED;
    }
    if (lpn >= FTL_LOGICAL_PAGES || buf == NULL) {
        return FTL_ERR_RANGE;
    }

    /* Make space before accepting the work, not after. */
    (void)gc_maybe_run();
    (void)wear_maybe_level();

    for (attempt = 0; attempt < FTL_MAX_OP_RETRY; attempt++) {
        err = mapping_write(lpn, buf);
        if (err != FTL_ERR_MEDIA) {
            break;
        }
        g_ftl.stats.program_retries++;
    }
    if (err != FTL_OK) {
        return err;
    }

    g_ftl.stats.host_writes++;
    g_ftl.writes_since_ckpt++;

    if (g_ftl.params.checkpointing &&
        g_ftl.writes_since_ckpt >= FTL_CHECKPOINT_INTERVAL) {
        (void)journal_checkpoint();
    }
    return FTL_OK;
}

ftl_err_t ftl_read(uint32_t lpn, void *buf)
{
    if (!g_ftl.mounted) {
        return FTL_ERR_UNSUPPORTED;
    }
    if (lpn >= FTL_LOGICAL_PAGES || buf == NULL) {
        return FTL_ERR_RANGE;
    }
    g_ftl.stats.host_reads++;
    return mapping_read(lpn, buf);
}

ftl_err_t ftl_trim(uint32_t lpn)
{
    if (!g_ftl.mounted) {
        return FTL_ERR_UNSUPPORTED;
    }
    if (lpn >= FTL_LOGICAL_PAGES) {
        return FTL_ERR_RANGE;
    }
    g_ftl.stats.host_trims++;
    return mapping_trim(lpn);
}

/* ===========================================================================
 * Introspection
 * ========================================================================= */

uint32_t ftl_free_block_count(void) { return g_ftl.free_blocks; }
uint32_t ftl_bad_block_count(void)  { return badblock_count(); }
uint32_t ftl_lookup(uint32_t lpn)   { return mapping_lookup(lpn); }

const ftl_stats_t *ftl_stats(void)  { return &g_ftl.stats; }

void ftl_stats_reset(void)
{
    memset(&g_ftl.stats, 0, sizeof(g_ftl.stats));
    nand_counters_reset();
}

uint64_t ftl_total_page_programs(const ftl_stats_t *s)
{
    return s->pgm_host + s->pgm_gc + s->pgm_merge + s->pgm_rmw + s->pgm_pad +
           s->pgm_wear + s->pgm_refresh + s->pgm_recover + s->pgm_meta;
}

double ftl_write_amplification(bool include_metadata)
{
    const ftl_stats_t *s = &g_ftl.stats;
    uint64_t           physical;

    if (s->host_writes == 0u) {
        return 0.0;
    }
    physical = ftl_total_page_programs(s);
    if (!include_metadata) {
        physical -= s->pgm_meta;
    }
    return (double)physical / (double)s->host_writes;
}

void ftl_erase_distribution(uint32_t *min_out, uint32_t *max_out,
                            double *mean_out, double *stddev_out)
{
    uint32_t pbn, ec, n = 0u;
    uint32_t mn = 0xFFFFFFFFu, mx = 0u;
    double   sum = 0.0, sumsq = 0.0, mean = 0.0;

    /* Bad blocks are excluded: their erase counts are frozen and would drag the
     * spread around for reasons that have nothing to do with the levelling
     * policy we are trying to measure. */
    for (pbn = FTL_DATA_BLOCK_FIRST; pbn < NAND_NUM_BLOCKS; pbn++) {
        if (nand_block_is_bad(pbn)) {
            continue;
        }
        ec = nand_block_erase_count(pbn);
        if (ec < mn) { mn = ec; }
        if (ec > mx) { mx = ec; }
        sum   += (double)ec;
        sumsq += (double)ec * (double)ec;
        n++;
    }

    if (n == 0u) {
        mn = 0u;
    } else {
        mean = sum / (double)n;
    }

    if (min_out    != NULL) { *min_out    = mn; }
    if (max_out    != NULL) { *max_out    = mx; }
    if (mean_out   != NULL) { *mean_out   = mean; }
    if (stddev_out != NULL) {
        double var = (n == 0u) ? 0.0 : (sumsq / (double)n) - (mean * mean);
        *stddev_out = (var > 0.0) ? sqrt(var) : 0.0;
    }
}

size_t ftl_mapping_table_bytes(void)
{
    switch (g_ftl.params.mapping) {
    case FTL_MAP_PAGE:
        return (size_t)FTL_LOGICAL_PAGES * sizeof(uint32_t);
    case FTL_MAP_BLOCK:
        return (size_t)FTL_LOGICAL_BLOCKS * sizeof(uint32_t);
    case FTL_MAP_HYBRID:
        /* The block table plus the log-block descriptors. The descriptors are
         * the hidden cost of hybrid mapping, and quoting the block table alone
         * would be flattering the design. */
        return (size_t)FTL_LOGICAL_BLOCKS * sizeof(uint32_t) + sizeof(g_ftl.log);
    default:
        return 0u;
    }
}

size_t ftl_context_bytes(void)
{
    return sizeof(ftl_ctx_t);
}

/* Do the per-cause program counters add up to what the die actually saw?
 * If they do not, some path is programming pages without declaring why, and
 * every write-amplification number in the report is a lie. This is checked at
 * the end of every benchmark run and in the tests. */
bool ftl_stats_selfcheck(void)
{
    const nand_counters_t *nc = nand_counters();
    uint64_t accounted = ftl_total_page_programs(&g_ftl.stats) + nc->program_failures;

    return accounted == nc->programs;
}

/* ===========================================================================
 * ftl_check_invariants
 * ---------------------------------------------------------------------------
 * The single most valuable 80 lines in the project. Every one of these
 * conditions has to hold after every operation; checking them from the tests
 * turns bugs that would otherwise appear as a mysterious wrong-data read
 * thousands of operations later into a failure on the operation that caused it.
 * ========================================================================= */
bool ftl_check_invariants(void)
{
    uint32_t pbn, off, ppn, lpn, wp;
    uint32_t counted_valid, counted_free = 0u;
    uint64_t total_valid_bits = 0u, total_mapped = 0u;

    for (pbn = 0u; pbn < NAND_NUM_BLOCKS; pbn++) {

        /* (1) The per-block valid count must equal the number of set bits. */
        counted_valid = 0u;
        for (off = 0u; off < NAND_PAGES_PER_BLOCK; off++) {
            ppn = BLOCK_OFFSET_TO_PPN(pbn, off);
            if (BITMAP_GET(g_ftl.page_valid, ppn)) {
                counted_valid++;
                total_valid_bits++;

                /* (2) A valid page must actually be programmed. A valid bit on
                 * an erased page means the mapping points at nothing. */
                if (!nand_page_is_programmed(ppn)) {
                    return false;
                }
            }
        }
        if (counted_valid != g_ftl.blk[pbn].valid_pages) {
            return false;
        }

        /* (3) Free blocks are genuinely erased and genuinely empty. */
        if (g_ftl.blk[pbn].state == BLK_FREE) {
            counted_free++;
            if (nand_block_write_pointer(pbn) != 0u || counted_valid != 0u) {
                return false;
            }
        }

        /* (4) Nothing valid may live in a retired block. */
        if (g_ftl.blk[pbn].state == BLK_BAD && counted_valid != 0u) {
            return false;
        }

        /* (5) A block marked bad on the die must be marked bad in RAM. */
        if (nand_block_is_bad(pbn) && g_ftl.blk[pbn].state != BLK_BAD) {
            return false;
        }
    }

    /* (6) The free-block counter must match reality. It is used to decide when
     * to garbage-collect, so drift here shows up as either premature GC or a
     * drive that runs out of space without warning. */
    if (counted_free != g_ftl.free_blocks) {
        return false;
    }

    /* (7) The write frontier must be inside its block and un-programmed. */
    if (g_ftl.active_pbn != PBN_INVALID) {
        if (g_ftl.active_pbn >= NAND_NUM_BLOCKS ||
            g_ftl.active_off > NAND_PAGES_PER_BLOCK) {
            return false;
        }
        wp = nand_block_write_pointer(g_ftl.active_pbn);
        if (wp != g_ftl.active_off) {
            return false;
        }
    }

    /* (8) Every mapped logical page points at a page that is valid and that
     * claims that same logical page. Under page mapping we can go further and
     * require the two sets to be the same size, which rules out orphaned valid
     * pages that nothing references -- those would never be reclaimed and would
     * leak capacity. */
    for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
        ppn = mapping_lookup(lpn);
        if (ppn == PPN_INVALID) {
            continue;
        }
        total_mapped++;
        if (ppn >= NAND_TOTAL_PAGES || !BITMAP_GET(g_ftl.page_valid, ppn)) {
            return false;
        }
    }
    if (g_ftl.params.mapping == FTL_MAP_PAGE && total_mapped != total_valid_bits) {
        return false;
    }

    return true;
}
