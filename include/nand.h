/* =============================================================================
 * nand.h -- The simulated NAND flash die.
 * =============================================================================
 *
 * THIS FILE IS THE HARDWARE.
 * --------------------------
 * Everything above this layer (mapping, garbage collection, wear levelling) is
 * firmware. This layer pretends to be the silicon, and it is deliberately
 * HOSTILE, because real NAND is hostile. It enforces:
 *
 *   1. PROGRAM-ONCE.  A page can be programmed exactly once after its block is
 *      erased. Programming it again is an error, not an overwrite. (Physically:
 *      programming pushes electrons ONTO a floating gate. There is no per-page
 *      way to pull them off again; only a block-wide erase does that.)
 *
 *   2. SEQUENTIAL PAGE PROGRAMMING.  Within a block you must program page 0,
 *      then 1, then 2... Skipping around is forbidden on modern MLC/TLC parts
 *      because programming a high page disturbs the cells of lower pages that
 *      have not been written yet. Firmware that ignores this reads back garbage
 *      on real hardware while working perfectly in a naive simulator -- which is
 *      exactly why this simulator enforces it.
 *
 *   3. ERASE AT BLOCK GRANULARITY ONLY.  There is no erase-page command.
 *
 *   4. FINITE ENDURANCE.  Each block has an erase counter. Past
 *      NAND_MAX_ERASE_CYCLES the block is worn out and refuses to erase.
 *
 *   5. BLOCKS FAIL.  Some are bad from the factory; others go bad at runtime
 *      when a program or erase operation does not complete.
 *
 *   6. READS COST SOMETHING.  Reading disturbs neighbouring cells in the block.
 *
 * WHAT THIS LAYER DELIBERATELY DOES *NOT* KNOW
 * --------------------------------------------
 * It has no idea what "valid" or "invalid" data means. A NAND die cannot tell
 * you whether a programmed page still holds live data -- only the FTL knows
 * that, because only the FTL knows the mapping. Many toy simulators put a
 * "PAGE_INVALID" state in the device layer; that is a layering violation that
 * makes the resulting design impossible to port to real hardware. Here,
 * validity lives one level up, in ftl.c.
 * ========================================================================== */

#ifndef NAND_H
#define NAND_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ftl_config.h"

/* ---------------------------------------------------------------------------
 * Error codes returned by every media operation.
 * A real controller reports these through a status register; we return them.
 * ------------------------------------------------------------------------- */
typedef enum {
    NAND_OK = 0,
    NAND_ERR_OUT_OF_RANGE,      /* PPN / PBN outside the die                  */
    NAND_ERR_NOT_ERASED,        /* program-once violation                     */
    NAND_ERR_OUT_OF_ORDER,      /* non-sequential program within a block      */
    NAND_ERR_READ_UNWRITTEN,    /* read of an erased page                     */
    NAND_ERR_BAD_BLOCK,         /* block is marked bad                        */
    NAND_ERR_PROGRAM_FAIL,      /* injected transient program failure         */
    NAND_ERR_ERASE_FAIL,        /* injected transient erase failure           */
    NAND_ERR_ECC_UNCORRECTABLE, /* data did not match its stored CRC          */
    NAND_ERR_WORN_OUT           /* erase count exceeded the endurance rating  */
} nand_err_t;

const char *nand_strerror(nand_err_t e);

/* ---------------------------------------------------------------------------
 * THE SPARE AREA (also called OOB -- Out Of Band).
 * ---------------------------------------------------------------------------
 * Every NAND page has a few extra bytes beyond the user data (typically 64-224
 * bytes for a 4 KiB page). The controller uses them for ECC parity, and
 * firmware uses whatever is left for its own bookkeeping.
 *
 * The single most important thing firmware stores there is the LOGICAL page
 * number that this physical page holds. That gives you a REVERSE MAP
 * (physical -> logical) written durably alongside the data itself, and it buys
 * you two things you cannot get any other way:
 *
 *   - Garbage collection can tell, for any physical page it is about to move,
 *     which logical page it belongs to -- so it can update the forward map.
 *     Without this you would need a reverse map in RAM, doubling the table cost.
 *
 *   - After an unexpected power loss you can RECONSTRUCT the entire forward
 *     mapping table by scanning the spare areas, even with no checkpoint at all.
 *     The media becomes self-describing.
 *
 * `seq` is a global monotonically increasing program counter. When two physical
 * pages claim the same LPN (which happens constantly -- that is what an
 * out-of-place update looks like), the one with the higher seq is the newer
 * one. This is how recovery resolves conflicts.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t lpn;   /* logical page this physical page holds, or LPN_INVALID  */
    uint32_t seq;   /* global program sequence number -- newest wins          */
    uint32_t crc;   /* CRC-32 of the data area; stands in for ECC parity      */
    uint32_t tag;   /* free for firmware use (we mark metadata pages with it) */
} nand_spare_t;

/* Values for nand_spare_t.tag.
 * The tag tells a recovery scan what KIND of page it is looking at without
 * needing any in-RAM state -- which is exactly the situation recovery is in.
 * A page tagged LOG_DATA belongs to a hybrid-mapping log block; one tagged
 * USER_DATA sits at its natural offset in a data block; one tagged CHECKPOINT
 * is part of a serialised mapping table in the metadata region. */
#define NAND_TAG_USER_DATA     0xD0000001u
#define NAND_TAG_LOG_DATA      0xD0000002u
#define NAND_TAG_PADDING       0xD0000003u
#define NAND_TAG_CHECKPOINT    0xC0000001u

/* ---------------------------------------------------------------------------
 * Counters. These are what the whole project ultimately reports on.
 * ------------------------------------------------------------------------- */
typedef struct {
    uint64_t reads;             /* page reads issued to the die               */
    uint64_t programs;          /* page programs issued to the die            */
    uint64_t erases;            /* block erases issued to the die             */
    uint64_t program_failures;  /* programs that failed and grew a bad block  */
    uint64_t erase_failures;    /* erases that failed and grew a bad block    */
    uint64_t ecc_failures;      /* CRC mismatches on read                     */
    uint64_t busy_us;           /* modelled time the die spent busy, in us    */
} nand_counters_t;

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------------- */

/* Manufacture a fresh die: every block erased, erase counts zero, a handful of
 * factory-bad blocks scattered deterministically by `seed`. Call this once at
 * program start -- it is the equivalent of taking the part out of its tray. */
void nand_format(uint64_t seed);

/* Simulate yanking the power cable. The media contents survive (that is the
 * whole point of non-volatile memory) but every volatile convenience the
 * simulator keeps is discarded, so the FTL is forced to rebuild its state from
 * what is actually on the die. Used by the power-loss recovery tests. */
void nand_power_cycle(void);

/* Failure injection knobs, in parts per million operations. */
void nand_set_failure_rates(uint32_t program_ppm, uint32_t erase_ppm);

/* Write the bad-block marker for this block.
 *
 * NOTE THE DIVISION OF RESPONSIBILITY. When a program or erase fails, the die
 * reports the failure and does nothing else -- it does not retire itself. It is
 * FIRMWARE's job to decide the block is finished and to record that decision
 * durably, and that is exactly how real parts behave. Keeping it this way
 * matters for one practical reason: after a failed program, the pages already
 * written in that block are usually still readable, and firmware needs to read
 * them out and relocate the data BEFORE it gives up on the block. A die that
 * retired itself on the spot would take the user's data with it. */
void nand_mark_bad(uint32_t pbn);

/* ---------------------------------------------------------------------------
 * Media operations -- the only four commands a NAND die really offers.
 * ------------------------------------------------------------------------- */

/* Read one page. `data` must be at least NAND_PAGE_SIZE_BYTES; either pointer
 * may be NULL if the caller only wants the other half (reading just the spare
 * area is a real, much cheaper NAND command, and GC uses it heavily). */
nand_err_t nand_read_page(uint32_t ppn, void *data, nand_spare_t *spare);

/* Read only the spare area. Modelled as cheaper than a full page read because
 * the die transfers far fewer bytes over the bus. */
nand_err_t nand_read_spare(uint32_t ppn, nand_spare_t *spare);

/* Program one page. Fails with NAND_ERR_NOT_ERASED if the page has already been
 * programmed since the last erase, and NAND_ERR_OUT_OF_ORDER if pages below it
 * in the same block are still erased. */
nand_err_t nand_program_page(uint32_t ppn, const void *data, const nand_spare_t *spare);

/* Erase one block: all NAND_PAGES_PER_BLOCK pages return to the erased state
 * and the block's erase counter increments. */
nand_err_t nand_erase_block(uint32_t pbn);

/* ---------------------------------------------------------------------------
 * Introspection -- things a real controller can genuinely ask the die,
 * plus a couple the simulator exposes for measurement.
 * ------------------------------------------------------------------------- */

/* Was this block marked bad by the manufacturer? On real parts this is a
 * non-0xFF byte in the spare area of page 0, which firmware scans for at first
 * power-on and must never erase away. */
bool     nand_block_is_factory_bad(uint32_t pbn);

/* Has this block been retired at runtime by a failed program/erase? */
bool     nand_block_is_runtime_bad(uint32_t pbn);
bool     nand_block_is_bad(uint32_t pbn);        /* either of the above       */

uint32_t nand_block_erase_count(uint32_t pbn);
uint32_t nand_block_read_count(uint32_t pbn);    /* reads since last erase    */
bool     nand_block_read_disturb_exceeded(uint32_t pbn);

/* Index of the next page that may legally be programmed in this block
 * (0 == fully erased, NAND_PAGES_PER_BLOCK == full). The write pointer. */
uint32_t nand_block_write_pointer(uint32_t pbn);

/* Has this specific page been programmed since the last erase of its block? */
bool     nand_page_is_programmed(uint32_t ppn);

const nand_counters_t *nand_counters(void);
void                   nand_counters_reset(void);

/* CRC-32 (IEEE 802.3 polynomial, reflected). Exposed because the journal uses
 * the same routine to protect its records. */
uint32_t nand_crc32(const void *buf, size_t len);

#endif /* NAND_H */
