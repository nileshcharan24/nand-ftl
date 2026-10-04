# 08 — One Write, Followed All the Way Down

*Assumes: documents 01–07. This is the document that turns "I read about it"
into "I wrote it."*

We follow a single call — `ftl_write(42, buffer)` — from the host interface
down to the flash chip and back. Every function named here is real; open the
files alongside this.

---

## The call stack, in one picture

```
  ftl_write(42, buf)                            src/ftl.c
      │
      ├─▶ gc_maybe_run()                        src/gc.c        ─── make space
      ├─▶ wear_maybe_level()                    src/wear.c      ─── level wear
      │
      └─▶ mapping_write(42, buf)                src/mapping.c   ─── dispatch
              │
              └─▶ page_write(42, buf)           src/mapping.c   ─── page scheme
                      │
                      ├─▶ mapping_program_at_frontier(...)      src/mapping.c
                      │       │
                      │       ├─▶ blockmgr_next_free_ppn()      src/blockmgr.c
                      │       │       └─▶ blockmgr_alloc()
                      │       │               └─▶ wear_pick_free_block()  src/wear.c
                      │       │
                      │       └─▶ program_into_block(...)       src/mapping.c
                      │               └─▶ nand_program_page()   src/nand.c  ◀── THE CHIP
                      │
                      ├─▶ mapping_invalidate_ppn(old)           src/mapping.c
                      └─▶ mapping_validate_ppn(new)             src/mapping.c
```

---

## Step 1 — `ftl_write()` in `src/ftl.c`

```c
ftl_err_t ftl_write(uint32_t lpn, const void *buf)
{
    if (!g_ftl.mounted)                        return FTL_ERR_UNSUPPORTED;
    if (lpn >= FTL_LOGICAL_PAGES || buf == NULL) return FTL_ERR_RANGE;

    /* Make space before accepting the work, not after. */
    (void)gc_maybe_run();
    (void)wear_maybe_level();

    for (attempt = 0; attempt < FTL_MAX_OP_RETRY; attempt++) {
        err = mapping_write(lpn, buf);
        if (err != FTL_ERR_MEDIA) break;
        g_ftl.stats.program_retries++;
    }
    ...
}
```

Four things happen, and each is a design decision:

**Bounds check first.** `lpn >= FTL_LOGICAL_PAGES` is rejected, not wrapped. An
FTL that silently wraps an out-of-range address corrupts a completely unrelated
file. Fail loudly.

**Housekeeping runs *before* the write, not after.** If GC ran afterwards, a
write could arrive with no free block available and fail — and "the drive
refused your write because it was busy tidying up" is not an acceptable answer.
Making space is a *precondition* of accepting work, not a consequence of it.

**The retry loop.** If the media fails a program, the offending block has
already been retired by the time we get back here (document 06), so simply
doing the operation again lands it somewhere else. Bounded, because each retry
costs a block — an unbounded loop on a dying drive would retire the whole
device.

**Statistics and checkpointing** at the end. `host_writes++` is the denominator
of write amplification, and every `FTL_CHECKPOINT_INTERVAL` writes triggers a
checkpoint (document 06).

---

## Step 2 — `gc_maybe_run()` in `src/gc.c`

```c
if (g_ftl.free_blocks > FTL_GC_LOW_WATERMARK) {
    return FTL_OK;                      /* plenty of room, do nothing */
}

while (g_ftl.free_blocks < FTL_GC_HIGH_WATERMARK) {
    before = g_ftl.free_blocks;
    err = gc_collect_one();
    ...
    if (g_ftl.free_blocks <= before) break;   /* no progress — stop */
}
```

On most writes this returns immediately after one comparison. It is the common
case and it must be cheap.

When it does fire, it collects until the pool reaches the **high** watermark —
the hysteresis from document 04.

The no-progress guard matters: if a collection frees a block but the pool
doesn't grow, every survivor consumed exactly as much space as was freed, which
means the drive is genuinely full of live data. Stop rather than spin burning
erase cycles.

Inside `gc_collect_one()`:

```c
victim = gc_select_victim();                        /* the policy */
mapping_relocate_block(victim, &stats.pgm_gc);      /* move survivors */
if (g_ftl.blk[victim].valid_pages != 0) return FTL_ERR_CORRUPT;
return blockmgr_release(victim);                    /* erase it */
```

Note the check between relocating and erasing. If relocation claims to have
emptied the block but the valid count says otherwise, we **refuse to erase**.
Returning an error here rather than erasing anyway is the difference between a
failing test and a data-loss bug.

---

## Step 3 — `mapping_write()` dispatches

```c
switch (g_ftl.params.mapping) {
case FTL_MAP_PAGE:   return page_write(lpn, buf);
case FTL_MAP_BLOCK:  return block_write(lpn, buf);
case FTL_MAP_HYBRID: return hybrid_write(lpn, buf);
}
```

One line per scheme. We follow the page-mapped path; the other two are in
`mapping.c` sections 5 and 6.

---

## Step 4 — `page_write()`, the five lines that matter

```c
static ftl_err_t page_write(uint32_t lpn, const void *buf)
{
    err = mapping_program_at_frontier(lpn, buf, NAND_TAG_USER_DATA, &new_ppn);
    if (err != FTL_OK) return err;
    g_ftl.stats.pgm_host++;

    /* Order matters. The new page is on the media BEFORE we drop the old one. */
    old_ppn = g_ftl.l2p.page[lpn];
    mapping_invalidate_ppn(old_ppn);

    g_ftl.l2p.page[lpn] = new_ppn;
    mapping_validate_ppn(new_ppn);

    return FTL_OK;
}
```

This is document 03 made concrete. Read the comment in the real file — the
ordering argument is the single most important sentence in the codebase.

If power dies between the program and the invalidate, two physical pages claim
LPN 42. Recovery picks the higher sequence number, which is the new one.
Correct. Reverse the order and there's a window where LPN 42 has no valid copy
at all. Data loss.

---

## Step 5 — finding a free page: `blockmgr_next_free_ppn()`

```c
uint32_t blockmgr_next_free_ppn(void)
{
    blockmgr_open_active_block();
    if (g_ftl.active_pbn == PBN_INVALID) return PPN_INVALID;
    return BLOCK_OFFSET_TO_PPN(g_ftl.active_pbn, g_ftl.active_off);
}
```

The FTL keeps **one open block** — the *write frontier*. Every page lands at
its write pointer. When it fills, `blockmgr_open_active_block()` closes it
(state becomes `BLK_DATA`, making it a GC candidate) and allocates another.

This is why writes are sequential at the chip even when the host is writing
randomly (document 03, section 6).

Allocation goes through `wear_pick_free_block()` in `src/wear.c`:

```c
for (pbn = 0; pbn < NAND_NUM_BLOCKS; pbn++) {
    if (!is_allocatable(pbn)) continue;
    ec = nand_block_erase_count(pbn);
    if (ec < best_ec) { best_ec = ec; best = pbn; }
}
```

The least-worn free block wins. Every allocation — host, GC, metadata — comes
through this one function, which is what makes dynamic wear levelling actually
work (document 05).

---

## Step 6 — talking to the chip: `program_into_block()`

```c
off = nand_block_write_pointer(pbn);         /* where the chip will accept a write */
ppn = BLOCK_OFFSET_TO_PPN(pbn, off);

spare.lpn = lpn;                             /* the margin note */
spare.seq = ++g_ftl.seq;                     /* monotonic; newest wins at recovery */
spare.tag = tag;

nerr = nand_program_page(ppn, buf, &spare);
```

Two things worth stopping on.

**The caller cannot choose the offset.** It's wherever the block's write pointer
happens to be. That isn't an API weakness — it's Rule 2 from document 02
surfacing in the interface, which is exactly where it should surface.

**The spare area is filled in here.** `lpn` and `seq` are what make the media
self-describing (document 06). This is the single line that makes power-loss
recovery possible, and it costs nothing.

---

## Step 7 — the chip: `nand_program_page()` in `src/nand.c`

The bottom of the stack. The fake silicon:

```c
off = PPN_TO_OFFSET(ppn);

/* RULE 1: program-once. */
if (off < g_blocks[pbn].write_pointer)  return NAND_ERR_NOT_ERASED;

/* RULE 2: sequential programming. */
if (off > g_blocks[pbn].write_pointer)  return NAND_ERR_OUT_OF_ORDER;

g_counters.programs++;
g_counters.busy_us += NAND_TPROG_US;        /* 600 µs */

if (rng_hit_ppm(g_program_fail_ppm)) {      /* RULE 5: programs sometimes fail */
    g_counters.program_failures++;
    g_blocks[pbn].write_pointer = off + 1;  /* the slot is consumed anyway */
    return NAND_ERR_PROGRAM_FAIL;
}

memcpy(g_page_data[ppn], data, NAND_PAGE_SIZE_BYTES);
g_page_spare[ppn]     = *spare;
g_page_spare[ppn].crc = nand_crc32(...);
g_blocks[pbn].write_pointer = off + 1;
```

Both rules from document 02, enforced as hard errors. **This is what makes the
project honest.** A permissive simulator would let firmware get away with things
real silicon rejects, and the code would be unshippable while passing every
test.

Note also that on failure the write pointer still advances — the slot is gone,
as on real hardware — and the block is *not* retired here. That decision belongs
to firmware (document 06).

---

## Step 8 — back up, updating the bookkeeping

```c
void mapping_validate_ppn(uint32_t ppn) {
    if (!BITMAP_GET(g_ftl.page_valid, ppn)) {
        BITMAP_SET(g_ftl.page_valid, ppn);
        g_ftl.blk[PPN_TO_BLOCK(ppn)].valid_pages++;
    }
}
```

Two structures kept in step: a **bitmap** over physical pages (1 bit each, 1 KiB
total) and a **per-block valid count**. The count is what garbage collection
reads to pick a victim — O(1), without touching the media at all.

The invariant checker later cross-checks the two against each other, and against
the mapping table. Three representations of the same fact, verified to agree.

---

## The read path, briefly

Much simpler:

```c
ppn = g_ftl.l2p.page[lpn];
if (ppn == PPN_INVALID) return FTL_ERR_UNWRITTEN;
return read_verified(ppn, lpn, buf);
```

and `read_verified()` does the thing worth copying into your own code:

```c
nand_read_page(ppn, buf, &spare);
if (spare.lpn != expect_lpn) return FTL_ERR_CORRUPT;
```

It checks the page *claims to be* the LPN we asked for. The spare area was read
anyway, so the check is free — and it converts the worst class of bug, silently
returning someone else's data, into an immediate loud failure.

---

## Tracing it yourself

Two ways to watch this happen.

**With the debugger:**

```sh
gdb ./ftlsim
(gdb) break page_write
(gdb) run --mapping page --ops 100 --quiet
(gdb) print lpn
(gdb) print g_ftl.l2p.page[lpn]      # where it lives now
(gdb) finish
(gdb) print g_ftl.l2p.page[lpn]      # where it lives after — different!
```

That last pair of prints *is* the out-of-place update, on screen.

**With the counters:**

```sh
./ftlsim --mapping page --ops 1000 --fill 0 --quiet
```

A fresh, empty drive: 1,000 host writes, ~1,000 programs, WA = 1.00, zero GC.
Then:

```sh
./ftlsim --mapping page --ops 1000 --fill 100 --quiet
```

Same 1,000 writes onto a *full* drive: now GC runs, `pgm_gc` is non-zero, and WA
climbs past 2. The difference between those two commands is the entire subject
of document 04.

---

## Check yourself

1. Why do `gc_maybe_run()` and `wear_maybe_level()` run *before* the write?
2. In `page_write()`, why must the program happen before the invalidate?
3. Why can't the caller of `program_into_block()` choose the page offset?
4. What two fields go into the spare area on every write, and what are they for?
5. Which two NAND rules does `nand_program_page()` enforce, and what does it
   return for each?
6. What are the two bookkeeping structures updated after a successful write, and
   which one does GC read?
7. What does `read_verified()` check that a naive read wouldn't, and why is it
   free?

**Next:** [09_RESULTS.md](09_RESULTS.md) — the numbers.
