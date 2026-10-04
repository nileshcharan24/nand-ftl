/* =============================================================================
 * nand.c -- Implementation of the simulated NAND die.
 * =============================================================================
 * See nand.h for the contract this file implements and *why* each restriction
 * exists. This file is about how the simulation is built.
 *
 * MEMORY BUDGET (the numbers that matter in an embedded review)
 * -------------------------------------------------------------
 * With the default geometry (128 blocks x 64 pages x 4096 B):
 *
 *   g_page_data   8192 pages x 4096 B  = 32.0 MiB   <- the flash array itself
 *   g_page_spare  8192 pages x   16 B  =  128 KiB   <- the OOB areas
 *   g_blocks       128 blocks x  16 B  =    2 KiB   <- per-block metadata
 *
 * The 32 MiB is the simulated media -- on a real product this is silicon, not
 * RAM, so it does not count against the controller's memory budget. The 2 KiB
 * of per-block metadata is the only part that would live in controller SRAM,
 * and that scaling (16 bytes per block) is worth remembering: a 1 TB drive with
 * 4 MiB blocks has ~256k blocks, so this table alone would be 4 MiB. Real
 * firmware therefore packs it far harder than we do here.
 *
 * Everything is a file-scope `static` array. No malloc anywhere in this project.
 * ========================================================================== */

#include <string.h>

#include "nand.h"

/* ---------------------------------------------------------------------------
 * Per-block metadata.
 *
 * A REALISM NOTE, because an interviewer will ask:
 * A real NAND die does not expose an erase counter, a read counter, or a write
 * pointer. Those are all things the FIRMWARE tracks, in controller SRAM, and
 * persists to the media periodically. We keep them inside the "device" here for
 * one specific reason: it lets the device layer *enforce* the rules (program
 * once, program in order) and hand the firmware a hard error when it gets them
 * wrong. That turns a class of bug that on real hardware shows up months later
 * as silent data corruption into an immediate, loud, testable failure.
 *
 * Where the distinction matters -- read counters are volatile in real firmware
 * and therefore lost across a power cut -- nand_power_cycle() models it.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t erase_count;    /* cumulative P/E cycles; never reset            */
    uint32_t read_count;     /* page reads since this block was last erased   */
    uint16_t write_pointer;  /* next legal page offset to program (0..PPB)    */
    uint8_t  factory_bad;    /* marked bad before the part shipped            */
    uint8_t  runtime_bad;    /* retired by firmware after an operation failed */
} nand_block_t;

/* ---- The media -------------------------------------------------------- */
static uint8_t      g_page_data[NAND_TOTAL_PAGES][NAND_PAGE_SIZE_BYTES];
static nand_spare_t g_page_spare[NAND_TOTAL_PAGES];
static nand_block_t g_blocks[NAND_NUM_BLOCKS];

/* ---- Simulator-only state --------------------------------------------- */
static nand_counters_t g_counters;
static uint32_t        g_program_fail_ppm;
static uint32_t        g_erase_fail_ppm;
static uint64_t        g_rng_state;
static bool            g_formatted;

/* ===========================================================================
 * A tiny deterministic PRNG (xorshift64*).
 * ---------------------------------------------------------------------------
 * Why not rand()? Because rand() differs between C libraries, so a benchmark
 * run on Linux would not reproduce on Windows. Every random decision in this
 * project -- factory bad block placement, injected failures, workload
 * generation -- comes from a seeded generator so that a reported write
 * amplification number can be reproduced exactly by anyone with the seed.
 * Reproducibility is not a nicety in firmware work; it is how you bisect a bug
 * that only appears after four hours of stress testing.
 * ========================================================================= */
static uint64_t rng_next(void)
{
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* Return true with probability ppm/1,000,000.
 * Guard on ppm == 0 first so that a run with failure injection disabled does
 * not consume any randomness -- that keeps two runs that differ only in
 * failure-injection settings otherwise bit-for-bit comparable. */
static bool rng_hit_ppm(uint32_t ppm)
{
    if (ppm == 0u) {
        return false;
    }
    return (uint32_t)(rng_next() % 1000000u) < ppm;
}

/* ===========================================================================
 * CRC-32, standard IEEE polynomial, reflected, table-driven.
 * ---------------------------------------------------------------------------
 * This stands in for the ECC engine a real controller runs in hardware. It does
 * not *correct* anything -- it only detects. That is enough for our purposes:
 * if a page ever reads back differently from how it was programmed, something
 * in the simulator or the firmware is broken and we want to know immediately
 * rather than debug a corrupted mapping table three thousand operations later.
 *
 * The table is built lazily on first use, into a static array -- 1 KiB of
 * const-after-init data, which is exactly how you would do it on a controller
 * with no hardware CRC block.
 * ========================================================================= */
static uint32_t g_crc_table[256];
static bool     g_crc_table_built;

static void crc32_build_table(void)
{
    uint32_t i, j, c;
    for (i = 0u; i < 256u; i++) {
        c = i;
        for (j = 0u; j < 8u; j++) {
            /* Reflected form: shift right, XOR the reversed polynomial. */
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        g_crc_table[i] = c;
    }
    g_crc_table_built = true;
}

uint32_t nand_crc32(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    if (!g_crc_table_built) {
        crc32_build_table();
    }
    for (i = 0u; i < len; i++) {
        crc = g_crc_table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ===========================================================================
 * Error strings
 * ========================================================================= */
const char *nand_strerror(nand_err_t e)
{
    switch (e) {
    case NAND_OK:                    return "OK";
    case NAND_ERR_OUT_OF_RANGE:      return "address out of range";
    case NAND_ERR_NOT_ERASED:        return "page already programmed (program-once violation)";
    case NAND_ERR_OUT_OF_ORDER:      return "out-of-order page program within block";
    case NAND_ERR_READ_UNWRITTEN:    return "read of an erased page";
    case NAND_ERR_BAD_BLOCK:         return "block is marked bad";
    case NAND_ERR_PROGRAM_FAIL:      return "program operation failed";
    case NAND_ERR_ERASE_FAIL:        return "erase operation failed";
    case NAND_ERR_ECC_UNCORRECTABLE: return "uncorrectable ECC error";
    case NAND_ERR_WORN_OUT:          return "block exceeded its endurance rating";
    default:                         return "unknown error";
    }
}

/* ===========================================================================
 * Lifecycle
 * ========================================================================= */

void nand_format(uint64_t seed)
{
    uint32_t i, placed, candidate;

    /* Seed first: everything below depends on it. A zero seed would make
     * xorshift produce zero forever, so fold in a non-zero constant. */
    g_rng_state = seed ^ 0x9E3779B97F4A7C15ULL;

    /* An erased NAND cell reads as 1, so an erased page is all 0xFF. Getting
     * this right matters: real firmware detects an erased page by checking for
     * all-0xFF, and a simulator that zero-fills instead would let a bug of that
     * shape pass. */
    memset(g_page_data, 0xFF, sizeof(g_page_data));

    for (i = 0u; i < NAND_TOTAL_PAGES; i++) {
        g_page_spare[i].lpn = LPN_INVALID;
        g_page_spare[i].seq = 0u;
        g_page_spare[i].crc = 0u;
        g_page_spare[i].tag = 0u;
    }
    memset(g_blocks, 0, sizeof(g_blocks));
    memset(&g_counters, 0, sizeof(g_counters));

    g_program_fail_ppm = NAND_PROGRAM_FAIL_PPM;
    g_erase_fail_ppm   = NAND_ERASE_FAIL_PPM;

    /* Scatter the factory bad blocks. We deliberately keep them out of the
     * metadata region: on a real part the manufacturer guarantees block 0 is
     * good precisely so that the boot loader has somewhere to live, and our
     * metadata region plays that role. */
    placed = 0u;
    while (placed < NAND_FACTORY_BAD_BLOCKS && placed < FTL_DATA_BLOCKS) {
        candidate = FTL_DATA_BLOCK_FIRST +
                    (uint32_t)(rng_next() % (uint64_t)FTL_DATA_BLOCKS);
        if (!g_blocks[candidate].factory_bad) {
            g_blocks[candidate].factory_bad = 1u;
            placed++;
        }
    }

    g_formatted = true;
}

void nand_power_cycle(void)
{
    uint32_t i;

    /* The array contents (g_page_data, g_page_spare) and the durable per-block
     * facts (erase counts, bad marks, write pointers) all survive -- that is
     * what "non-volatile" means.
     *
     * The read-disturb counters do NOT: on a real product they live in
     * controller SRAM and are lost. Modelling that is not pedantry; it is the
     * reason real firmware periodically flushes those counters to the media,
     * and a design that forgets it will silently stop refreshing hot-read
     * blocks on a machine that reboots often. */
    for (i = 0u; i < NAND_NUM_BLOCKS; i++) {
        g_blocks[i].read_count = 0u;
    }
}

void nand_set_failure_rates(uint32_t program_ppm, uint32_t erase_ppm)
{
    g_program_fail_ppm = program_ppm;
    g_erase_fail_ppm   = erase_ppm;
}

/* ===========================================================================
 * Media operations
 * ========================================================================= */

/* Shared entry checks for a page-addressed operation. */
static nand_err_t check_ppn(uint32_t ppn, uint32_t *pbn_out)
{
    uint32_t pbn;

    if (!g_formatted) {
        return NAND_ERR_OUT_OF_RANGE;   /* using the die before manufacture */
    }
    if (ppn >= NAND_TOTAL_PAGES) {
        return NAND_ERR_OUT_OF_RANGE;
    }
    pbn = PPN_TO_BLOCK(ppn);
    if (g_blocks[pbn].factory_bad || g_blocks[pbn].runtime_bad) {
        return NAND_ERR_BAD_BLOCK;
    }
    *pbn_out = pbn;
    return NAND_OK;
}

nand_err_t nand_read_page(uint32_t ppn, void *data, nand_spare_t *spare)
{
    uint32_t pbn, off, crc;
    nand_err_t err;

    err = check_ppn(ppn, &pbn);
    if (err != NAND_OK) {
        return err;
    }

    off = PPN_TO_OFFSET(ppn);

    /* Because programming within a block is strictly sequential, the write
     * pointer alone tells us whether a page holds data -- no separate
     * per-page "programmed" bitmap is needed. That is 8 KiB of RAM saved at
     * the default geometry, and it scales: this trick is why the write
     * pointer is worth enforcing rather than merely documenting. */
    if (off >= g_blocks[pbn].write_pointer) {
        return NAND_ERR_READ_UNWRITTEN;
    }

    g_counters.reads++;
    g_counters.busy_us += NAND_TREAD_US;
    g_blocks[pbn].read_count++;

    /* Verify the stored CRC. On real hardware this is the ECC engine reporting
     * how many bits it had to correct, and how close the page is to being
     * uncorrectable. */
    crc = nand_crc32(g_page_data[ppn], NAND_PAGE_SIZE_BYTES);
    if (crc != g_page_spare[ppn].crc) {
        g_counters.ecc_failures++;
        return NAND_ERR_ECC_UNCORRECTABLE;
    }

    if (data != NULL) {
        memcpy(data, g_page_data[ppn], NAND_PAGE_SIZE_BYTES);
    }
    if (spare != NULL) {
        *spare = g_page_spare[ppn];
    }
    return NAND_OK;
}

nand_err_t nand_read_spare(uint32_t ppn, nand_spare_t *spare)
{
    uint32_t pbn, off;
    nand_err_t err;

    err = check_ppn(ppn, &pbn);
    if (err != NAND_OK) {
        return err;
    }
    off = PPN_TO_OFFSET(ppn);
    if (off >= g_blocks[pbn].write_pointer) {
        return NAND_ERR_READ_UNWRITTEN;
    }

    /* A spare-only read moves ~16 bytes instead of ~4096 across the bus, so it
     * is much cheaper. Garbage collection leans on this: to decide whether a
     * page is still valid it only needs the LPN in the spare area, not the
     * 4 KiB of data. Charging a tenth of a full read makes the benchmark
     * reflect that, instead of pretending metadata scans are free. */
    g_counters.reads++;
    g_counters.busy_us += NAND_TREAD_US / 10u;
    g_blocks[pbn].read_count++;

    if (spare != NULL) {
        *spare = g_page_spare[ppn];
    }
    return NAND_OK;
}

nand_err_t nand_program_page(uint32_t ppn, const void *data, const nand_spare_t *spare)
{
    uint32_t pbn, off;
    nand_err_t err;

    err = check_ppn(ppn, &pbn);
    if (err != NAND_OK) {
        return err;
    }
    if (data == NULL || spare == NULL) {
        return NAND_ERR_OUT_OF_RANGE;
    }

    off = PPN_TO_OFFSET(ppn);

    /* RULE 1: program-once. The page has already been written since the last
     * erase, so its floating gates already hold charge. There is no command
     * that takes it off again short of erasing the entire block. */
    if (off < g_blocks[pbn].write_pointer) {
        return NAND_ERR_NOT_ERASED;
    }
    /* RULE 2: sequential programming. Writing page 7 while page 5 is still
     * erased is illegal on MLC/TLC parts -- the program pulse for a high page
     * disturbs the not-yet-programmed cells beneath it. */
    if (off > g_blocks[pbn].write_pointer) {
        return NAND_ERR_OUT_OF_ORDER;
    }

    g_counters.programs++;
    g_counters.busy_us += NAND_TPROG_US;

    /* RULE 5: programs sometimes just fail. When they do the page's contents
     * are indeterminate -- and the slot is CONSUMED, because charge did go onto
     * the floating gates, just not reliably. So we advance the write pointer
     * without storing anything: any later read of this page will find data that
     * does not match its CRC and be reported as an uncorrectable ECC error,
     * which is precisely what real hardware does.
     *
     * Note what we do NOT do: retire the block. That is firmware's decision,
     * taken in badblock.c, and it has to happen only AFTER firmware has read
     * the still-good pages out of this block and relocated them. */
    if (rng_hit_ppm(g_program_fail_ppm)) {
        g_counters.program_failures++;
        g_blocks[pbn].write_pointer = (uint16_t)(off + 1u);
        return NAND_ERR_PROGRAM_FAIL;
    }

    memcpy(g_page_data[ppn], data, NAND_PAGE_SIZE_BYTES);
    g_page_spare[ppn]     = *spare;
    g_page_spare[ppn].crc = nand_crc32(g_page_data[ppn], NAND_PAGE_SIZE_BYTES);
    g_blocks[pbn].write_pointer = (uint16_t)(off + 1u);

    return NAND_OK;
}

nand_err_t nand_erase_block(uint32_t pbn)
{
    uint32_t i, base;

    if (!g_formatted || pbn >= NAND_NUM_BLOCKS) {
        return NAND_ERR_OUT_OF_RANGE;
    }
    if (g_blocks[pbn].factory_bad || g_blocks[pbn].runtime_bad) {
        return NAND_ERR_BAD_BLOCK;
    }
    /* RULE 4: finite endurance. Past the rating the oxide is too degraded to
     * hold charge reliably; the block must be retired even though nothing has
     * "failed" yet. Wear levelling exists to push this moment as far into the
     * future as possible, and to make sure every block reaches it at roughly
     * the same time rather than a few blocks dying while the rest are fresh. */
    if (g_blocks[pbn].erase_count >= NAND_MAX_ERASE_CYCLES) {
        return NAND_ERR_WORN_OUT;
    }

    g_counters.erases++;
    g_counters.busy_us += NAND_TERASE_US;

    /* A failed erase leaves the block in an indeterminate state: some cells
     * discharged, some not. We deliberately do NOT reset the write pointer, so
     * the block cannot be programmed into afterwards, and we leave the retire
     * decision to firmware. The erase counter has already been incremented --
     * the wear happened whether or not the operation succeeded. */
    if (rng_hit_ppm(g_erase_fail_ppm)) {
        g_counters.erase_failures++;
        g_blocks[pbn].erase_count++;
        return NAND_ERR_ERASE_FAIL;
    }

    base = pbn * NAND_PAGES_PER_BLOCK;
    memset(g_page_data[base], 0xFF, (size_t)NAND_PAGES_PER_BLOCK * NAND_PAGE_SIZE_BYTES);
    for (i = 0u; i < NAND_PAGES_PER_BLOCK; i++) {
        g_page_spare[base + i].lpn = LPN_INVALID;
        g_page_spare[base + i].seq = 0u;
        g_page_spare[base + i].crc = 0u;
        g_page_spare[base + i].tag = 0u;
    }

    g_blocks[pbn].write_pointer = 0u;
    g_blocks[pbn].read_count    = 0u;   /* erasing resets the disturb budget */
    g_blocks[pbn].erase_count++;

    return NAND_OK;
}

void nand_mark_bad(uint32_t pbn)
{
    /* On real hardware this writes a non-0xFF marker byte into the spare area
     * of the block's first page -- one of the very few times firmware programs
     * a page it does not intend to read back as data. It has to be durable,
     * because the whole point is that the NEXT boot must not touch this block.
     * Here the flag lives in g_blocks, which survives nand_power_cycle(), so it
     * models the durability correctly. */
    if (pbn < NAND_NUM_BLOCKS) {
        g_blocks[pbn].runtime_bad = 1u;
    }
}

/* ===========================================================================
 * Introspection
 * ========================================================================= */

bool nand_block_is_factory_bad(uint32_t pbn)
{
    return (pbn < NAND_NUM_BLOCKS) && (g_blocks[pbn].factory_bad != 0u);
}

bool nand_block_is_runtime_bad(uint32_t pbn)
{
    return (pbn < NAND_NUM_BLOCKS) && (g_blocks[pbn].runtime_bad != 0u);
}

bool nand_block_is_bad(uint32_t pbn)
{
    if (pbn >= NAND_NUM_BLOCKS) {
        return true;    /* out of range is "unusable", which is safest here */
    }
    return (g_blocks[pbn].factory_bad != 0u) || (g_blocks[pbn].runtime_bad != 0u);
}

uint32_t nand_block_erase_count(uint32_t pbn)
{
    return (pbn < NAND_NUM_BLOCKS) ? g_blocks[pbn].erase_count : 0u;
}

uint32_t nand_block_read_count(uint32_t pbn)
{
    return (pbn < NAND_NUM_BLOCKS) ? g_blocks[pbn].read_count : 0u;
}

bool nand_block_read_disturb_exceeded(uint32_t pbn)
{
    return (pbn < NAND_NUM_BLOCKS) &&
           (g_blocks[pbn].read_count >= NAND_READ_DISTURB_LIMIT);
}

uint32_t nand_block_write_pointer(uint32_t pbn)
{
    return (pbn < NAND_NUM_BLOCKS) ? (uint32_t)g_blocks[pbn].write_pointer : 0u;
}

bool nand_page_is_programmed(uint32_t ppn)
{
    uint32_t pbn;

    if (ppn >= NAND_TOTAL_PAGES) {
        return false;
    }
    pbn = PPN_TO_BLOCK(ppn);
    return PPN_TO_OFFSET(ppn) < g_blocks[pbn].write_pointer;
}

const nand_counters_t *nand_counters(void)
{
    return &g_counters;
}

void nand_counters_reset(void)
{
    memset(&g_counters, 0, sizeof(g_counters));
}
