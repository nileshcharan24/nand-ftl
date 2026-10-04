# 06 — Bad Blocks and Power Loss

*Assumes: documents 01–05. Two separate topics, both about surviving things
going wrong.*

---

# PART A — Bad Blocks

## 1. Blocks are broken from the factory

Every NAND chip ships with some blocks already unusable.

This is not a defect in the sense of "something went wrong." Silicon at this
density always has flaws. Guaranteeing a *perfect* die would mean discarding
most of every wafer, which would make flash far too expensive. So manufacturers
do the economically sane thing: test the die, mark the bad blocks, and guarantee
"no more than X% bad" in the datasheet.

The mark itself is a non-`0xFF` byte written into the spare area of the block's
first page. Firmware must scan for these marks at first power-on — and then
**never erase those blocks**, because erasing would wipe the only record that
the block is unusable.

In this project: `NAND_FACTORY_BAD_BLOCKS` (default 2) in
`include/ftl_config.h`, scattered deterministically at format time. They're
kept out of the metadata region, mirroring the real guarantee that block 0 is
always good so the boot loader has somewhere to live.

## 2. Blocks also fail during use

A program or erase operation can simply not complete. The chip reports failure.
When that happens the block can no longer be trusted, and the drive has to keep
working anyway. These are called **grown bad blocks**.

## 3. The part that's easy to get wrong

Here is the mistake, and it's the reason this topic makes a good interview
question:

> When a program fails partway through a block, the pages **already written** in
> that block are usually still readable.

So the ordering is not negotiable:

```
  WRONG                              RIGHT
  ─────                              ─────
  1. Program fails                   1. Program fails
  2. Mark block bad                  2. Read out the pages that are still good
  3. ... data in it is now            3. Relocate them elsewhere, update the map
     unreachable forever              4. NOW mark the block bad
```

Get it backwards and every retirement silently eats up to a block's worth of
user data.

This is why `src/nand.c` **does not retire blocks by itself.** When a program
fails, the fake chip reports the failure and does nothing else — exactly like
real silicon. The decision to retire, and the ordering, belong to firmware, in
`src/badblock.c`. A chip that retired itself on the spot would take the user's
data with it.

`badblock_retire()` follows six steps in a fixed order:

1. Rescue whatever is still readable (`mapping_relocate_block()`).
2. Clear any remaining validity bits for the block.
3. If it was in the free pool, remove it.
4. If it was the current write frontier, drop it.
5. If it was serving as a hybrid log block, release the slot.
6. **Only now** write the durable bad-block marker.

## 4. One more subtlety: the failed page is consumed

When a program fails, the page slot is *gone* — charge did go onto the floating
gates, just not reliably. So the write pointer advances anyway, and any later
read of that page finds data that doesn't match its CRC and is reported as an
uncorrectable ECC error.

`src/nand.c` models exactly this. It's a detail most simulators skip, and it
matters because firmware has to cope with "this page exists but is garbage."

## 5. Testing it: fault injection

You cannot wait for real hardware to fail. So the fake chip can be told to fail
operations at a rate you choose:

```sh
./ftlsim --program-fail-ppm 500 --erase-fail-ppm 200 --verify
```

500 parts per million is far worse than any real part — chosen so a short test
sees dozens of failures. `tests/test_badblock.c` runs the whole workload under
injection and then asserts that **every single logical page still reads back
correctly**, with the only permitted exceptions being pages the FTL explicitly
reported as lost.

That distinction — between "we know we lost it and said so" and "it silently
went wrong" — is the entire point of the test.

---

# PART B — Power Loss

## 6. The problem

The mapping table lives in the controller's RAM. Pull the power and it is gone.

The *data* is all still on the flash — that's what non-volatile means. But
without the table, the drive has no idea which physical page holds logical page
12345. Which is exactly as bad as having lost everything.

And "power loss" is not exotic. It's a USB stick yanked out mid-copy, a laptop
battery dying, a data-centre rack losing a feed. Firmware is expected to come
back from it **every** time, not usually.

### In the notebook

You keep the index in your head while you work, and only occasionally write it
down. If you're interrupted, everything you've written since the last time you
copied the index out is orphaned — ink on the page with nothing pointing at it.

## 7. Mechanism 1: the spare area IS the journal

Remember from document 02 that every page is programmed together with its spare
area, which contains:

- `lpn` — which logical page this physical page holds
- `seq` — a global counter that only ever increases

This makes the media **self-describing**. You can rebuild the entire mapping
table by reading every page's spare area and, for each logical page, keeping the
physical copy with the **highest sequence number**.

```
  Scanning finds:                      Conclusion:
    PPN 700 : lpn=42, seq=1500           LPN 42 currently lives at PPN 901,
    PPN 901 : lpn=42, seq=9200           because 9200 > 1500.
```

This costs **nothing extra** — the spare area was going to be programmed
anyway. It is why this design needs no separate write-ahead log, which would
double the metadata traffic for information that's already implicit in the data
writes.

**Notebook version: you scribble the chapter number in the margin of every line
as you write it. Lose the index entirely and you can rebuild it by reading all
the margins.**

## 8. Mechanism 2: checkpoints bound the scan

Rebuilding by full scan is O(capacity). On a 4 TB drive that's minutes, and
nobody waits minutes for a disk to appear.

So the FTL periodically writes a **checkpoint** — a snapshot of its whole state
— into a fixed metadata region at a known address. (Fixed, because at mount time
you have no mapping table with which to find your own metadata. Chicken and
egg.)

On the next boot: load the snapshot, then only examine pages written *after* it.

**Mount time becomes a function of the checkpoint interval, not of capacity.**
That's the whole value.

Measured here (`tests/test_journal.c`): mounting from a checkpoint scanned
**93 pages**; a full scan of the same drive scanned **6,045**. Sixty-five times
less work.

### The clever trick that makes the scan cheap

To find pages newer than the checkpoint you'd think you must read every spare
area. You don't:

> Pages within a block are programmed in strictly increasing order (Rule 2), and
> the sequence number is globally monotonic. **Therefore the last programmed
> page of a block always carries the highest sequence number in that block.**

So one spare-area read per *block* tells you whether that block is worth
scanning at all. 128 cheap reads instead of 8,192.

## 9. Why the header is written last

A checkpoint spans several pages. If power dies halfway through, you must not
mount from the half that made it.

So the payload pages go down first, and the **header** — carrying the magic
number, the sequence number and a CRC over the payload — goes **last**.

A header on the media is therefore *proof that everything it describes made it*.
A torn checkpoint has no header, is invisible to recovery, and the previous
checkpoint is used instead.

This "commit record written last" pattern is exactly what journalling
filesystems and databases do, for exactly the same reason.
`tests/test_journal.c::test_torn_checkpoint_is_ignored` proves it by
deliberately writing a headerless partial checkpoint and confirming recovery
ignores it.

## 10. How power loss is simulated

The model is exact and worth stating:

```c
nand_power_cycle();          /* volatile state is gone; the media persists */
ftl_mount(&params, mode);    /* firmware boots and rebuilds */
```

`ftl_mount()` **memsets the entire FTL context to zero** before doing anything.
There is no way for a test to pass by accident because some variable survived.
The only thing that carries information across the cut is the media — which is
exactly the situation the real product is in.

Two flavours are tested, and they're genuinely different:

| | What happened | Result |
|---|---|---|
| **Clean** | `ftl_flush()` called first, so a checkpoint describes everything | 0 pages lost |
| **Dirty** | Power died mid-workload, thousands of writes after the last checkpoint | 0 pages lost |

Measured, for all three mapping schemes:

| Scheme | Pages scanned | Pages applied | **Pages lost** |
|---|---|---|---|
| page | 4,132 | 3,751 | **0** |
| block | 5,952 | 5,952 | **0** |
| hybrid | 5,962 | 5,962 | **0** |

And `test_repeated_power_cuts` does it **20 times in a row** with interleaved
work, verifying everything after each one. One successful recovery could be
luck; twenty is evidence.

## 11. What this design does *not* protect — stated honestly

**TRIM is not journalled.** If the host TRIMs a page and power is lost before
the next checkpoint, recovery finds the old data still on the media, still
carrying the highest sequence number for that LPN, and resurrects it.

That is not laziness — NVMe explicitly permits it (Dataset Management Deallocate
is non-deterministic unless the drive advertises otherwise). But it is a real
behaviour, and naming it yourself in an interview is much better than being
caught by it.

---

## 12. Where this lives in the code

| File | Role |
|---|---|
| `src/badblock.c` | Retirement, in the correct order. Recursion-bounded so a cascade of failures can't run away. |
| `src/nand.c` | Factory bad blocks, `nand_mark_bad()`, fault injection, `nand_power_cycle()`. |
| `src/journal.c` | Checkpoint write, checkpoint discovery, roll-forward, full-scan rebuild. |
| `tests/test_badblock.c` | 6 tests including the fault-injection survival run. |
| `tests/test_journal.c` | 10 tests: clean shutdown × 3 schemes, dirty shutdown × 3 schemes, full-scan rebuild with checkpointing disabled entirely, torn checkpoint, 20 repeated cuts. |

Two details worth knowing:

**Checkpoint writes are atomic.** Write to `<name>.tmp`, then `os.replace()`.
Without that, a crash during a write leaves a truncated file that fails to load
hours later.

**RNG state is saved in the checkpoint.** That's what makes a resumed run
bit-identical to an uninterrupted one — otherwise the random decisions diverge
from that point and the run is no longer reproducible.

---

## 13. Check yourself

1. Why do chips ship with bad blocks? Why is that an economic decision?
2. Why must firmware never *erase* a factory bad block?
3. What's the correct ordering when a program failure retires a block, and what
   happens if you get it backwards?
4. Why does the fake NAND chip refuse to retire blocks by itself?
5. What two fields in the spare area make the media self-describing?
6. If two physical pages both claim to be LPN 42, how does recovery decide?
7. What does a checkpoint buy you, given the spare-area scan already works?
8. Why is the checkpoint header written last?
9. Explain the one-read-per-block trick for finding pages newer than the
   checkpoint. Which NAND rule makes it valid?
10. What is the honest limitation around TRIM?

**Next:** [07_REPO_TOUR.md](07_REPO_TOUR.md) — what every file in the project is
for.
