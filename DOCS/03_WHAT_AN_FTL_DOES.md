# 03 — What the FTL Actually Does

*Assumes: documents 01 and 02.*

---

## 1. The trick: stop trying to overwrite

Document 02 ended with a disaster: overwriting one page the honest way costs 63
reads, 64 writes, an erase, and 100 milliseconds.

The way out is almost embarrassingly simple.

> **Don't overwrite. Write the new data somewhere else, and remember where you
> put it.**

That's it. That's the Flash Translation Layer.

Writing the new version to a fresh location instead of the original is called an
**out-of-place update**. The cost is that you now have to *remember* where
everything went — which means keeping a table.

### In the notebook

You wrote "Chapter 5: page 12" on line 30. Chapter 5 has moved to page 47.

You cannot rub out line 30. So instead:

1. Write "Chapter 5: page 47" on the next blank line — line 88.
2. In the index at the front, change Chapter 5's entry to point at line 88.
3. Line 30 is now **garbage**. It still has ink on it, but nothing refers to it.

Done. Three cheap steps instead of recopying the whole sheet.

---

## 2. Two kinds of address

This is the vocabulary you must be fluent in.

| Term | Stands for | Who uses it | Meaning |
|---|---|---|---|
| **LPN** | Logical Page Number | The host | "The 42nd page of the drive, as far as Windows is concerned." A *name*. |
| **PPN** | Physical Page Number | The flash chip | "Page 901 of the actual silicon." A *place*. |
| **PBN** | Physical Block Number | The flash chip | "Block 14 of the actual silicon." |

The FTL's whole job is the arrow between the first two:

```
        LPN  ──────────▶  PPN
      (name)   mapping    (place)
                table
```

An LPN is stable — LPN 42 is always LPN 42, for the life of the drive. Its PPN
changes on **every single write**.

---

## 3. The three operations

### Write LPN 42

```
 1. Find a fresh, erased page.                      → say PPN 901
 2. Program the data into PPN 901,
    with lpn=42 written into its spare area.
 3. Look up what LPN 42 pointed at before.          → PPN 700
 4. Mark PPN 700 as INVALID (dead).
 5. Update the table:  LPN 42 → PPN 901.
```

**The order of steps 2 and 4 matters enormously.** The new copy goes onto the
media *before* the old one is disowned. If the power dies between them, there
are two pages claiming to be LPN 42 — and the one with the higher sequence
number wins, so recovery picks the new one. Do it the other way round and there
is a window where LPN 42 has no valid copy at all. That is data loss.

In the code: `page_write()` in `src/mapping.c`.

### Read LPN 42

```
 1. Look up the table:  LPN 42 → PPN 901.
 2. Read PPN 901.
 3. Check the spare area really says lpn == 42.
 4. Return the data.
```

Step 3 is free — you just read the spare area anyway — and it converts the worst
possible bug (silently handing back somebody else's data) into a loud error.
`read_verified()` in `src/mapping.c`.

### Trim LPN 42

The host says "I've deleted that file, you can forget LPN 42."

```
 1. Mark its current PPN invalid.
 2. Set the table entry to "nowhere".
```

This sounds trivial but is worth real money. Without TRIM the drive is
contractually obliged to preserve data nobody wants, and the garbage collector
wastes effort copying it around forever. Document 04 shows the measured effect.

---

## 4. Page states

At any moment every physical page is in one of three states:

| State | Meaning | Notebook |
|---|---|---|
| **Free** | Erased, ready to be programmed | A blank line |
| **Valid** | Programmed, and the mapping table points at it | A line holding current information |
| **Invalid** | Programmed, but superseded — nothing points at it | A line whose content has been rewritten elsewhere |

The lifecycle is a one-way street:

```
  FREE ──program──▶ VALID ──superseded──▶ INVALID ──┐
    ▲                                               │
    └──────────────── erase the whole block ────────┘
```

An invalid page is **not** reusable. It is dirty space. The only way back to
FREE is erasing its entire block — which is document 04.

> **A layering point worth making in an interview.** The NAND chip has no idea
> what "valid" means. It only knows programmed vs erased. Validity is purely an
> FTL concept, because only the FTL holds the mapping. In this project
> `src/nand.c` deliberately does *not* have a `PAGE_INVALID` state — many toy
> simulators put one there, and it's a layering violation that makes the design
> impossible to port to real hardware.

---

## 5. How big is the table?

This is the central design trade-off, and it's a favourite interview question.

The obvious scheme is **page-level mapping**: one table entry per logical page.

```
  entries = logical pages
  bytes   = entries × 4     (a PPN is a 32-bit integer)
```

For this project: 5,952 logical pages × 4 = **23,808 bytes**. Fine.

For a real 1 TB drive with 4 KiB pages: 268 million pages × 4 bytes ≈ **1 GB of
table**. You cannot put a gigabyte of SRAM on a controller. This is a genuine
industrial problem, and it's why the rule of thumb exists:

> **Roughly 1 MB of mapping RAM per 1 GB of flash.**

Modern SSDs solve it by soldering a DRAM chip next to the controller. Cheap
DRAM-less drives cannot, so they need something smaller.

### Block-level mapping

One entry per logical *block* — 64× smaller. **372 bytes** here instead of
23,808.

The catch: a page's offset within its block becomes **fixed**. Logical page
`lbn × 64 + k` must live at physical offset `k`. You lose the freedom to put
data anywhere.

Now consider overwriting one page in the middle of a full block. Offset `k` is
already programmed, and Rule 1 forbids reprogramming it. So you must allocate a
fresh block and rebuild the entire logical block into it — the exact disaster
from document 02.

**Measured in this project: block mapping gives write amplification of exactly
64.00 on random writes, against 2.70 for page mapping.** Sixty-four times more
flash traffic to save 64× the RAM. That is the trade, quantified.

### Hybrid mapping

The historical compromise: block-map the bulk of the data, but keep a small pool
of **log blocks** that are page-mapped and absorb updates. Periodically **merge**
a log block back into its data block.

This project implements it (BAST — Block-Associative Sector Translation, at most
one log block per data block). Its famous weakness is **log block thrashing**: a
workload touching more distinct logical blocks than there are log blocks forces
a merge on nearly every write. **Measured here: 59.51 write amplification under
random writes — barely better than block mapping**, which is exactly the
pathology the literature describes.

### The summary table

Real numbers from this project, 50,000 random writes, seed 1:

| Scheme | Table size | Write amplification | Verdict |
|---|---|---|---|
| **Page** | 23,808 B | **2.70** | What modern SSDs do. Costs RAM. |
| **Block** | 372 B | **64.00** | 64× less RAM, catastrophic on random writes. |
| **Hybrid** | 2,548 B | **59.51** | Meant to be the middle ground; thrashes here. |

Selectable at runtime: `./ftlsim --mapping page|block|hybrid`.

---

## 6. Why out-of-place updates are actually a *good* thing

It's easy to read all this as "flash is annoying, the FTL is damage control."
Half true. But note what the indirection buys you for free:

**Writes become sequential.** Every write goes to the next free page at the
write frontier, regardless of what logical address the host asked for. A wildly
random host workload becomes a neat sequential stream at the chip. Flash
strongly prefers that.

**Wear levelling becomes possible.** Because data isn't nailed to a location,
you can move it. Document 05 depends entirely on this.

**Bad blocks become survivable.** A block dying doesn't destroy an address —
just relocate its contents and repoint the table. Document 06.

A hard disk cannot do any of this. Sector 12345 is a physical patch of magnetic
surface, and if it goes bad, that address is gone.

---

## 7. Where this lives in the code

| File | Role |
|---|---|
| `include/ftl.h` | The public interface — `ftl_read`, `ftl_write`, `ftl_trim`, `ftl_flush`. Deliberately shaped like NVMe commands. |
| `src/mapping.c` | **The heart of the project.** All three mapping schemes. Start reading here. |
| `include/ftl_internal.h` | The shared state — the mapping table, block metadata, the validity bitmap. |
| `src/blockmgr.c` | The free-block pool and the write frontier: "give me a fresh page." |

Two things worth looking at in `ftl_internal.h`:

**The mapping table is a `union`**, not a struct — because a real product picks
one scheme and pays for one table. Writing it as a union means you cannot
accidentally read the block table while running page-mapped, and `sizeof` the
context reflects the worst case rather than the sum.

**Validity is a bitmap**, one bit per physical page: 8,192 pages → 1 KiB instead
of 8 KiB for a byte array. On a controller that difference is real money.

---

## 8. Check yourself

1. What does "out-of-place update" mean, and what problem does it solve?
2. What is the difference between an LPN and a PPN? Which one changes on a write?
3. Why must the new page be programmed *before* the old one is invalidated?
4. What are the three page states, and which transition needs a block erase?
5. Why doesn't the NAND layer know what "invalid" means?
6. Page vs block mapping: what is traded for what, and what were the measured
   numbers here?
7. Name two things out-of-place updates make *easier*, not harder.

**Next:** [04_GARBAGE_COLLECTION.md](04_GARBAGE_COLLECTION.md) — dealing with all
those dead pages, and the number that defines storage firmware.
