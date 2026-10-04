# 07 — What Every File and Folder Is For

*Assumes: documents 01–06. This is a reference — skim it, then come back when
you open a file and wonder what it's doing there.*

---

## The top level

```
FTL/
├── include/      the .h files — interfaces, "what you can call"
├── src/          the .c files — implementations, "how it works"
├── tests/        the test suite
├── tools/        Python helpers
├── DOCS/         these documents
├── build/        compiler output (.o object files). Regenerated; ignore it.
├── bench/        benchmark results (results.csv). Generated.
├── Makefile      the build recipe
├── ftlsim        ← COMPILED PROGRAM: the benchmark
├── ftltest       ← COMPILED PROGRAM: the test suite
└── README.md     one-page summary
```

**Two programs get built.** `ftlsim` runs a workload and prints measurements.
`ftltest` runs 65 tests and prints pass/fail. Both are built from the same
library of `.c` files in `src/` — they just have different `main()` functions.

### Why `.h` and `.c` are separate

If you're new to C: a **header** (`.h`) declares *what exists* — function names,
their arguments, struct layouts. A **source file** (`.c`) contains the actual
code. Other files `#include` the header to learn what they're allowed to call,
without needing to see the implementation.

It's the C equivalent of an interface: `nand.h` says "you can call
`nand_program_page()` and here's its signature"; `nand.c` decides how.

---

## `include/` — the interfaces

| File | Lines | What it is |
|---|---|---|
| **`ftl_config.h`** | 267 | **Every tunable number in the project, in one place.** Page size, pages per block, block count, endurance rating, over-provisioning percentage, GC watermarks, timing figures. Change one line and the whole device resizes. Heavily commented — read this one early, it's the most approachable file. |
| **`nand.h`** | 211 | The fake flash chip's interface. Four operations (read, read-spare, program, erase) plus introspection. **The comments here explain each of flash's rules at the point where it's enforced** — arguably the best single explanation in the codebase. |
| **`ftl.h`** | 300 | The FTL's public interface: `ftl_read`, `ftl_write`, `ftl_trim`, `ftl_flush`, plus the configuration enums (mapping scheme, GC policy, wear policy) and the statistics struct. Deliberately shaped like NVMe commands. |
| **`ftl_internal.h`** | 225 | State shared *between* the FTL's own modules — the mapping table, per-block metadata, the validity bitmap. Not part of the public interface. |

### Why static configuration matters

`ftl_config.h` uses compile-time constants, not runtime variables, and every
array in the project is sized from them. That's deliberate: real SSD firmware
runs on a controller with a few hundred KB of SRAM and **no `malloc`**. Every
buffer is sized at build time so the *linker* can tell you — before the firmware
ever ships — whether the image fits the silicon.

There is not a single `malloc` in this project. Worth saying out loud in an
interview.

---

## `src/` — the implementation

Read them in this order; it matches the documents you've just read.

| File | Lines | What it does | Doc |
|---|---|---|---|
| **`nand.c`** | 507 | **The fake flash chip.** Holds the data array, enforces program-once and sequential programming, tracks erase counts, injects failures, models timing. This is "the hardware". | 02 |
| **`mapping.c`** | 1,229 | **The heart of the project.** All three mapping schemes, the write/read/trim paths, and the shared block-relocation routine. The biggest file, and the one to understand best. | 03 |
| **`blockmgr.c`** | 201 | The free-block pool and the write frontier. "Give me a fresh page to write to." | 03 |
| **`gc.c`** | 316 | Garbage collection: the four victim-selection policies, the watermark logic, read-disturb refresh. | 04 |
| **`wear.c`** | 182 | Wear levelling: the block-allocation policy and the static migration sweep. | 05 |
| **`badblock.c`** | 133 | Retiring failed blocks in the correct order, without losing data. | 06 |
| **`journal.c`** | 790 | Checkpoints and power-loss recovery. Second biggest file — recovery is genuinely intricate. | 06 |
| **`ftl.c`** | 458 | The glue. `ftl_format`, `ftl_mount`, `ftl_write`, `ftl_read`, plus the statistics and **`ftl_check_invariants()`**. | 03 |
| **`main.c`** | 592 | The `ftlsim` benchmark program: argument parsing, workload generation, preconditioning, the report you see printed. | 09 |

### The one function to look at first

`ftl_check_invariants()` in `ftl.c` is the most valuable 80 lines in the
project. It walks every condition the design depends on and returns false if any
is broken:

- per-block valid counts match the bitmap
- no valid bit sits on an erased page
- free blocks are genuinely erased and genuinely empty
- nothing valid lives in a retired block
- the free-block counter matches reality
- the write frontier is where it claims to be
- every mapped logical page points at a valid physical page

The tests call it after every few hundred operations. That turns bugs which
would otherwise surface as a mysterious wrong-data read thousands of operations
later into a failure on the operation that caused it.

---

## `tests/` — 65 tests, 673,930 assertions

| File | Tests | Covers |
|---|---|---|
| `test_framework.h` | — | A ~100-line test harness. No external framework, because firmware test builds often can't have one. |
| `test_main.c` | — | The runner and shared helpers. |
| `test_nand.c` | 12 | **Is the fake chip faithful?** Mostly proving it says *no* in all the right places. |
| `test_mapping.c` | 16 | All three schemes obey the same contract; measured RAM sizes; block-mapping RMW cost; hybrid switch vs full merges. |
| `test_gc.c` | 10 | GC doesn't run on a fresh drive; reclaims without data loss; greedy picks the emptiest block; a dead block costs zero copies; TRIM reduces work. |
| `test_wear.c` | 4 | Dynamic beats none; static raises the minimum erase count; the allocator really picks the least-worn block. |
| `test_badblock.c` | 6 | Factory bad blocks excluded; retirement rescues data; survives injected failures on all three schemes. |
| `test_journal.c` | 10 | Clean and dirty shutdown × 3 schemes; full-scan rebuild; torn checkpoint ignored; 20 repeated power cuts. |
| `test_ftl.c` | 7 | End-to-end stress with everything on at once; full-drive correctness; reproducibility; read-only workload triggering refresh. |

### The single most valuable test

`test_ftl.c` uses a **shadow copy**: the test keeps its own trivial model of
what the drive should contain and compares against it. Every page written
carries an encoded `(lpn, version)` pattern, so a read can prove not merely that
it succeeded, but that it returned **the right page at the right version** —
the two ways an FTL can be wrong while appearing to work.

Also worth knowing: `test_can_overfit_...`-style sanity checks exist in spirit
here as `test_runs_are_reproducible` — two runs with the same seed produce
byte-identical statistics. Without that, no benchmark number would be
reproducible and no long-running bug could be bisected.

---

## `tools/` — Python helpers

| File | Lines | What it does |
|---|---|---|
| `workload_gen.py` | 51 | Generates trace files (`W 42`, `R 17`, `T 99`) with different access patterns, which `ftlsim --trace` replays. Lets you drive the C simulator from externally-defined workloads. |
| `run_experiments.py` | 62 | Sweeps configurations, runs `ftlsim` for each, collects the CSV output into `bench/results.csv`. This is what produced the tables in document 09. |

---

## `Makefile` — how it builds

```sh
make          # build both ftlsim and ftltest
make test     # build and run the test suite
make bench    # run the experiment sweep into bench/results.csv
make asan     # rebuild with AddressSanitizer + UBSan and run the tests
make memcheck # run the tests under valgrind
make clean
```

The compiler flags are worth knowing, because they're a statement of intent:

```
-std=c99 -Wall -Wextra -Werror -Wshadow -Wpedantic
-Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition
```

`-Werror` turns every warning into a build failure. In firmware a warning is
usually a bug that hasn't bitten yet. `-Wshadow` catches the classic
inner-variable-hides-outer bug. `-std=c99` with `-Wpedantic` means no compiler
extensions, so the code ports.

You can resize the device from the command line without editing anything:

```sh
make CFLAGS_EXTRA="-DNAND_NUM_BLOCKS=512 -DFTL_OP_PERCENT=7"
```

---

## The two compiled programs

### `ftltest` — just run it

```sh
./ftltest
```

Prints every test and finishes with a summary. Should end:

```
 65 tests run, 0 failed   |   673930 checks run, 0 failed
 ALL TESTS PASSED
```

### `ftlsim` — the benchmark

```sh
./ftlsim --mapping page --gc greedy --wear dynamic --workload rand --ops 50000 --seed 1 --verify
```

| Option | Values | Meaning |
|---|---|---|
| `--mapping` | `page` \| `block` \| `hybrid` | Address translation scheme (doc 03) |
| `--gc` | `greedy` \| `costbenefit` \| `fifo` \| `random` | Victim selection (doc 04) |
| `--wear` | `none` \| `dynamic` \| `static` | Wear levelling (doc 05) |
| `--workload` | `seq` \| `rand` \| `hotcold` \| `zipf` \| `mixed` | Access pattern |
| `--ops N` | number | Measured host writes |
| `--fill PCT` | 0–100 | Precondition to this % full before measuring |
| `--seed S` | number | Reproducibility |
| `--program-fail-ppm` / `--erase-fail-ppm` | number | Fault injection (doc 06) |
| `--verify` | flag | Read back and check every page at the end |
| `--csv` | flag | One CSV row instead of the human report |

Full list: `./ftlsim --help`.

---

## Reading order for the code

If you sit down to actually read the source, do it in this order:

1. **`include/ftl_config.h`** — get the geometry in your head.
2. **`include/nand.h`** — the rules, with the reasoning in the comments.
3. **`src/nand.c`** — see the rules enforced.
4. **`include/ftl.h`** — what the FTL promises the host.
5. **`src/mapping.c`, section 4 only** (page mapping — `page_write`, `page_read`,
   `page_trim`). Ignore the block and hybrid sections on the first pass.
6. **`src/gc.c`** — `gc_select_victim()` and `gc_maybe_run()`.
7. **`src/ftl.c`** — `ftl_write()` to see the order of operations, then
   `ftl_check_invariants()`.

Then document 08 walks a single write through all of it, line by line.

**Next:** [08_CODE_WALKTHROUGH.md](08_CODE_WALKTHROUGH.md)
