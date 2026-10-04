/* =============================================================================
 * ftl_config.h -- Compile-time configuration for the whole simulator.
 * =============================================================================
 *
 * WHY A SINGLE CONFIG HEADER?
 * ---------------------------
 * Real SSD firmware runs on a controller with a few hundred KB of SRAM and no
 * operating system. There is no malloc(), no realloc(), and no "we'll grow the
 * array if we need to". Every buffer, every table, every queue is sized at
 * BUILD TIME so that the linker can tell you -- before the firmware ever ships
 * -- whether the image fits in the silicon you have.
 *
 * This project follows the same rule: every array in the simulator is a
 * fixed-size static array whose dimensions come from the macros below. If you
 * change NAND_NUM_BLOCKS from 128 to 4096, nothing needs to be rewritten; the
 * whole device just gets bigger and the static memory report printed at start-up
 * tells you exactly how much RAM the mapping tables now cost.
 *
 * That "how much RAM does the mapping table cost?" number is the single most
 * important trade-off in FTL design, and having it fall straight out of the
 * config is deliberate.
 *
 * HOW TO OVERRIDE
 * ---------------
 * Every macro is wrapped in #ifndef, so you can override any of them from the
 * command line without editing this file:
 *
 *     make CFLAGS_EXTRA="-DNAND_NUM_BLOCKS=512 -DFTL_OP_PERCENT=7"
 * ========================================================================== */

#ifndef FTL_CONFIG_H
#define FTL_CONFIG_H

/* ---------------------------------------------------------------------------
 * 1. NAND GEOMETRY
 * ---------------------------------------------------------------------------
 * A NAND die is organised as:
 *
 *      device  =  N blocks
 *      block   =  P pages          <- ERASE happens at this granularity
 *      page    =  B bytes + spare  <- READ and PROGRAM happen at this granularity
 *
 * The asymmetry (read/program a page, but erase a whole block) is the entire
 * reason the Flash Translation Layer exists. See DOCS/CONCEPTS_EXPLAINED.md.
 *
 * The defaults below describe a small 32 MiB device. It is deliberately small:
 * a small device fills up quickly, which means garbage collection and wear
 * levelling kick in after a few seconds of simulated work instead of a few
 * hours. All the interesting behaviour is scale-invariant.
 * ------------------------------------------------------------------------- */

#ifndef NAND_PAGE_SIZE_BYTES
#define NAND_PAGE_SIZE_BYTES      4096u   /* user-data area of one page       */
#endif

#ifndef NAND_PAGES_PER_BLOCK
#define NAND_PAGES_PER_BLOCK        64u   /* pages that share one erase gate  */
#endif

#ifndef NAND_NUM_BLOCKS
#define NAND_NUM_BLOCKS            128u   /* blocks on the (single) die       */
#endif

/* Derived geometry. Note these are macros, not variables: the compiler folds
 * them to constants, so array bounds below are compile-time constants. */
#define NAND_TOTAL_PAGES          (NAND_NUM_BLOCKS * NAND_PAGES_PER_BLOCK)
#define NAND_BLOCK_SIZE_BYTES     (NAND_PAGES_PER_BLOCK * NAND_PAGE_SIZE_BYTES)
#define NAND_TOTAL_BYTES          ((unsigned long long)NAND_NUM_BLOCKS * NAND_BLOCK_SIZE_BYTES)

/* Convert between a physical page number (PPN) and (block, offset).
 * PPN is a flat 0..NAND_TOTAL_PAGES-1 index -- exactly how a real controller
 * addresses the die once row/column decoding is abstracted away. */
#define PPN_TO_BLOCK(ppn)         ((ppn) / NAND_PAGES_PER_BLOCK)
#define PPN_TO_OFFSET(ppn)        ((ppn) % NAND_PAGES_PER_BLOCK)
#define BLOCK_OFFSET_TO_PPN(b, o) ((b) * NAND_PAGES_PER_BLOCK + (o))

/* ---------------------------------------------------------------------------
 * 2. NAND RELIABILITY / TIMING MODEL
 * ------------------------------------------------------------------------- */

/* Program/Erase cycles a block survives before we declare it worn out.
 * Real numbers: SLC ~60k-100k, MLC ~3k-10k, TLC ~1k-3k, QLC ~150-1000.
 * 3000 puts us in MLC territory, which is what enterprise/industrial parts
 * historically used. This number is what makes wear levelling matter. */
#ifndef NAND_MAX_ERASE_CYCLES
#define NAND_MAX_ERASE_CYCLES     3000u
#endif

/* Blocks marked bad by the manufacturer before the part ever shipped.
 * Every NAND die ships with some: guaranteeing a 100% good die would make the
 * part far too expensive, so the datasheet instead guarantees "at most X% bad".
 * Firmware MUST scan for these at first power-on and never touch them. */
#ifndef NAND_FACTORY_BAD_BLOCKS
#define NAND_FACTORY_BAD_BLOCKS      2u
#endif

/* Read disturb: reading a page slightly stresses the *other* pages in the same
 * block. Read enough times without an intervening erase and neighbouring pages
 * start returning bit errors. Firmware counts reads per block and proactively
 * rewrites ("refreshes") a block that crosses the threshold.
 * Real thresholds are ~50k-1M reads; we use a small one so the mechanism is
 * actually exercised in a short simulation. */
#ifndef NAND_READ_DISTURB_LIMIT
#define NAND_READ_DISTURB_LIMIT  20000u
#endif

/* Operation latencies in microseconds, from a typical MLC datasheet.
 * The 1 : 12 : 60 ratio between read, program and erase is the whole reason
 * garbage collection is expensive and why we count erases so carefully. */
#ifndef NAND_TREAD_US
#define NAND_TREAD_US               50u
#endif
#ifndef NAND_TPROG_US
#define NAND_TPROG_US              600u
#endif
#ifndef NAND_TERASE_US
#define NAND_TERASE_US            3000u
#endif

/* Injected failure rates, expressed in parts per million operations.
 * Real NAND does fail a program or erase occasionally; firmware that cannot
 * cope with that corrupts user data. Setting these non-zero is how we prove
 * the bad-block manager works. Default 0 so benchmarks are deterministic;
 * the CLI can raise them, and the unit tests do. */
#ifndef NAND_PROGRAM_FAIL_PPM
#define NAND_PROGRAM_FAIL_PPM        0u
#endif
#ifndef NAND_ERASE_FAIL_PPM
#define NAND_ERASE_FAIL_PPM          0u
#endif

/* ---------------------------------------------------------------------------
 * 3. FTL LAYOUT
 * ---------------------------------------------------------------------------
 * The physical blocks are split into two regions:
 *
 *    [ metadata region ][ ------------- data region ------------- ]
 *      FTL_META_BLOCKS                FTL_DATA_BLOCKS
 *
 * The metadata region holds the checkpoint of the mapping table and the
 * journal used for power-loss recovery. It is deliberately at a FIXED,
 * known location: at mount time the firmware has no mapping table yet, so it
 * has to be able to find its own metadata without one. Chicken and egg.
 * ------------------------------------------------------------------------- */

#ifndef FTL_META_BLOCKS
#define FTL_META_BLOCKS              4u
#endif

#define FTL_META_BLOCK_FIRST         0u
#define FTL_META_BLOCK_LAST          (FTL_META_BLOCKS - 1u)
#define FTL_DATA_BLOCK_FIRST         FTL_META_BLOCKS
#define FTL_DATA_BLOCKS              (NAND_NUM_BLOCKS - FTL_META_BLOCKS)

/* ---------------------------------------------------------------------------
 * 4. OVER-PROVISIONING
 * ---------------------------------------------------------------------------
 * We expose LESS capacity to the host than the die physically has. The hidden
 * remainder is "over-provisioning" (OP), and it is the free space garbage
 * collection breathes with.
 *
 * With OP = 0 the drive is 100% full the moment the host writes every LBA once,
 * and every subsequent write has to garbage-collect an almost-entirely-valid
 * block -- write amplification explodes towards infinity. With OP = 25% the
 * collector always has slack.
 *
 * Consumer drives ship ~7% (which is really just the GiB-vs-GB gap: a "512 GB"
 * drive holds 512 * 10^9 bytes of user data on 512 * 2^30 bytes of NAND).
 * Enterprise write-intensive drives ship 28% or more. This single number is the
 * biggest lever on write amplification in the whole design -- and
 * tools/run_experiments.py sweeps it to prove exactly that.
 * ------------------------------------------------------------------------- */

#ifndef FTL_OP_PERCENT
#define FTL_OP_PERCENT              25u
#endif

/* Logical capacity, in blocks then pages.
 * We compute it in BLOCKS first and multiply up, so that the exposed capacity
 * is always an exact whole number of blocks. Block-mapped and hybrid FTLs
 * require that; a logical capacity of "93.4 blocks" is not representable. */
#define FTL_LOGICAL_BLOCKS        ((FTL_DATA_BLOCKS * (100u - FTL_OP_PERCENT)) / 100u)
#define FTL_LOGICAL_PAGES         (FTL_LOGICAL_BLOCKS * NAND_PAGES_PER_BLOCK)
#define FTL_LOGICAL_BYTES         ((unsigned long long)FTL_LOGICAL_PAGES * NAND_PAGE_SIZE_BYTES)

/* LPN <-> (logical block, offset), the block-mapped view of the address space */
#define LPN_TO_LBN(lpn)           ((lpn) / NAND_PAGES_PER_BLOCK)
#define LPN_TO_LOFF(lpn)          ((lpn) % NAND_PAGES_PER_BLOCK)

/* ---------------------------------------------------------------------------
 * 5. GARBAGE COLLECTION AND WEAR LEVELLING POLICY
 * ------------------------------------------------------------------------- */

/* Start collecting when the free-block pool drops to this level. Too low and
 * a burst of writes can starve; too high and we collect blocks that would have
 * become entirely invalid on their own if we had just waited. */
#ifndef FTL_GC_LOW_WATERMARK
#define FTL_GC_LOW_WATERMARK         6u
#endif

/* Stop collecting once the pool has recovered to here. The gap between the two
 * watermarks is hysteresis: without it, the FTL would collect one block, drop
 * back below the trigger on the very next write, and thrash. */
#ifndef FTL_GC_HIGH_WATERMARK
#define FTL_GC_HIGH_WATERMARK       10u
#endif

/* Hybrid mapping: how many log blocks sit in front of the block-mapped data
 * blocks. More log blocks = fewer merges = lower write amplification, but each
 * one costs a page-level map of NAND_PAGES_PER_BLOCK entries in SRAM. */
#ifndef FTL_LOG_BLOCK_POOL
#define FTL_LOG_BLOCK_POOL           8u
#endif

/* Static wear levelling: if (max erase count - min erase count) across all
 * blocks exceeds this, forcibly migrate the cold data sitting in the
 * least-erased block so that block can join the free pool and take its turn.
 * Too small a threshold and we burn erases doing pointless migrations; too
 * large and cold data pins young blocks forever. */
#ifndef FTL_WEAR_GAP_THRESHOLD
#define FTL_WEAR_GAP_THRESHOLD      16u
#endif

/* Cost-benefit GC weights the age of a block. This is the clock tick used as
 * "now" -- we use the global page-program sequence number, which advances
 * with real work done rather than with wall-clock time. */

/* ---------------------------------------------------------------------------
 * 6. POWER-LOSS RECOVERY
 * ------------------------------------------------------------------------- */

/* Write a full checkpoint of the mapping table every this-many host writes.
 * Trade-off: frequent checkpoints cost NAND writes (they amplify!) but shorten
 * the journal that has to be replayed after an unexpected power loss. */
#ifndef FTL_CHECKPOINT_INTERVAL
#define FTL_CHECKPOINT_INTERVAL   4096u
#endif

/* ---------------------------------------------------------------------------
 * 7. SENTINELS
 * ------------------------------------------------------------------------- */
#define PPN_INVALID               0xFFFFFFFFu   /* "this LPN has no physical home" */
#define PBN_INVALID               0xFFFFFFFFu
#define LPN_INVALID               0xFFFFFFFFu

/* ---------------------------------------------------------------------------
 * 8. BUILD-TIME SANITY CHECKS
 * ---------------------------------------------------------------------------
 * These are static assertions written in C99 (no <assert.h> _Static_assert
 * dependency, so this header compiles anywhere). A violated condition makes the
 * array size zero or negative, which is a compile error naming the check.
 * Catching a bad configuration at build time rather than at run time is exactly
 * the discipline embedded work demands.
 * ------------------------------------------------------------------------- */
#define FTL_STATIC_ASSERT(cond, name) \
    typedef char ftl_static_assert_##name[(cond) ? 1 : -1]

FTL_STATIC_ASSERT(NAND_PAGES_PER_BLOCK >= 4,            pages_per_block_too_small);
FTL_STATIC_ASSERT(FTL_META_BLOCKS >= 2,                 need_two_meta_blocks);
FTL_STATIC_ASSERT(FTL_DATA_BLOCKS > FTL_LOGICAL_BLOCKS, need_some_overprovisioning);
FTL_STATIC_ASSERT(FTL_GC_HIGH_WATERMARK > FTL_GC_LOW_WATERMARK, watermarks_need_hysteresis);
/* The free pool, the log pool and the exposed capacity all have to fit in the
 * data region simultaneously, or the FTL can deadlock with nowhere to write. */
FTL_STATIC_ASSERT(FTL_LOGICAL_BLOCKS + FTL_LOG_BLOCK_POOL + FTL_GC_HIGH_WATERMARK
                  + NAND_FACTORY_BAD_BLOCKS <= FTL_DATA_BLOCKS, data_region_oversubscribed);

#endif /* FTL_CONFIG_H */
