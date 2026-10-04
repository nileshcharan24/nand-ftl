# 04 — Garbage Collection and Write Amplification

*Assumes: documents 01–03.*

---

## 1. The problem you just created

Document 03 solved overwriting by never overwriting: write the new copy
elsewhere, mark the old one dead.

Do that a few thousand times and the drive fills up with corpses.

```
  After a while, a typical block looks like this:

  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │ ✗  │ ✓  │ ✗  │ ✗  │ ✓  │ ✗  │ ✗  │ ✓  │   ✓ = valid (live)
  └────┴────┴────┴────┴────┴────┴────┴────┘   ✗ = invalid (dead)

  5 of 8 pages are dead. That space is unusable.
```

You cannot reuse a dead page — Rule 1. The only way to get it back is to erase
the whole block. But the block still has three live pages in it, and erasing
destroys those too.

So:

> **To reclaim space, you must first move the survivors out of the way.**

That is **garbage collection**. Three steps:

```
 1. Pick a victim block.
 2. Copy its still-valid pages somewhere else (and update the mapping table).
 3. Erase the victim. It rejoins the pool of free blocks.
```

### In the notebook

Your notebook is full. Most lines are crossed out, but they're scattered across
every sheet, so no sheet is completely dead. To free a sheet you must copy its
few surviving lines onto a fresh sheet, update the index for each, and only then
erase the old one.

---

## 2. Write amplification — the number that matters

Look at step 2 again. Those copies are **flash writes the host never asked
for**. The host wrote one page; the drive wrote that page *plus* however many
pages garbage collection had to relocate.

That ratio has a name:

```
                      physical page writes to flash
  Write Amplification = ─────────────────────────────
                        page writes requested by host
```

**WA = 1.0** — perfect. Every host write costs exactly one flash write.
**WA = 3.0** — for every 1 GB the host writes, 3 GB hits the flash.

WA is the headline metric of storage firmware, because it drives all three
things customers care about:

| WA affects | How |
|---|---|
| **Endurance** | 3× the writes means the drive dies in a third of the time. |
| **Performance** | The flash is busy doing GC instead of serving the host. |
| **Power** | Programs and erases are the expensive operations, and you're doing more. |

Reducing WA is a large part of what SSD firmware engineers are actually paid to
do. That is why this project measures it obsessively.

### Why the victim choice controls WA

Say a block has 64 pages and `u` of them are still valid.

- Copy `u` pages, erase, get 64 free pages back.
- Net gain: `64 − u` free pages, for a cost of `u` writes.

| Victim is… | Valid pages `u` | Copies | Free pages gained | Cost per page freed |
|---|---|---|---|---|
| Mostly dead | 6 | 6 | 58 | 0.10 |
| Half dead | 32 | 32 | 32 | 1.00 |
| Mostly alive | 58 | 58 | 6 | **9.67** |

Picking a mostly-dead block is nearly free. Picking a mostly-alive block is
close to pointless. **Victim selection is the algorithm.**

---

## 3. Over-provisioning: the biggest lever

Here is the part people find counter-intuitive.

Every SSD hides some capacity from you. A "512 GB" drive contains more than 512
GB of flash. The hidden slice is called **over-provisioning** (OP), and this
project uses **25%** — 32 MiB of physical flash exposed as 23.25 MiB.

Why deliberately waste a quarter of the chip?

Because **OP is the slack the garbage collector breathes with.** With zero OP,
the instant the host has written every logical address once, the drive is 100%
full. Every subsequent write must collect a block that is almost entirely valid
— the disastrous bottom row of the table above. WA heads for infinity.

With 25% OP there is always dead space distributed around, so the collector can
usually find a mostly-dead block.

The classic approximation for uniform random writes:

```
              1
  WA  ≈  ───────────        where OP is the over-provisioned fraction
          2 × OP
```

At OP = 0.25 that predicts **WA ≈ 2.0**. This project measured **2.70** — the
same ballpark, higher because the approximation ignores metadata writes and
assumes an idealised steady state.

Real drives: consumer models ship ~7% OP (which is really just the
GB-vs-GiB gap — a "512 GB" drive holds 512×10⁹ bytes on 512×2³⁰ bytes of flash).
Enterprise write-intensive drives ship 28% or more, and cost accordingly. You
are literally buying write endurance with capacity.

`FTL_OP_PERCENT` in `include/ftl_config.h`.

---

## 4. When to collect

Collect too early and you burn erases on blocks that would have died naturally
if you'd waited. Collect too late and a burst of writes finds no free block and
stalls.

This project uses **two watermarks**:

```c
#define FTL_GC_LOW_WATERMARK   6   /* start collecting below this many free blocks */
#define FTL_GC_HIGH_WATERMARK 10   /* stop once the pool recovers to here          */
```

The gap between them is **hysteresis**. Without it the FTL would collect exactly
one block, drop back below the trigger on the very next write, and collect
again — turning a background activity into a per-write tax. Hysteresis is a
general control-systems idea and it's the same reason your thermostat doesn't
switch on and off every second.

> Real drives also collect during **idle time**, which hides the cost entirely
> until the workload gets busy enough that idle time disappears. That "the drive
> was fast for ten minutes and then fell off a cliff" behaviour you may have
> seen in reviews is exactly this — idle-time GC running out of idle. This
> project collects only on demand, which measures the worst case honestly.

`gc_maybe_run()` in `src/gc.c`, called before every host write.

---

## 5. The four victim-selection policies

All four are implemented in `gc_select_victim()` in `src/gc.c`, selectable with
`--gc`.

### Greedy — fewest valid pages wins

Scan every candidate block, pick the one with the smallest `valid_pages`.

Minimises the copy cost of *this* collection, and for uniform random workloads
it is very close to optimal.

Its blind spot: it has no notion of *time*. It will happily collect a **cold**
block at 40% valid when a **hot** block at 45% valid would have decayed to 5%
valid on its own if left alone for a few more seconds.

### Cost-benefit — account for age too

```
                (1 − u) × age
    score  =  ─────────────────
                    2u
```

where `u` is the valid fraction and `age` is how long since the block was last
written.

Reading it piece by piece:

- `(1 − u)` — the space you get back. More is better.
- `× age` — a block untouched for a long time is **cold**, so the space you free
  from it will *stay* free instead of being immediately re-dirtied.
- `÷ 2u` — the cost: `u` page reads plus `u` page writes.

So you're maximising "space freed, weighted by how long it stays freed, per unit
of work." This came from the Sprite log-structured filesystem cleaner and flash
firmware adopted it nearly unchanged, because the problem is identical: a log
that must be cleaned, with hot and cold regions mixed together.

A block with `u = 0` is a free lunch — zero cost, whole block reclaimed — so it
always wins outright.

### FIFO — collect whatever was written longest ago

Costs nothing to evaluate. Ignores validity entirely, so it will cheerfully
collect a 100%-live block.

### Random — the control group

If your clever policy can't beat random, it isn't earning its complexity. Having
the baseline in the results table is what makes the other numbers mean anything.

---

## 6. What was actually measured

50,000 host writes, seed 1, after preconditioning the drive to 100% full.
Page-level mapping, dynamic wear levelling.

| Policy | Workload | **WA** | Erases |
|---|---|---|---|
| Greedy | uniform random | **2.70** | 2,089 |
| Cost-benefit | uniform random | **2.72** | 2,103 |
| **Random** | uniform random | **5.13** | 3,984 |
| Greedy | 90/10 hot-cold | **2.76** | 2,135 |
| **Cost-benefit** | 90/10 hot-cold | **2.17** | 1,677 |
| Greedy | sequential | **1.00** | 761 |

Four findings, and each is a good thing to be able to explain:

**Random GC is 1.9× worse than greedy** (5.13 vs 2.70). Victim selection is not
a detail.

**On uniform random, cost-benefit is no better than greedy** (2.72 vs 2.70 — a
rounding error). Exactly as theory predicts: when every page is equally likely
to be overwritten, there is no such thing as a cold block, so the `age` term has
nothing to say.

**On hot-cold, cost-benefit wins by 21%** (2.17 vs 2.76), and cuts erases by
21%. This is the whole point of the policy, and it only shows up when the
workload has the structure the policy was designed for. *Measuring a policy on
the wrong workload will tell you it's useless.*

**Sequential writing gives WA = 1.00 exactly.** No garbage collection cost at
all, because the host overwrites whole blocks in order, so blocks become
entirely invalid on their own and are free to reclaim. This is why "write
sequentially" is the first piece of advice in any storage performance guide.

---

## 7. TRIM, and why it's worth having

When you delete a file, the filesystem just forgets about it. The drive has no
idea — as far as the FTL knows, those LPNs still hold precious data, and the
garbage collector faithfully copies them around forever.

**TRIM** is the host telling the drive "I will never read these addresses
again." The FTL can then mark those pages invalid immediately, and GC stops
wasting work on them.

`tests/test_gc.c::test_trim_reduces_gc_work` proves the effect: the same
workload, run once with and once without a TRIM of three quarters of the address
space, and the GC copy count is measurably lower with it.

`ftl_trim()` in `src/ftl.c`. It maps to the NVMe *Dataset Management /
Deallocate* command on a real drive.

---

## 8. Where this lives in the code

| File | Role |
|---|---|
| `src/gc.c` | Victim selection (all four policies), the collection loop, the watermarks, read-disturb refresh. |
| `src/blockmgr.c` | The free-block pool that GC feeds. |
| `tests/test_gc.c` | 10 tests: GC doesn't run on a fresh drive, reclaims without losing data, greedy really picks the emptiest block, a fully-dead block costs zero copies, TRIM reduces work. |

Two implementation details worth knowing:

**GC shares the write frontier with host writes.** Real firmware usually gives
GC a *separate* open block, so that relocated (by definition, cold-ish) data
isn't interleaved with fresh host data that's about to be overwritten. Keeping
them apart makes future blocks more uniformly hot or cold, which cuts future GC
cost. That separation is the highest-value improvement still on the table here,
and it's named as such in document 10.

**There's a no-progress guard.** If a collection frees a block but the pool
doesn't grow — every survivor consumed exactly as much space as was freed — the
loop stops rather than spinning and burning erases. That state is what
over-provisioning exists to prevent, and hitting it means OP is set too low.

---

## 9. Check yourself

1. Why can't you just reuse an invalid page?
2. Define write amplification. What does WA = 3 mean in plain words?
3. Why does picking a mostly-*dead* victim block matter so much?
4. What is over-provisioning and why does a manufacturer hide capacity on purpose?
5. Why two watermarks instead of one?
6. Explain each of the three parts of the cost-benefit formula.
7. Why did cost-benefit tie with greedy on random writes but win by 21% on
   hot-cold? What does that tell you about benchmarking?
8. Why is sequential writing WA = 1.00?
9. What does TRIM buy the drive?

**Next:** [05_WEAR_LEVELLING.md](05_WEAR_LEVELLING.md) — all those erases are
killing the chip.
