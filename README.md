# NAND Flash Translation Layer Simulator

A fixed-allocation C simulator of SSD firmware fundamentals: NAND program-once semantics, page/block/hybrid mapping, garbage collection, wear levelling, bad-block recovery, read-disturb refresh, checkpoints, and power-loss recovery.

## Build and run

The project is intentionally portable C99 and uses no `malloc`.

With GNU Make:

```sh
make
make test
./ftlsim --mapping page --workload rand --ops 20000 --verify
```

On Windows without `make`, the same strict build can be run from PowerShell with GCC:

```powershell
$src = Get-ChildItem src -Filter *.c | ForEach-Object FullName
$tests = Get-ChildItem tests -Filter *.c | ForEach-Object FullName
gcc -std=c99 -Wall -Wextra -Werror -Wshadow -Wpedantic -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -O2 -g -Iinclude $src $tests -lm -o ftltest.exe
.\ftltest.exe
```

Read `DOCS/QUICK_START.md` first. The full documentation index is in `DOCS/README.md`.

## Project shape

- `include/`: public API, compile-time geometry, and shared internal state.
- `src/nand.c`: simulated NAND media and reliability model.
- `src/mapping.c`: page, block, and BAST-style hybrid translation.
- `src/gc.c`: greedy, cost-benefit, FIFO, and random victim selection.
- `src/wear.c`: dynamic and static wear levelling.
- `src/badblock.c`: factory/runtime retirement and data rescue.
- `src/journal.c`: checkpoints and spare-area recovery scans.
- `tests/`: focused unit and end-to-end tests.
- `tools/workload_gen.py`: deterministic trace generator.
- `DOCS/`: study and interview material.

## One-sentence explanation

NAND can program a page only once and erase only a whole block, so the FTL writes new versions elsewhere, translates logical pages to physical pages, and cleans old versions in the background.

## Important metric

Write amplification is physical page programs divided by accepted host page writes. Run workloads after preconditioning the drive; fresh-drive measurements are nearly always 1.0 and hide GC behaviour.
