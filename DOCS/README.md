# NAND Flash Translation Layer — Documentation Index

> Comprehensive, beginner-friendly documentation for understanding this project
> in full depth and explaining it confidently in interviews. **Assumes no prior
> knowledge of embedded systems or storage.**

---

## What this project is, in one paragraph

Inside every SSD and pen drive there are two chips: the NAND flash memory that
holds your data, and a small computer called a controller that runs firmware.
Flash memory has three awkward physical rules — you can write a page only once,
you can only erase a whole block of 64 pages at a time, and blocks wear out
after a few thousand erases — which together mean **you cannot overwrite data in
place**. The firmware layer that hides this from your operating system, so
Windows can say "write to block 12345" and not care, is called the **Flash
Translation Layer (FTL)**. This project is an FTL written from scratch in C,
running against a NAND flash chip that is also simulated from scratch and which
enforces every one of those real hardware rules. On top of address translation
it implements garbage collection, wear levelling, bad-block management and
power-loss recovery, and it *measures* the trade-offs between them — which is
something you cannot do on a real drive, because the firmware is proprietary and
the chip won't tell you.

---

## → Start at [00_START_HERE.md](00_START_HERE.md)

That document contains the full study flow. Read the documents in order; each
assumes only what the previous ones taught you. Roughly 3–4 hours total.

| Stage | Documents | You will understand |
|---|---|---|
| **1. The problem** | [01](01_WHY_THIS_EXISTS.md) · [02](02_HOW_FLASH_WORKS.md) · [03](03_WHAT_AN_FTL_DOES.md) | What flash is, why it's awkward, and the one trick that makes it usable |
| **2. The consequences** | [04](04_GARBAGE_COLLECTION.md) · [05](05_WEAR_LEVELLING.md) · [06](06_BAD_BLOCKS_AND_POWER_LOSS.md) | Garbage collection, write amplification, wear, failure, power loss |
| **3. Your code** | [07](07_REPO_TOUR.md) · [08](08_CODE_WALKTHROUGH.md) | What every file does, and one write traced from host to chip |
| **4. Being questioned** | [09](09_RESULTS.md) · [10](10_INTERVIEW_QA.md) | The measured numbers, and the answers — including honest weaknesses |

[**GLOSSARY.md**](GLOSSARY.md) — every term, one line each. Keep it open in a
second window.

---

## Quick facts

| | |
|---|---|
| Language | C99, `-Wall -Wextra -Werror -Wpedantic`, no compiler extensions |
| Memory | **Zero `malloc`.** Every buffer is a fixed-size static array. Whole FTL state: 28,824 bytes |
| Simulated device | 128 blocks × 64 pages × 4 KiB = 32 MiB physical, 23.25 MiB exposed (25% over-provisioned) |
| Mapping schemes | Page, block, hybrid (BAST) — selectable at runtime |
| GC policies | Greedy, cost-benefit, FIFO, random |
| Wear levelling | None, dynamic, static |
| Reliability | Factory + grown bad blocks, fault injection, read-disturb refresh |
| Power-loss recovery | Self-describing media via spare area, plus periodic checkpoints |
| Tests | **65 tests, 673,930 assertions, all passing** |
| Tooling | Makefile, Python workload generator, experiment sweep script |

## Headline measurements

All on a preconditioned (100% full) drive, 50,000 host writes, seed 1.
Full detail and reproduction commands in [09_RESULTS.md](09_RESULTS.md).

| Finding | Number |
|---|---|
| Write amplification, page mapping, random writes | **2.70** |
| Write amplification, block mapping, same workload | **64.00** |
| Mapping table size, page vs block | 23,808 B vs 372 B |
| Cost-benefit vs greedy GC on hot-cold workload | **2.17 vs 2.76** (21% better) |
| Random GC policy (the control group) | **5.13** |
| Sequential write workload | **1.00** |
| Erase-count spread: no levelling → static | **50 → 18** |
| Minimum erase count: no levelling → static | **0 → 10** |
| Data lost across unclean power cuts (3 schemes) | **0 pages** |
| Mount scan: from checkpoint vs full scan | **93 vs 6,045 pages** |

---

## Build and run

```sh
make            # build ftlsim and ftltest
make test       # run the 65 tests
./ftlsim --help # see all benchmark options

./ftlsim --mapping page --gc greedy --wear dynamic \
         --workload rand --ops 50000 --seed 1 --verify
```

---

## One-line interview summary

> "A Flash Translation Layer written from scratch in C — the firmware inside an
> SSD that lets a host address flash as a disk despite flash being unable to
> overwrite in place — with page, block and hybrid address mapping, four garbage
> collection policies, static and dynamic wear levelling, bad-block retirement
> and power-loss recovery, running against a simulated NAND die that enforces
> the real program-once and erase-at-block-granularity rules, with write
> amplification measured at 2.70 for page mapping against 64.00 for block
> mapping under random writes."
