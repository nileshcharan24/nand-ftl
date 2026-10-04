# 01 — Why This Project Exists

*Assumes: nothing. Read this first.*

---

## 1. What is inside a pen drive?

Open one up and you find two chips.

```
        ┌──────────────────────────────────────────┐
        │              PEN DRIVE / SSD             │
        │                                          │
   USB  │   ┌──────────────┐    ┌──────────────┐   │
  ──────┼──▶│  CONTROLLER  │───▶│  NAND FLASH  │   │
        │   │  (a tiny     │    │  (the chips  │   │
        │   │   computer)  │    │   that hold  │   │
        │   │              │    │   the bits)  │   │
        │   └──────────────┘    └──────────────┘   │
        └──────────────────────────────────────────┘
```

**The NAND flash chips** hold your data. They are just memory — they cannot
think. They accept three commands and nothing else: *read this page*,
*write this page*, *erase this block*.

**The controller** is a small, real computer. It has a CPU (often an ARM core),
a little RAM, and it runs a program burned into it. That program is called
**firmware**.

That word is worth pinning down, because you are applying for firmware roles:

| Term | What it means |
|---|---|
| **Software** | A program running on a general computer, on top of an OS. Word, Chrome, your Python scripts. |
| **Firmware** | A program running on a small dedicated device, usually with no OS underneath, controlling hardware directly. The code inside your SSD, your router, your microwave. |
| **Embedded** | The broad field of building such devices. "Embedded" because the computer is buried inside a product that isn't sold as a computer. |

An SSD's firmware is genuinely hard software. It manages gigabytes of data, has
to be correct under power failure, has to be fast, and has to run in a few
hundred kilobytes of RAM with no `malloc`, no operating system, and no ability
to ship a patch easily.

---

## 2. What the host expects

Your operating system — Windows, Linux, whatever — thinks about storage in the
simplest possible way. It thinks the drive is a very long numbered shelf:

```
  block 0     block 1     block 2     block 3    ...   block 999999
 ┌─────────┬───────────┬───────────┬───────────┬─────┬─────────────┐
 │  data   │   data    │   data    │   data    │ ... │    data     │
 └─────────┴───────────┴───────────┴───────────┴─────┴─────────────┘
```

and it issues exactly two kinds of request:

- **"Read block 12345."**
- **"Write these bytes to block 12345."**

That's it. This model comes from spinning hard disks, where it was literally
true — block 12345 was a specific physical patch of magnetic surface, and you
could rewrite it as many times as you liked, in place, forever.

Those numbered slots are called **LBAs** — Logical Block Addresses. In this
project we call the same idea an **LPN**, a Logical Page Number.

---

## 3. Flash cannot do that

Here is the whole problem in one sentence:

> **Flash memory cannot overwrite data in place.**

Not "it's slow". Not "it's discouraged". It is *physically impossible*. The
next document explains exactly why, at the level of electrons on a floating
gate. For now, take it on trust and sit with the consequence:

- The host says **"overwrite block 12345"**.
- The flash chip **cannot do that**.
- Something has to bridge the gap.

That something is the **Flash Translation Layer**.

---

## 4. What the FTL does, in one picture

The FTL keeps a table. The host's address goes in; the real address comes out.

```
   HOST                    FTL                        NAND FLASH
   ────                    ───                        ──────────

  "write LPN 42"  ──▶  look up LPN 42            ┌───────────────┐
                       in the mapping table      │ physical page │
                       ↓                         │      0        │
                  it currently lives at          │      1        │
                  physical page 700              │     ...       │
                       ↓                         │     700  ◀────┼── old copy
                  DON'T touch page 700.          │     ...       │   (now dead)
                  Write the new data to          │     901  ◀────┼── new copy
                  a fresh page, say 901.         │     ...       │
                       ↓                         └───────────────┘
                  update the table:
                  LPN 42 → physical page 901
                       ↓
                  mark page 700 as dead
```

The host never learns any of this happened. It asked to overwrite address 42,
and as far as it can tell, address 42 was overwritten.

That indirection is the single idea the entire project is built on. Everything
in documents 04, 05 and 06 is a *consequence* of doing this — a mess that has to
be cleaned up afterwards.

---

## 5. So what is this project, concretely?

Two pieces of C code:

**A fake NAND flash chip** (`src/nand.c`). It pretends to be the memory chip.
It stores data in a big array, and — crucially — it **enforces all of flash's
awkward rules**. If your firmware tries to overwrite a page, the fake chip
returns an error, exactly as real silicon would. This is what makes the project
honest: it is not a simplified model that lets you cheat.

**A Flash Translation Layer** (everything else in `src/`). The real firmware.
It does the address translation, cleans up dead pages, spreads out the wear,
handles broken blocks, and rebuilds its state after power loss.

On top there is a benchmark program (`ftlsim`) that hammers the whole thing with
different workloads and prints measurements, and a test suite (`ftltest`) with
65 tests that prove it is correct.

---

## 6. Why simulate instead of using a real SSD?

Three reasons, and this is a likely interview question:

**You can see inside.** On a real drive you cannot ask "how many times has
physical block 57 been erased?" The firmware is proprietary and the chip does
not tell you. In a simulator every counter is visible, so you can *measure*
write amplification rather than guess at it.

**You can break things on demand.** This project can make the fake chip fail a
write operation with a probability you choose, and can simulate the power being
cut at any instant. Doing that to real hardware means physically pulling cables
thousands of times and hoping you hit the right microsecond.

**You can change the physics.** Want to know what happens with 7% spare capacity
instead of 25%? Change one number and rebuild. On real hardware that is a new
product.

---

## 7. Why this project is a good fit for a storage firmware role

The job description for a firmware engineer at a company like SanDisk or Western
Digital asks for: C, data structures, algorithms, understanding of the data
path, memory and performance constraints, unit tests, and debugging.

This project is all of those at once, applied to *exactly* the product those
companies make:

| They want | This project has |
|---|---|
| C in constrained environments | Pure C99, **zero `malloc`** — every buffer is a fixed-size static array, as on a real controller |
| Data structures | Mapping tables, free-block pools, validity bitmaps, log-block structures |
| Algorithms | Garbage-collection victim selection, wear-levelling policies, recovery scans |
| Data path understanding | The read/write path from host command down to the chip |
| Performance optimisation | Write amplification measured and reduced from 64× to 2.7× |
| Unit tests and test scripts | 65 tests, 673,930 assertions, plus a Python workload generator |
| Debugging | Fault injection, invariant checkers, deliberate power-cut testing |

---

## 8. Check yourself before moving on

You should be able to answer these. If not, re-read.

1. What are the two chips inside a pen drive, and what does each do?
2. What is the difference between software and firmware?
3. What does the host operating system think a storage device looks like?
4. What is the one thing flash cannot do that the host assumes it can?
5. In one sentence, what does the FTL do about that?

**Next:** [02_HOW_FLASH_WORKS.md](02_HOW_FLASH_WORKS.md) — the physics, and the
three rules everything else follows from.
