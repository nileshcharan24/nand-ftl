# 09 — Results

*Assumes: documents 01–08. Every number here was measured by running the code,
not estimated. `bench/results.csv` holds the raw output.*

---

## The setup

```
  Device        128 blocks × 64 pages × 4096 B = 32.00 MiB physical
  Exposed       23.25 MiB  (25% over-provisioned)
  Endurance     3,000 P/E cycles per block
  Metadata      4 blocks reserved
  Timing        read 50 µs · program 600 µs · erase 3,000 µs
```

**Every run is preconditioned.** The drive is filled to 100% of its exposed
capacity, *then* the counters are reset, *then* the measured workload runs.

This matters more than anything else on this page. Write amplification measured
on a freshly formatted drive is always ≈ 1.0, because every write lands in an
already-erased block and garbage collection has never had to run. That number
tells you nothing. The meaningful figure is **steady state**, which is what
SNIA's solid-state performance test spec requires and what the `--fill` flag
plus `ftl_stats_reset()` implement here.

Unless stated: 50,000 measured host writes, seed 1.

---

## 1. Correctness first

```
 65 tests run, 0 failed   |   673,930 checks run, 0 failed
 ALL TESTS PASSED
```

Reproduce with `make test`. Nothing below means anything without this line.

---

## 2. Headline: mapping scheme vs write amplification

Uniform random writes, greedy GC, dynamic wear levelling.

| Scheme | Mapping table | **Write amplification** | Erases |
|---|---|---|---|
| **Page** | 23,808 B | **2.70** | 2,089 |
| **Block** | 372 B | **64.00** | 50,001 |
| **Hybrid** | 2,548 B | **59.51** | 90,714 |

Three things to notice.

**Block mapping's WA is exactly 64.00** — precisely `NAND_PAGES_PER_BLOCK`.
Every random write rebuilds an entire logical block. This is the textbook worst
case, and here it is measured rather than asserted.

**Page mapping costs 64× the RAM to save 24× the flash traffic.** 23,808 bytes
against 372. That is the central trade-off of FTL design, quantified on one
line. It is also why real 1 TB drives solder a DRAM chip next to the controller
(~1 MB of table per 1 GB of flash) and why DRAM-less drives are slower.

**Hybrid mapping barely helps here (59.51), and does the most erases of all.**
That is not a bug — it is **log-block thrashing**, the documented weakness of
BAST. A uniform random workload touches far more distinct logical blocks than
there are log blocks (8), so nearly every write forces a merge, and a full merge
costs two erases where a block-mapped rewrite costs one. Hybrid mapping is
designed for workloads with locality; give it none and it loses to the scheme it
was invented to improve on.

> **The resume claim this supports:** "reduced write amplification from ~64× to
> ~2.7× under random-write workloads." Both halves of that are in the table.

---

## 3. Workload matters more than policy

Page mapping, greedy GC, dynamic wear.

| Workload | **WA** | Erases |
|---|---|---|
| Sequential | **1.00** | 761 |
| Uniform random | **2.70** | 2,089 |
| 90/10 hot-cold | **2.76** | 2,135 |

**Sequential writing produces WA of exactly 1.00** — zero garbage-collection
overhead. The host overwrites whole blocks in order, so blocks become entirely
invalid on their own and cost nothing to reclaim. This is why "write
sequentially" is the first line of every storage performance guide, and why
databases and filesystems go to such lengths to turn random writes into
sequential ones.

The same drive, same firmware, same everything: **2.7× more flash traffic purely
because of the access pattern.**

---

## 4. GC victim selection

Page mapping, dynamic wear, 50,000 writes.

| Policy | Uniform random | 90/10 hot-cold |
|---|---|---|
| Greedy | **2.70** | **2.76** |
| Cost-benefit | **2.72** | **2.17** |
| Random | **5.13** | — |

This is the most interesting table in the project.

**Random is 1.9× worse than greedy** (5.13 vs 2.70). Victim selection is the
algorithm, not a detail. Having the control group is what makes the other
numbers meaningful.

**On uniform random, cost-benefit ties with greedy** (2.72 vs 2.70 — noise).
Exactly as theory predicts. Cost-benefit's advantage comes from its `age` term,
which identifies *cold* blocks whose freed space will stay free. Under uniform
random writes there is no such thing as a cold block, so the extra term has
nothing to say and the extra complexity buys nothing.

**On hot-cold, cost-benefit wins by 21%** (2.17 vs 2.76) and cuts erases 21%
(1,677 vs 2,135). Here 90% of writes hit 10% of the address space, so the other
90% of the data genuinely is cold, and the policy does exactly what it was
designed to do.

> **The lesson worth stating in an interview:** benchmarking cost-benefit on a
> uniform random workload would have told you it was useless. The workload has
> to have the structure the policy exploits, or you measure nothing.

---

## 5. Wear levelling

90/10 hot-cold workload — chosen because 90% of the data is genuinely cold,
which is the only situation where static levelling can help.

| Policy | WA | Erase min | Erase max | **Spread** | Std dev |
|---|---|---|---|---|---|
| `none` | 2.75 | **0** | 50 | **50** | 10.37 |
| `dynamic` | 2.76 | 2 | 33 | **31** | 8.23 |
| `static` | 2.78 | **10** | 28 | **18** | 5.53 |

**Spread falls 50 → 18, a 64% reduction.** Standard deviation nearly halves.

**Write amplification moves 2.75 → 2.78 — about 1%.** Static levelling's extra
writes are almost free at the default threshold of 16.

**The minimum erase count rises from 0 to 10**, and this is the column that
proves the mechanism worked. Under `none` and `dynamic`, some blocks were
*never erased at all* — cold data sat on them, so garbage collection never
picked them, so they never returned to the free pool, so their endurance was
simply wasted. Static levelling dragged that data off and put those blocks into
circulation. Nothing else in the table demonstrates that.

### What it's worth

Using the most-worn block as the one that dies first:

| Policy | Max erases per 50k writes | Projected host writes to first block death |
|---|---|---|
| `none` | 50 | **3.0 M** |
| `static` | 28 | **5.4 M** |

**~80% more useful drive life** from a software policy that costs 1% throughput.

---

## 6. Power-loss recovery

`nand_power_cycle()` then `ftl_mount()`, after thousands of writes with **no
clean shutdown**.

| Scheme | Pages scanned | Pages applied | **Pages lost** |
|---|---|---|---|
| Page | 4,132 | 3,751 | **0** |
| Block | 5,952 | 5,952 | **0** |
| Hybrid | 5,962 | 5,962 | **0** |

Zero data loss on all three schemes after an unclean cut. And
`test_repeated_power_cuts` does it **20 times in a row** with fresh work
interleaved, verifying every logical page after each one.

### What the checkpoint buys

| Mount path | Pages scanned |
|---|---|
| From checkpoint | **93** |
| Full scan | **6,045** |

**65× less work at mount.** This is the whole justification for spending flash
writes on checkpoints: mount time becomes a function of the checkpoint interval
rather than of drive capacity.

Also proven: with checkpointing **disabled entirely**, a full scan of the spare
areas still rebuilds the complete mapping table with zero loss (7,446 spare-area
reads). The media is genuinely self-describing.

---

## 7. Fault tolerance

With program failures injected at 500 ppm and erase failures at 200 ppm — far
worse than any real part — the drive retires blocks, relocates their contents,
and every logical page still reads back correctly, on all three mapping schemes.
The tests permit mismatches only for pages the FTL explicitly counted as lost.

---

## 8. Memory footprint

| Item | Bytes |
|---|---|
| Mapping table (page-mapped) | 23,808 |
| Full FTL context | 28,824 |
| Table bytes per MiB of flash | 744 |

Zero heap allocation. Every structure is a fixed-size static array sized from
`ftl_config.h`, so the linker reports the footprint before the code ever runs —
which is what a real controller with a few hundred KB of SRAM requires.

The 744 bytes/MiB figure extrapolates to the industry rule of thumb of roughly
1 MB of mapping RAM per 1 GB of flash.

---

## 9. Reproducing all of it

```sh
make                # build
make test           # 65 tests

# the mapping comparison (section 2)
./ftlsim --mapping page   --workload rand --ops 50000 --seed 1 --quiet
./ftlsim --mapping block  --workload rand --ops 50000 --seed 1 --quiet
./ftlsim --mapping hybrid --workload rand --ops 50000 --seed 1 --quiet

# GC policies on hot-cold (section 4)
./ftlsim --gc greedy      --workload hotcold --ops 50000 --seed 1 --quiet
./ftlsim --gc costbenefit --workload hotcold --ops 50000 --seed 1 --quiet

# wear levelling (section 5)
./ftlsim --wear none    --workload hotcold --ops 50000 --seed 1 --quiet
./ftlsim --wear static  --workload hotcold --ops 50000 --seed 1 --quiet
```

Runs are deterministic: the same seed gives byte-identical statistics, asserted
by `test_runs_are_reproducible`.

---

## 10. How to state these numbers honestly

Write amplification is not a property of firmware alone. It depends on geometry,
over-provisioning, fill level, workload, policy and seed. A bare "my FTL got
WA down to 2" is not a defensible claim.

**Defensible:**

> "On a 32 MiB simulated device at 25% over-provisioning, preconditioned to
> 100% full, under 50,000 uniform random 4 KiB writes: page-level mapping with
> greedy GC measured write amplification 2.70, against 64.00 for block-level
> mapping on the identical workload."

**Not defensible:**

> "My FTL has a write amplification of 2."

The same discipline applies to every other number here. Quote the conditions.

---

## 11. The three findings worth leading with

If you have sixty seconds:

1. **Page vs block mapping: 2.70 vs 64.00 write amplification, for 64× the
   RAM.** The central trade-off of FTL design, measured.
2. **Cost-benefit GC beats greedy by 21% — but only on hot-cold workloads,
   and ties on uniform random.** Shows you understand *why* a policy works, not
   just that it does.
3. **Static wear levelling cut erase spread 50 → 18 for a 1% WA cost, raising
   the minimum erase count from 0 to 10.** That last number is the proof that
   cold data actually got unstuck.

**Next:** [10_INTERVIEW_QA.md](10_INTERVIEW_QA.md)
