# 05 — Wear Levelling

*Assumes: documents 01–04.*

---

## 1. The problem, stated precisely

Rule 3 from document 02: **a block survives about 3,000 erases, then it's dead.**

Now the important bit, which is easy to get wrong:

> A drive dies when it **runs out of usable blocks** — not when the *average*
> block is worn out, but when enough *individual* blocks are.

So the enemy is not the mean erase count. **The enemy is the spread.**

### The arithmetic

128 blocks, each rated for 3,000 erases.

| Scenario | Total erases the drive absorbs |
|---|---|
| Wear spread perfectly evenly | 128 × 3,000 = **384,000** |
| The FTL keeps reusing the same 10 blocks | 10 × 3,000 = **30,000** |

Same silicon. **An order of magnitude less life.** And the difference is purely
a software decision about which free block to hand out next.

That is why the metric reported in document 09 is the erase-count **spread**
(max − min) and **standard deviation**, not the total. The total is set by write
amplification; the spread is set by this document.

### In the notebook

Your notebook has 128 sheets. Each survives about 3,000 erasings before it
falls apart. If you always grab the sheet nearest the front, you'll shred the
first ten sheets while the back hundred are pristine — and then you'll be out of
paper, with most of your notebook untouched.

---

## 2. Dynamic wear levelling — free, and mostly enough

Every time the FTL needs a fresh block it has to choose one from the free pool.
It has to choose *something*. So choose well:

> **Always hand out the free block with the lowest erase count.**

This costs nothing — you were picking a block anyway. It is implemented in
`wear_pick_free_block()` in `src/wear.c`, and the important design point is that
**every** allocation goes through it: host writes, garbage collection,
metadata, everything. Route only host writes through the policy and you get an
erase distribution with a long ugly tail that shows up months later.

Compare against the naive alternative — "first free block found" — which this
project also implements (`--wear none`) precisely so the damage is measurable
rather than merely asserted.

---

## 3. Why dynamic isn't enough: the cold data problem

Here is the hole, and it's the interesting part.

Dynamic levelling only ever sees blocks that are **in the free pool**. Consider
a block holding data that was written once and never touched again — an
operating system image, a photo you'll never edit, a music file.

```
  That block's pages stay VALID forever
            ↓
  It never becomes mostly-invalid
            ↓
  Garbage collection never picks it as a victim
            ↓
  It never gets erased, so never returns to the free pool
            ↓
  Dynamic wear levelling NEVER SEES IT
            ↓
  It sits at erase count 3 while the rest of the drive climbs past 2,000
```

Its endurance is simply wasted. You paid for 3,000 erases on that block and
you'll use four of them.

Worse, the effect compounds: because the cold blocks are unavailable, the *hot*
blocks are recycled through a smaller pool, so they wear even faster.

### In the notebook

A few sheets hold your permanent reference notes. You never erase them, so they
never come back into rotation. All your day-to-day scribbling churns through
the remaining sheets, which wear out while the reference sheets stay factory
fresh.

---

## 4. Static wear levelling — the fix that feels wrong

The only way to level cold data is to do something that seems perverse:

> **Deliberately move data nobody asked you to move.**

Spend flash writes — increasing write amplification — purely to buy endurance.

The rule this project uses:

```
  if (max_erase_count − min_erase_count) > FTL_WEAR_GAP_THRESHOLD:
        find the least-erased block that is holding data
        relocate its contents elsewhere
        it now joins the free pool and takes its turn
```

`FTL_WEAR_GAP_THRESHOLD` is 16 by default (`include/ftl_config.h`), and it is
the whole design:

- **Too small** → the drive burns itself out shuffling cold data around for no
  reason.
- **Too large** → cold data pins young blocks for years.

Implemented in `wear_maybe_level()` in `src/wear.c`, checked every 256 host
writes rather than on every write (erase counts move slowly, and scanning all
blocks on the write path would be absurd on real firmware).

Three guards worth noticing in the code:

- It only considers blocks that are **occupied**. A young block that's already
  free will be picked up by dynamic levelling on the next allocation with zero
  data movement — migrating it would be pure waste.
- It only acts if the coldest occupied block is *meaningfully* younger than the
  most worn one, so it doesn't churn over trivial differences.
- It refuses to start unless there's somewhere to put the survivors.

---

## 5. What was actually measured

50,000 writes on the 90/10 hot-cold workload, seed 1, page mapping, greedy GC.
This workload is chosen deliberately: 90% of writes hit 10% of the address
space, so the other 90% of the data is genuinely cold — exactly the situation
static levelling exists for.

| Policy | WA | Erase min | Erase max | **Spread** | Std dev |
|---|---|---|---|---|---|
| `none` (first free block) | 2.75 | **0** | 50 | **50** | 10.37 |
| `dynamic` (lowest erase count) | 2.76 | 2 | 33 | **31** | 8.23 |
| `static` (dynamic + migration) | 2.78 | **10** | 28 | **18** | 5.53 |

Read that table carefully, because it tells the whole story:

**Spread falls from 50 to 18 — a 64% reduction.** That is the headline.

**Write amplification barely moves: 2.75 → 2.78, about 1%.** Static levelling's
extra writes are almost free at this threshold. You bought a 64% tighter wear
distribution for a 1% throughput cost.

**The minimum erase count rises from 0 to 10.** This is the direct evidence that
static levelling did the thing it was invented for. Under `none` and `dynamic`,
some blocks were *never erased at all* — untouched, wasted endurance. Static
levelling dragged the cold data off them and put them into circulation.

**Dynamic alone gets you most of the way for free** (50 → 31), which is why
every drive does it and why the interesting engineering question is whether
static is worth its cost.

### Translating spread into drive life

Rough estimate, using the max as the block that dies first:

| Policy | Max erases after 50k writes | Host writes before the first block dies |
|---|---|---|
| `none` | 50 | 50,000 × 3,000 / 50 = **3.0 M** |
| `static` | 28 | 50,000 × 3,000 / 28 = **5.4 M** |

**About 80% more useful life** from a software policy change. The benchmark
prints this projection directly.

---

## 6. What wear levelling is *not*

Two clarifications that catch people out:

**It does not reduce the number of erases.** It slightly *increases* them
(2.75 → 2.78 WA). The claim is never "fewer erases" — it is "the same erases,
spread evenly."

**It is not the same as garbage collection**, though they share machinery. GC
reclaims *space*; wear levelling redistributes *wear*. GC picks the block with
the least valid data; static wear levelling picks the block with the lowest
erase count, which is often a block full of perfectly valid data. Opposite
criteria, same relocation plumbing (`mapping_relocate_block()`).

---

## 7. Where this lives in the code

| File | Role |
|---|---|
| `src/wear.c` | `wear_pick_free_block()` — the allocation policy, called for *every* block. `wear_maybe_level()` — the static migration sweep. |
| `src/blockmgr.c` | Calls the allocation policy; owns the free pool. |
| `src/mapping.c` | `mapping_relocate_block()` — the shared "empty this block safely" routine used by wear levelling, read-disturb refresh, and bad-block retirement. |
| `tests/test_wear.c` | 4 tests, including a direct A/B: run the identical workload under `none` and `dynamic` and assert the standard deviation actually falls. |

The shared relocation routine is worth a look. Three different subsystems need
to empty a block without losing anything — static wear levelling, read-disturb
refresh, and rescuing data off a block that just failed. Writing it once is not
just tidiness: it's the only realistic way to be sure all three handle the
block-mapped case identically.

---

## 8. Check yourself

1. Why is the erase-count *spread* the metric, rather than the total?
2. Do the arithmetic: 128 blocks at 3,000 cycles, evenly worn vs 10 blocks
   hammered. What's the ratio?
3. What is dynamic wear levelling, and why is it essentially free?
4. Explain precisely why dynamic levelling can never touch cold data.
5. What does static wear levelling do that feels wrong, and why is it right?
6. In the measured table, why is the rise in *minimum* erase count the most
   telling column?
7. Does wear levelling reduce the number of erases? What does it actually claim?
8. How do garbage collection and static wear levelling pick their victims
   differently?

**Next:** [06_BAD_BLOCKS_AND_POWER_LOSS.md](06_BAD_BLOCKS_AND_POWER_LOSS.md) —
when the hardware breaks and when the power dies.
