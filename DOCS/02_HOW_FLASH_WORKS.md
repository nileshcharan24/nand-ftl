# 02 — How NAND Flash Actually Works

*Assumes: document 01. **This is the most important document in the folder.**
Everything else is a consequence of what's here.*

---

## 1. One memory cell

A flash memory cell is a single transistor with an extra piece of metal buried
inside it, completely surrounded by insulating glass. That buried metal is
called the **floating gate**.

```
              control gate
        ═══════════════════════
        ░░░░░ insulator ░░░░░░░
        ▓▓▓▓ FLOATING GATE ▓▓▓▓   ◀── electrons get trapped in here
        ░░░░░ insulator ░░░░░░░
    ────┬───────────────────┬────
   source│    silicon        │drain
```

Because the floating gate is wrapped in insulator, **any electrons you push onto
it are stuck there** — they cannot leak away. Not when you switch the power off,
not for years. That is what makes flash *non-volatile*: it remembers without
electricity.

Reading the cell means checking whether electrons are present:

- **No electrons trapped** → reads as **1**
- **Electrons trapped** → reads as **0**

Two operations move electrons:

| Operation | What happens | Effect on the bit |
|---|---|---|
| **Program** | A high voltage forces electrons *onto* the floating gate | 1 → 0 |
| **Erase** | A high voltage in reverse pulls electrons *off* | 0 → 1 |

Notice the asymmetry, because it is the root of everything:

> **Programming can only turn 1s into 0s. Only an erase can turn a 0 back into a 1.**

An erased cell is a 1. Writing means selectively knocking cells down to 0. You
can never push a cell back up to 1 by writing — you have to erase.

---

## 2. Why you can't erase one cell

You could, in principle, wire every cell so it could be erased individually. But
that needs extra wires per cell, and the whole appeal of flash is packing
billions of cells into a few square millimetres as cheaply as possible.

So manufacturers share the erase circuitry across a large group of cells. The
group that shares one erase gate is called a **block**, and erasing is
all-or-nothing across it.

This is a deliberate cost/density trade-off, not an oversight. Fine-grained
erase would make flash bigger, slower and far more expensive per gigabyte.

---

## 3. Pages and blocks

Cells are organised into a two-level hierarchy:

```
  ONE BLOCK  (the unit of ERASE — 256 KiB in this project)
 ┌──────────────────────────────────────────────────┐
 │  page 0    │ 4096 bytes │  ◀── unit of READ and WRITE
 │  page 1    │ 4096 bytes │
 │  page 2    │ 4096 bytes │
 │   ...      │            │
 │  page 63   │ 4096 bytes │
 └──────────────────────────────────────────────────┘

  THE WHOLE CHIP  =  128 blocks  =  8192 pages  =  32 MiB
```

| Unit | Size here | What you can do with it |
|---|---|---|
| **Page** | 4096 bytes | **Read** it, **program** (write) it |
| **Block** | 64 pages = 256 KiB | **Erase** it |

**You read and write pages. You erase blocks. The block is 64× bigger.**
That mismatch is called the **read/write/erase asymmetry**, and it is the reason
the FTL exists.

> Real drives are bigger — a modern SSD might have 16 KiB pages, 1024 pages per
> block, and hundreds of thousands of blocks. The *ratios* are what matter, and
> they're the same. This project uses small numbers so experiments finish in
> seconds. See `include/ftl_config.h` — change one line and the whole device
> resizes.

### Back to the notebook

- A **page** is a **line** you write on.
- A **block** is a **sheet of paper** holding 64 lines.
- Writing a line: fine, once.
- Changing a line: impossible. The only eraser wipes the entire sheet.

---

## 4. The spare area (the margin)

Each page has a few extra bytes beyond the 4096 — the **spare area**, also
called **OOB** (out-of-band). Real chips give you 64–224 bytes.

The controller uses part of it for error-correction parity. Firmware uses the
rest for its own notes. In this project (`nand_spare_t` in `include/nand.h`)
each page's spare area holds:

```c
typedef struct {
    uint32_t lpn;   /* which logical page this physical page holds */
    uint32_t seq;   /* a global counter — bigger means newer       */
    uint32_t crc;   /* integrity check                             */
    uint32_t tag;   /* what kind of page this is                   */
} nand_spare_t;
```

That `lpn` field is doing something clever, and it becomes the hero of document
06. In notebook terms: **every time you write a line, you also scribble the
chapter number in the margin.** If you ever lose your index, you can rebuild it
by reading all the margins.

---

## 5. The three rules

Here they are. Memorise these three. Every design decision in this project
traces back to one of them.

### Rule 1 — Program once

> After a block is erased, each page in it may be programmed **exactly once**.
> To write that page again, the **whole block** must be erased first.

Why: programming pushes electrons onto floating gates. There is no per-page way
to pull them off. Trying to program an already-programmed page just adds more
electrons to a cell that already has them — you cannot turn a 0 back into a 1.

In the code, `nand_program_page()` in `src/nand.c` returns `NAND_ERR_NOT_ERASED`
if you try. The fake chip refuses, exactly like real silicon.

### Rule 2 — Program pages in order

> Within a block you must program page 0, then page 1, then page 2… You may not
> skip ahead and come back.

Why: on modern multi-level chips, programming a high page disturbs the cells of
the lower pages that haven't been written yet. Write out of order and you
corrupt data.

This one catches people out. It means a block has a **write pointer** — a
position that only moves forward. In the code, `nand_block_write_pointer()`
tracks it and `nand_program_page()` returns `NAND_ERR_OUT_OF_ORDER` if you try
to jump.

> A nice consequence used in this project: because programming is strictly
> sequential, the write pointer alone tells you which pages hold data. No
> separate "is this page written?" bitmap is needed — that's 8 KiB of RAM saved.

### Rule 3 — Blocks wear out

> Every erase damages the insulator slightly. After a few thousand
> program/erase cycles the block can no longer hold charge reliably and must be
> retired.

Typical endurance ratings:

| Flash type | Bits per cell | P/E cycles |
|---|---|---|
| SLC | 1 | 60,000–100,000 |
| MLC | 2 | 3,000–10,000 |
| TLC | 3 | 1,000–3,000 |
| QLC | 4 | 150–1,000 |

More bits per cell means cheaper storage but far less endurance, because you're
distinguishing more voltage levels in the same physical range, so less
degradation is tolerable. This project uses **3,000** (`NAND_MAX_ERASE_CYCLES`),
which is MLC territory.

**This rule is why wear levelling exists** (document 05). Erases are a finite
resource that you are spending.

---

## 6. Two more physical annoyances

Not part of the core three, but both are implemented here and both make good
interview material.

**Read disturb.** Reading a page applies voltage across the whole word line,
which slightly nudges the cells in *other* pages of the same block. Read a block
enough times without erasing it and neighbouring pages start returning bit
errors. The fix is unglamorous: rewrite the block somewhere else, which resets
its disturb budget.

> The surprising consequence, worth being able to state: **a purely read-only
> workload can still cause flash writes and erases.** This project implements it
> — `nand_block_read_disturb_exceeded()` and the refresh path in `src/gc.c`.

**Blocks fail.** Some blocks are bad when the chip leaves the factory —
guaranteeing a perfect die would mean throwing away most of the wafer, so
manufacturers instead guarantee "no more than X% bad" and mark them. Others fail
during normal use. Document 06 covers both.

---

## 7. How long things take

Real MLC datasheet figures, used in this project (`include/ftl_config.h`):

| Operation | Time | Relative |
|---|---|---|
| Read a page | 50 µs | 1× |
| Program a page | 600 µs | **12×** |
| Erase a block | 3,000 µs | **60×** |

Reading is cheap. Writing costs 12 reads. Erasing costs 60 reads.

That ratio is why the numbers in document 09 matter. Every avoidable erase is
worth 60 reads of saved time — *and* a chunk of the block's finite lifetime.

---

## 8. Putting it together: why the host's model is impossible

The host says **"overwrite LBA 12345 with new data."**

Try to do that literally:

1. LBA 12345 currently lives at some physical page. Say block 7, page 20.
2. Rule 1 says you cannot program page 20 again.
3. So you must erase block 7 first.
4. But block 7 holds 63 other pages of perfectly good, unrelated data.
5. So first copy those 63 pages somewhere safe, then erase, then write all 64
   back including the new one.
6. That is 63 reads + 64 writes + 1 erase — call it 100 milliseconds — for one
   4 KiB write. And you just spent one of block 7's 3,000 lifetimes.

Doing this for every write would make the drive absurdly slow and burn it out in
days.

**So firmware does something else entirely, and that is document 03.**

---

## 9. Where this lives in the code

| File | What it does |
|---|---|
| `include/nand.h` | The interface of the fake chip. Read the comments — they explain each rule at the point it's enforced. |
| `src/nand.c` | The implementation. The array holding the data, the write pointers, the erase counters, the rule enforcement. |
| `include/ftl_config.h` | Every geometry and timing number in one place. |
| `tests/test_nand.c` | 12 tests proving the fake chip is faithful — including that it *refuses* illegal operations. |

Worth noticing in `nand.c`: an erased page is filled with **`0xFF`**, not zeros —
because an erased cell reads as 1. Small detail, but real firmware detects an
erased page by checking for all-`0xFF`, and a simulator that zero-filled would
let that class of bug slip through.

---

## 10. Check yourself

1. What are the two operations that move electrons, and which direction does
   each move a bit?
2. Why can't you erase a single page?
3. What are the three rules? State each in one sentence.
4. What is the write pointer and which rule forces it to exist?
5. Why does a read-only workload sometimes cause writes?
6. Roughly how much slower is an erase than a read?
7. Walk through why literally overwriting one 4 KiB page would cost ~100 ms.

**Next:** [03_WHAT_AN_FTL_DOES.md](03_WHAT_AN_FTL_DOES.md) — the trick that makes
all of this workable.
