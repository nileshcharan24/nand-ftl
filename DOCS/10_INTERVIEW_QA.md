# 10 — Interview Preparation, A to Z

> **You have limited time and you have not read the source code.** This document
> is built for exactly that situation. Everything here is defensible from the
> concepts in documents 01–07, which you have already read.

---

## How to use this in 25 minutes

| Minutes | Read | Why |
|---|---|---|
| 0–2 | **Part 1 — Scope** | Decide what you're claiming *before* you speak. Non-negotiable. |
| 2–8 | **Part 2 — The 60-second answer** | Memorise this. It is the single highest-value thing on this page. |
| 8–12 | **Part 3 — The whiteboard picture** | One diagram that answers half the follow-ups. |
| 12–20 | **Part 5 — The concept chain** | The eight beats, in order. Say them as a story, not a list. |
| 20–24 | **Part 7 — Five numbers** | Memorise five. Not more. |
| 24–25 | **Part 10 — Traps** | Three things not to say. |

Skip Parts 4, 6, 8, 9 on a first pass. Come back to them when you have an hour.

---

# PART 1 — What you are claiming

Get this straight in your head before you open your mouth. The version below is
**smaller than the whole project, and that is deliberate.** A smaller claim you
can fully defend beats a bigger one you cannot.

### Say this, early and unprompted

> "The NAND flash simulator underneath — the part that models the physical chip
> — I adapted from existing open-source SSD simulator designs. What I built is
> the Flash Translation Layer on top of it: the address mapping, garbage
> collection, wear levelling, bad-block handling and power-loss recovery."

Two reasons this is the right move.

**It is the honest boundary and it sounds professional.** Nobody writes a NAND
model from nothing; FEMU and SSDsim are the standard references and every
firmware engineer knows them. Saying "I used a reference model for the media and
wrote the firmware" is what a real engineer would say.

**It cuts your defence surface in half.** You will not be asked to justify
timing constants or the CRC implementation. You will be asked about the FTL,
which is what you actually understand.

### The one rule

> **Do not claim anything you cannot explain or change on the spot.**

If you are asked "how would you modify X" and the honest answer is that you'd
need to re-read the file, say so plainly: *"I'd need to look at the code, but
the logic is — "* and then explain the logic. That is a completely normal
answer. Bluffing is the only fatal move.

---

# PART 2 — "Explain your project"

## The 60-second answer — memorise this

Say it in five beats. **Problem → Core idea → Consequences → Numbers → Scope.**

> **[Context]** "It's SSD firmware. Inside every SSD there's a flash memory chip
> and a small controller running firmware, and the most important part of that
> firmware is the Flash Translation Layer.
>
> **[Problem]** The reason it exists is that NAND flash can't overwrite data in
> place. You can write a page once, but to write it again you have to erase the
> entire block it sits in — and a block is 64 pages. So the host says 'overwrite
> address 42' and the chip physically cannot do it.
>
> **[Core idea]** So the FTL never overwrites. It writes the new data to a fresh
> page somewhere else and updates a table that maps the host's logical address
> to the actual physical location. That's the core of the whole thing — an
> indirection table.
>
> **[Consequences]** Everything else falls out of that. Out-of-place writes
> leave dead pages behind, so you need a garbage collector to copy the survivors
> out of a block and erase it. Those copies are writes the host never asked for
> — that's write amplification, the main metric in storage firmware. Erasing
> wears the block out, so you need wear levelling to spread erases evenly.
> Blocks fail, so you need bad-block retirement. And the mapping table lives in
> RAM, so on a power cut it's gone — I made the media self-describing by writing
> the logical address into each page's spare area, so the table can be rebuilt
> by scanning.
>
> **[Numbers]** I implemented three mapping schemes and measured them. Page-level
> mapping gave write amplification of 2.7 under random writes; block-level
> mapping gave 64 — but block mapping uses 64 times less RAM for the table.
> That trade-off is the central design decision in an FTL.
>
> **[Scope]** The NAND chip model I adapted from open-source SSD simulators. The
> FTL on top is mine."

Time it. It should land around 60–75 seconds. **Then stop talking** and let them
pick a thread.

## When they say "go deeper"

Do not restart. Pick the thread they seemed interested in, or default to
**garbage collection** — it's the richest and it leads naturally to write
amplification and over-provisioning.

---

# PART 3 — The one picture to draw

If there's a whiteboard, draw this while delivering the "core idea" beat. It
pre-answers a dozen follow-ups.

```
   HOST                  FTL                       NAND FLASH
                                             ┌──────────────────┐
  "write LPN 42" ──▶  mapping table          │  block 0         │
                      ┌──────────┐           │   page 0  [dead] │
                      │ 42 → 901 │           │   page 1  [live] │
                      │ 43 → 128 │           │   ...            │
                      │ ...      │           ├──────────────────┤
                      └──────────┘           │  block 14        │
                           │                 │   page 700 [dead]│◀─ old copy of 42
                           │                 │   ...            │
                           └────────────────▶│   page 901 [live]│◀─ new copy of 42
                                             └──────────────────┘
                                              read/write = 1 page
                                              ERASE = whole block
```

Three labels to write and point at:

- **LPN** = the address the host uses. Never changes.
- **PPN** = where it actually is. Changes on **every** write.
- **The asymmetry**: read/write a page, **erase 64 pages**. ← *"this is why the whole layer exists"*

---

# PART 4 — The 3-minute version

If they want more, expand in this order. Same story, more detail.

1. **The three NAND rules** (30 s) — program once; program pages in order within
   a block; blocks die after ~3,000 erases.
2. **Out-of-place update and the L2P table** (30 s) — plus: this is why writes
   become sequential at the chip even when the host is random.
3. **Three mapping schemes and the RAM/WA trade-off** (45 s) — with the numbers.
4. **Garbage collection, victim selection, write amplification, over-provisioning** (45 s).
5. **Wear levelling: dynamic vs static, and the cold-data problem** (30 s).
6. **Bad blocks and power-loss recovery** (30 s).
7. **How I validated it** (20 s) — 65 tests, invariant checker, fault injection,
   20 consecutive simulated power cuts.

---

# PART 5 — The concept chain

**This is the spine of your preparation.** Eight beats, each one causing the
next. If you can recite the chain, you can answer almost anything, because every
question is somewhere on it.

> flash can't overwrite → **mapping** → dead pages → **garbage collection** →
> **write amplification** → **over-provisioning** → erases wear blocks →
> **wear levelling** → blocks fail → **bad blocks** → table is in RAM →
> **power-loss recovery**

## 1. Why flash can't overwrite *(the foundation)*

> "Writing pushes electrons onto a floating gate, which turns 1s into 0s. There's
> no per-page way to pull them back off — only a block-wide erase does that. So
> a page can be programmed exactly once per erase cycle. And the erase unit is
> 64× bigger than the write unit, because giving every page its own erase
> circuitry would make the chip much bigger and more expensive."

**Follow-up: "why is erase block-granular?"** Cost and density. Deliberate
trade-off, not an oversight.

## 2. Mapping — the core of the FTL *(you were right that this is the centre)*

> "The FTL keeps a logical-to-physical table. The host's address, the LPN, is
> stable for the life of the drive. The physical address, the PPN, changes on
> every single write. On a write I get a fresh page, program the data there,
> mark the old physical page invalid, and update the table entry. The host never
> knows anything moved."

**Follow-up: "what's the order of those steps and why?"** *(This is the best
single question you can be asked — have the answer ready.)*

> "Program the new page **before** invalidating the old one. If power dies
> between the two, recovery finds two pages claiming to be LPN 42, and it picks
> the one with the higher sequence number — the new one. If you do it the other
> way round, there's a window where that LPN has no valid copy at all. That's
> data loss."

## 3. Page vs block vs hybrid mapping

> "Page mapping is one table entry per page — any page can live anywhere, so
> random writes cost the same as sequential. Block mapping is one entry per
> block, 64× less RAM, but the page's offset within its block is fixed, so
> overwriting one page means rebuilding the whole block. I measured 2.70 write
> amplification for page mapping against 64.00 for block mapping — and 23,808
> bytes of table against 372."

> "Hybrid is the historical compromise: block-map the data, keep a small pool of
> page-mapped log blocks to absorb updates, merge them back periodically. Mine
> measured 59.51, barely better than block mapping — that's log-block thrashing,
> the known weakness of the BAST scheme. A random workload touches more logical
> blocks than there are log blocks, so nearly every write forces a merge."

**Why real drives use page mapping:** roughly 1 MB of table RAM per 1 GB of
flash, so a 1 TB drive needs ~1 GB of table — which is why SSDs have a DRAM chip
next to the controller, and why cheap DRAM-less drives are slower.

## 4. Garbage collection

> "Out-of-place writes leave invalid pages scattered everywhere. You can't reuse
> them individually — the only way to reclaim them is to erase the whole block,
> and the block usually still has live pages in it. So garbage collection picks
> a victim block, copies the surviving valid pages elsewhere, updates the
> mapping for each, and erases the block."

**Follow-up: "how do you pick the victim?"**

> "Greedy picks the block with the fewest valid pages, which minimises the copy
> cost right now. Cost-benefit also weights how long the block has sat untouched
> — the formula is (1−u)·age over 2u, where u is the valid fraction. The idea is
> that a long-idle block is cold, so the space you free from it stays free.
> I measured cost-benefit beating greedy by 21% on a hot-cold workload, and
> tying with it on uniform random — which is exactly right, because with no cold
> data the age term has nothing to say."

That last clause is worth saying deliberately. It shows you understand *why* the
policy works, not just that it does.

## 5. Write amplification

> "Physical writes divided by host writes. It's the headline metric because it
> drives all three things that matter: endurance — 3× the writes means a third
> of the lifetime; performance — the flash is busy doing GC instead of serving
> the host; and power."

## 6. Over-provisioning

> "The drive hides some capacity from the host — mine hides 25%. That hidden
> space is the slack garbage collection breathes with. At zero over-provisioning
> the drive is 100% full the moment the host writes every address once, and
> every subsequent write has to collect an almost-entirely-valid block, so write
> amplification runs away. The approximation is WA ≈ 1/(2·OP); at 25% that
> predicts 2.0 and I measured 2.70."

## 7. Wear levelling

> "Each erase damages the block slightly; after about 3,000 cycles it's dead.
> A drive dies when it runs out of usable blocks, so what matters isn't the
> average erase count, it's the **spread**."

> "Dynamic levelling is free — when you have to pick a free block anyway, pick
> the least-erased one. But it only ever sees blocks in the free pool, and a
> block holding cold data never becomes invalid, so garbage collection never
> collects it, so it never returns to the pool and never ages. Static levelling
> fixes that by deliberately relocating cold data off young blocks. It costs
> writes to buy endurance. I measured the erase spread dropping from 50 to 18,
> and the minimum erase count rising from 0 to 10 — that second number is the
> proof the cold data actually got unstuck — for about a 1% write-amplification
> cost."

**Follow-up: "does wear levelling reduce erases?"** No — it slightly increases
them. The claim is the same erases, spread evenly.

## 8. Bad blocks

> "Some blocks are marked bad by the manufacturer before shipping — guaranteeing
> a perfect die would mean throwing away most of the wafer, so they guarantee a
> maximum bad percentage instead. Firmware must find those marks at first
> power-on and never erase them, because erasing destroys the only record that
> the block is unusable. Other blocks fail during use."

**The part worth volunteering:**

> "The subtle bit is ordering. When a program fails partway through a block, the
> pages already written in it are usually still readable. So you read those out
> and relocate them **first**, and only then write the bad-block marker. Get it
> backwards and every retirement silently eats up to a block of user data."

## 9. Power-loss recovery

> "The mapping table is in RAM, so a power cut destroys it. The data's all still
> on the flash, but without the table you don't know what's where."

> "Two mechanisms. First, every page is programmed with its logical page number
> and a monotonically increasing sequence number in its spare area — the extra
> bytes attached to each page. That makes the media self-describing: you can
> rebuild the entire table by scanning and keeping the highest sequence number
> per logical page. And it's free, because the spare area gets programmed
> anyway. Second, periodic checkpoints of the whole table into a fixed metadata
> region, so you only have to scan pages written after the last checkpoint.
> Mounting from a checkpoint scanned 93 pages against 6,045 for a full scan."

**Follow-up: "how do you know a checkpoint is complete?"**

> "The header goes last. It carries the magic number, the sequence number and a
> CRC over the payload, so its presence on the media proves everything it
> describes made it. A torn checkpoint has no header and recovery just ignores
> it and uses the previous one. Same commit-record pattern as a journalling
> filesystem."

---

# PART 6 — Rapid-fire answers

Short. One or two sentences each.

**What's a page? A block?** Page = unit of read/write, 4 KiB. Block = unit of
erase, 64 pages.

**What's the spare area?** Extra bytes on each page beyond the user data. ECC
parity plus firmware bookkeeping — I store the LPN and a sequence number there.

**Why is a sequence number needed?** To decide which of two copies of the same
LPN is newer during recovery.

**What's TRIM?** The host telling the drive it will never read those addresses
again, so GC can stop preserving that data. NVMe calls it Deallocate.

**What's the write frontier?** The single open block that writes append to. It's
why writes hit the chip sequentially even under a random host workload.

**Why two GC watermarks?** Hysteresis. With one threshold you'd collect a block,
drop below the trigger on the very next write, and collect again — a per-write
tax instead of a background activity.

**Can a read-only workload cause writes?** Yes. Reading disturbs neighbouring
cells in the same block; past a threshold the block must be rewritten elsewhere
to reset its disturb budget.

**Why no malloc?** Real controllers have a few hundred KB of SRAM and no OS.
Everything is a fixed-size static array so the linker tells you whether the
image fits before it ships.

**How did you validate it?** 65 tests, ~674,000 assertions, an invariant checker
run every few hundred operations during stress tests, fault injection, and 20
consecutive simulated power cuts with full verification after each.

**What would you improve first?** Separate the GC write stream from the host
write stream. Right now both append to the same open block, so relocated cold
data gets interleaved with fresh host data that's about to be overwritten.
Keeping them apart makes future blocks more uniformly hot or cold, which
directly cuts future GC cost.

---

# PART 7 — Five numbers

Memorise these five. Not more. If pressed on anything else, say *"I'd have to
check the results table."*

| # | Number | The sentence it belongs to |
|---|---|---|
| 1 | **2.70 vs 64.00** | Write amplification, page vs block mapping, random writes |
| 2 | **64× RAM** | 23,808 bytes vs 372 bytes of mapping table — the trade |
| 3 | **21%** | Cost-benefit GC beat greedy on a hot-cold workload |
| 4 | **50 → 18** | Erase-count spread, no wear levelling → static |
| 5 | **0 pages lost** | Across unclean power cuts, on all three mapping schemes |

Two spares if the conversation goes there: **WA 1.00** for sequential writes,
and **65 tests / 674k assertions**.

**Always give the conditions.** Not "my WA was 2.7" but *"2.70 at 25%
over-provisioning, on a preconditioned drive, under 50,000 uniform random 4 KiB
writes."* A bare WA number is not a claim — WA depends on geometry,
over-provisioning, fill level, workload and seed. Volunteering the conditions
signals you know that.

---

# PART 8 — When they ask about the code

You have not read the source. Handle it like this.

## The safe pattern

**Describe the logic, not the lines.** *"The write path does X, then Y, then Z —
the ordering matters because…"* That is a better answer than reciting a function
name, and it's what they actually want.

## The eight names worth knowing

From document 07. Enough to sound oriented without pretending.

| File | What lives there |
|---|---|
| `nand.c` | The simulated chip — enforces program-once and sequential programming |
| `mapping.c` | **The core.** All three mapping schemes, the read/write/trim paths |
| `gc.c` | Victim selection and the collection loop |
| `wear.c` | Block allocation policy and static migration |
| `badblock.c` | Retirement, in the safe order |
| `journal.c` | Checkpoints and recovery |
| `ftl.c` | The public API and the invariant checker |
| `blockmgr.c` | Free-block pool and the write frontier |

## The one piece of code to be able to write

If handed a marker and asked "write the write path", this is it. Five lines,
plain pseudocode is fine:

```
write(lpn, data):
    if free_blocks < LOW_WATERMARK:  run_gc()

    ppn = next_free_page()                    # at the write frontier
    program(ppn, data, spare = {lpn, seq++})  # spare area makes it recoverable

    invalidate(table[lpn])                    # old copy is now dead
    table[lpn] = ppn                          # repoint
```

Then say the sentence that earns the marks:

> "The program has to happen before the invalidate. If power dies in between,
> two pages claim this LPN and recovery picks the higher sequence number — the
> new one. Reverse it and there's a window with no valid copy at all."

## If you genuinely don't know

> *"I don't remember that detail — let me tell you how I'd reason about it."*

Then reason about it out loud. Interviewers rate that far above a confident
wrong answer, and firmware interviewers in particular are testing whether you
panic.

---

# PART 9 — Honest limitations

Volunteer one or two unprompted. It signals you know the boundary of what you
built, which is a maturity marker.

**It's a simulator, not silicon.** No real bus, no channel or plane parallelism,
no real ECC engine. Real controllers interleave across 8–16 channels, and that
parallelism changes which optimisations matter.

**No NVMe or PCIe.** The API is shaped like NVMe commands — read, write,
deallocate, flush — but there's no queue pair, no doorbell register, no link
layer.

**Single-threaded.** No concurrency, so none of the hard parts: no locking, no
host command arriving mid-garbage-collection.

**Small device — 32 MiB.** Some things that are fine at this scale aren't at
real scale. The free-block allocator is an O(N) linear scan, which is nothing at
128 blocks and unacceptable at the ~256,000 blocks of a 1 TB drive. Real
firmware buckets free blocks by erase count for O(1) selection. *(Good to say —
it shows you know the difference between "works" and "scales".)*

**GC shares the write frontier with host writes.** Named above as the first
improvement.

**Hybrid mapping thrashes** at 59.51 WA. That's the known BAST failure mode;
FAST-style fully-associative log blocks would fix the thrashing at the cost of
much more expensive merges. I measured the weakness rather than hiding it.

**TRIM isn't journalled.** A TRIM followed by a power cut before the next
checkpoint resurrects the old data. NVMe permits this, but it's a real gap.

---

# PART 10 — Three things not to say

**Don't claim it's production firmware.** It's a faithful simulator of the
algorithms. That is plenty, and overclaiming invites a question you can't
answer.

**Don't quote a bare write-amplification number.** Always attach the conditions.

**Don't say you invented cost-benefit GC.** It comes from the Sprite
log-structured filesystem cleaner. Knowing the provenance is a point in your
favour, not against you.

And a fourth, since you'll be tempted: **don't over-explain the NAND simulator.**
You've already said you adapted it. If they push, describe what it enforces —
program-once, sequential programming, erase counters, injectable failures — and
move the conversation back to the FTL.

---

# PART 11 — The cram card

Read this alone in the last five minutes before you walk in.

**The chain:**
flash can't overwrite → mapping table → dead pages → garbage collection →
write amplification → over-provisioning → erases wear blocks → wear levelling →
blocks fail → bad blocks → table is in RAM → power-loss recovery

**The five beats of the pitch:**
Problem → Core idea (mapping) → Consequences → Numbers → Scope

**The five numbers:**
2.70 vs 64.00 · 64× RAM · 21% · 50 → 18 · 0 pages lost

**The three sentences that earn the most credit:**

1. *"Program the new page before invalidating the old one — otherwise a power
   cut leaves the LPN with no valid copy."*
2. *"Cost-benefit beat greedy by 21% on hot-cold and tied on uniform random,
   which is exactly what you'd expect, because with no cold data the age term
   has nothing to say."*
3. *"When a program fails you rescue the readable pages first, then mark the
   block bad — backwards, and every retirement eats a block of user data."*

**The scope sentence:**
*"NAND model adapted from open-source SSD simulators; the FTL on top is mine."*
