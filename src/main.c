/* =============================================================================
 * main.c -- The benchmark driver.
 * =============================================================================
 *
 * Builds a device, conditions it, runs a workload against it, and reports what
 * the flash actually had to do.
 *
 * THE MOST IMPORTANT THING IN THIS FILE IS THE PRECONDITIONING STEP.
 * ------------------------------------------------------------------
 * If you format a drive and immediately measure write amplification, you get
 * 1.0 -- every write lands in an already-erased block, no garbage collection
 * has ever had to run, and the number tells you nothing. Vendors' "fresh out of
 * box" numbers are exactly this, which is why nobody in the industry quotes
 * them.
 *
 * The meaningful number is STEADY STATE: fill the drive, keep writing until the
 * garbage collector is running continuously and the free-block pool has settled
 * into equilibrium, and only then start counting. SNIA's Solid State Storage
 * Performance Test Specification formalises this, and it is what --fill and
 * ftl_stats_reset() implement here.
 *
 * Every number in DOCS/RESULTS.md is a steady-state number for that reason.
 * ========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ftl.h"

/* ---------------------------------------------------------------------------
 * Workload patterns
 * ------------------------------------------------------------------------- */
typedef enum {
    WL_SEQUENTIAL = 0,  /* LPN 0,1,2,... wrapping. The easy case for every FTL */
    WL_RANDOM,          /* uniform over the whole address space. The hard case */
    WL_HOTCOLD,         /* 90% of writes hit 10% of the address space          */
    WL_ZIPF,            /* power-law skew: a few very hot pages, a long tail   */
    WL_MIXED,           /* alternating bursts of sequential and random         */
    WL_TRACE            /* replay a file produced by tools/workload_gen.py     */
} workload_t;

typedef struct {
    ftl_params_t params;
    workload_t   workload;
    uint64_t     ops;
    uint32_t     fill_percent;
    bool         verify;
    bool         csv;
    bool         quiet;
    uint32_t     program_fail_ppm;
    uint32_t     erase_fail_ppm;
    const char  *trace_path;
    const char  *label;
} config_t;

/* ---------------------------------------------------------------------------
 * Page content: a repeating 8-byte record of (lpn, version).
 *
 * Verification is not optional decoration. An FTL bug that returns the wrong
 * page is far more likely than one that crashes, and without content checking
 * the benchmark would happily report beautiful write-amplification numbers for
 * a drive that has been silently corrupting data for the last million writes.
 * ------------------------------------------------------------------------- */
static uint8_t  s_page[NAND_PAGE_SIZE_BYTES];
static uint8_t  s_readback[NAND_PAGE_SIZE_BYTES];
static uint32_t s_version[FTL_LOGICAL_PAGES];   /* newest version per LPN, 0 = unwritten */

static void fill_pattern(uint8_t *buf, uint32_t lpn, uint32_t version)
{
    uint32_t i;
    uint32_t *w = (uint32_t *)(void *)buf;

    for (i = 0u; i < NAND_PAGE_SIZE_BYTES / sizeof(uint32_t); i += 2u) {
        w[i]      = lpn ^ (i << 16);
        w[i + 1u] = version;
    }
}

static bool check_pattern(const uint8_t *buf, uint32_t lpn, uint32_t version)
{
    uint32_t i;
    const uint32_t *w = (const uint32_t *)(const void *)buf;

    for (i = 0u; i < NAND_PAGE_SIZE_BYTES / sizeof(uint32_t); i += 2u) {
        if (w[i] != (lpn ^ (i << 16)) || w[i + 1u] != version) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * A deterministic PRNG for the workload, independent of the FTL's and the
 * die's, so that changing a policy cannot change the access pattern.
 * ------------------------------------------------------------------------- */
static uint64_t s_wl_rng;

static uint64_t wl_rand(void)
{
    uint64_t x = s_wl_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s_wl_rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static double wl_rand_unit(void)
{
    return (double)(wl_rand() >> 11) / (double)(1ULL << 53);
}

/* Pick the next LPN for the chosen pattern. */
static uint32_t next_lpn(workload_t w, uint64_t i)
{
    const uint32_t N = FTL_LOGICAL_PAGES;
    uint32_t hot_span;
    double   u;

    switch (w) {
    case WL_SEQUENTIAL:
        return (uint32_t)(i % N);

    case WL_RANDOM:
        return (uint32_t)(wl_rand() % N);

    case WL_HOTCOLD:
        /* The classic 90/10 rule. This is the pattern that separates greedy
         * from cost-benefit victim selection, because it is the only one where
         * "this block is cold, its free space will stay free" is true. */
        hot_span = (N / 10u) ? (N / 10u) : 1u;
        if ((wl_rand() % 100u) < 90u) {
            return (uint32_t)(wl_rand() % hot_span);
        }
        return hot_span + (uint32_t)(wl_rand() % (N - hot_span));

    case WL_ZIPF:
        /* A power-law approximation, not a true Zipf: u^3 concentrates mass
         * near zero with a long tail. Calling it what it is matters -- a real
         * Zipf sampler needs a normalising constant over N terms, and claiming
         * to have one when you do not is the sort of thing that unravels badly
         * when someone asks how you computed it. */
        u = wl_rand_unit();
        return (uint32_t)(u * u * u * (double)N) % N;

    case WL_MIXED:
        /* 64-page sequential bursts interleaved with random single writes --
         * roughly what a filesystem doing writeback plus metadata updates
         * looks like. */
        if ((i / 64u) % 2u == 0u) {
            return (uint32_t)(i % N);
        }
        return (uint32_t)(wl_rand() % N);

    default:
        return 0u;
    }
}

/* ---------------------------------------------------------------------------
 * Reporting
 * ------------------------------------------------------------------------- */
static void print_human_report(const config_t *cfg)
{
    const ftl_stats_t     *s  = ftl_stats();
    const nand_counters_t *nc = nand_counters();
    uint32_t emin, emax;
    double   emean, estd;
    uint64_t total_pgm = ftl_total_page_programs(s);

    ftl_erase_distribution(&emin, &emax, &emean, &estd);

    printf("\n");
    printf("================================================================\n");
    printf(" NAND FTL SIMULATOR -- RESULTS\n");
    printf("================================================================\n");

    printf("\n-- Device geometry ---------------------------------------------\n");
    printf("  page size                 %u B\n",   (unsigned)NAND_PAGE_SIZE_BYTES);
    printf("  pages per block           %u\n",     (unsigned)NAND_PAGES_PER_BLOCK);
    printf("  blocks                    %u  (%u metadata, %u data)\n",
           (unsigned)NAND_NUM_BLOCKS, (unsigned)FTL_META_BLOCKS,
           (unsigned)FTL_DATA_BLOCKS);
    printf("  physical capacity         %.2f MiB\n", (double)NAND_TOTAL_BYTES / 1048576.0);
    printf("  exposed capacity          %.2f MiB  (%u%% over-provisioned)\n",
           (double)FTL_LOGICAL_BYTES / 1048576.0, (unsigned)FTL_OP_PERCENT);
    printf("  endurance rating          %u P/E cycles per block\n",
           (unsigned)NAND_MAX_ERASE_CYCLES);

    printf("\n-- Configuration -----------------------------------------------\n");
    printf("  mapping scheme            %s\n", ftl_map_scheme_name(cfg->params.mapping));
    printf("  GC victim policy          %s\n", ftl_gc_policy_name(cfg->params.gc));
    printf("  wear levelling            %s\n", ftl_wear_policy_name(cfg->params.wear));
    printf("  checkpointing             %s\n", cfg->params.checkpointing ? "on" : "off");
    printf("  read-disturb refresh      %s\n", cfg->params.read_disturb_refresh ? "on" : "off");
    printf("  workload                  %s\n", cfg->label);
    printf("  preconditioning fill      %u%% of exposed capacity\n",
           (unsigned)cfg->fill_percent);
    printf("  measured operations       %llu\n", (unsigned long long)cfg->ops);
    printf("  seed                      %llu\n", (unsigned long long)cfg->params.seed);

    printf("\n-- Controller memory -------------------------------------------\n");
    printf("  mapping table             %llu B\n",
           (unsigned long long)ftl_mapping_table_bytes());
    printf("  full FTL context          %llu B\n",
           (unsigned long long)ftl_context_bytes());
    printf("  table bytes per MiB flash %.1f B\n",
           (double)ftl_mapping_table_bytes() / ((double)NAND_TOTAL_BYTES / 1048576.0));

    printf("\n-- Host work ---------------------------------------------------\n");
    printf("  host page writes          %llu\n", (unsigned long long)s->host_writes);
    printf("  host page reads           %llu\n", (unsigned long long)s->host_reads);
    printf("  host trims                %llu\n", (unsigned long long)s->host_trims);

    printf("\n-- Flash work (page programs, by cause) ------------------------\n");
    printf("  host data                 %10llu\n", (unsigned long long)s->pgm_host);
    printf("  garbage collection        %10llu\n", (unsigned long long)s->pgm_gc);
    printf("  hybrid merges             %10llu\n", (unsigned long long)s->pgm_merge);
    printf("  block-map read-mod-write  %10llu\n", (unsigned long long)s->pgm_rmw);
    printf("  padding                   %10llu\n", (unsigned long long)s->pgm_pad);
    printf("  static wear levelling     %10llu\n", (unsigned long long)s->pgm_wear);
    printf("  read-disturb refresh      %10llu\n", (unsigned long long)s->pgm_refresh);
    printf("  bad-block recovery        %10llu\n", (unsigned long long)s->pgm_recover);
    printf("  metadata checkpoints      %10llu\n", (unsigned long long)s->pgm_meta);
    printf("  ----------------------------------\n");
    printf("  total                     %10llu\n", (unsigned long long)total_pgm);
    printf("  block erases              %10llu\n", (unsigned long long)s->erases);

    printf("\n-- WRITE AMPLIFICATION -----------------------------------------\n");
    printf("  excluding metadata        %.3f\n", ftl_write_amplification(false));
    printf("  including metadata        %.3f\n", ftl_write_amplification(true));

    printf("\n-- Garbage collection ------------------------------------------\n");
    printf("  invocations               %llu\n", (unsigned long long)s->gc_invocations);
    printf("  blocks collected          %llu\n", (unsigned long long)s->gc_victims);
    if (s->gc_victims > 0u) {
        printf("  pages copied per victim   %.2f  (out of %u)\n",
               (double)s->pgm_gc / (double)s->gc_victims,
               (unsigned)NAND_PAGES_PER_BLOCK);
    }
    printf("  switch merges             %llu\n", (unsigned long long)s->switch_merges);
    printf("  partial merges            %llu\n", (unsigned long long)s->partial_merges);
    printf("  full merges               %llu\n", (unsigned long long)s->full_merges);

    printf("\n-- Wear ---------------------------------------------------------\n");
    printf("  erase count min / max     %u / %u\n", emin, emax);
    printf("  spread (max - min)        %u\n", emax - emin);
    printf("  mean / std deviation      %.2f / %.2f\n", emean, estd);
    if (emean > 0.0) {
        printf("  coefficient of variation  %.3f\n", estd / emean);
    }
    printf("  static WL migrations      %llu\n", (unsigned long long)s->wear_migrations);
    printf("  read-disturb refreshes    %llu\n", (unsigned long long)s->refresh_events);
    if (emax > 0u) {
        printf("  projected host writes to first block death   %.3g\n",
               (double)s->host_writes * (double)NAND_MAX_ERASE_CYCLES / (double)emax);
    }

    printf("\n-- Reliability --------------------------------------------------\n");
    printf("  bad blocks (total)        %u\n", ftl_bad_block_count());
    printf("  blocks retired at runtime %llu\n", (unsigned long long)s->blocks_retired);
    printf("  program failures (die)    %llu\n", (unsigned long long)nc->program_failures);
    printf("  erase failures (die)      %llu\n", (unsigned long long)nc->erase_failures);
    printf("  write retries             %llu\n", (unsigned long long)s->program_retries);
    printf("  pages lost                %llu\n", (unsigned long long)s->pages_lost);
    printf("  free blocks remaining     %u\n", ftl_free_block_count());

    printf("\n-- Modelled time ------------------------------------------------\n");
    printf("  die busy time             %.3f s\n", (double)nc->busy_us / 1e6);
    if (s->host_writes > 0u) {
        printf("  per host write            %.1f us\n",
               (double)nc->busy_us / (double)s->host_writes);
        printf("  effective write bandwidth %.2f MiB/s\n",
               ((double)s->host_writes * NAND_PAGE_SIZE_BYTES / 1048576.0) /
               ((double)nc->busy_us / 1e6));
    }

    printf("\n-- Self checks --------------------------------------------------\n");
    printf("  program accounting        %s\n", ftl_stats_selfcheck() ? "BALANCED" : "*** MISMATCH ***");
    printf("  FTL invariants            %s\n", ftl_check_invariants() ? "HOLD" : "*** VIOLATED ***");
    printf("================================================================\n\n");
}

static void print_csv_header(void)
{
    printf("mapping,gc,wear,workload,ops,fill_pct,seed,"
           "host_writes,pgm_host,pgm_gc,pgm_merge,pgm_rmw,pgm_pad,pgm_wear,"
           "pgm_refresh,pgm_recover,pgm_meta,total_pgm,erases,"
           "wa_nometa,wa_meta,gc_victims,switch_merges,partial_merges,full_merges,"
           "erase_min,erase_max,erase_spread,erase_mean,erase_stddev,"
           "map_table_bytes,bad_blocks,busy_us\n");
}

static void print_csv_row(const config_t *cfg)
{
    const ftl_stats_t     *s  = ftl_stats();
    const nand_counters_t *nc = nand_counters();
    uint32_t emin, emax;
    double   emean, estd;

    ftl_erase_distribution(&emin, &emax, &emean, &estd);

    printf("%s,%s,%s,%s,%llu,%u,%llu,"
           "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
           "%.4f,%.4f,%llu,%llu,%llu,%llu,"
           "%u,%u,%u,%.3f,%.3f,%llu,%u,%llu\n",
           ftl_map_scheme_name(cfg->params.mapping),
           ftl_gc_policy_name(cfg->params.gc),
           ftl_wear_policy_name(cfg->params.wear),
           cfg->label,
           (unsigned long long)cfg->ops,
           (unsigned)cfg->fill_percent,
           (unsigned long long)cfg->params.seed,
           (unsigned long long)s->host_writes,
           (unsigned long long)s->pgm_host,
           (unsigned long long)s->pgm_gc,
           (unsigned long long)s->pgm_merge,
           (unsigned long long)s->pgm_rmw,
           (unsigned long long)s->pgm_pad,
           (unsigned long long)s->pgm_wear,
           (unsigned long long)s->pgm_refresh,
           (unsigned long long)s->pgm_recover,
           (unsigned long long)s->pgm_meta,
           (unsigned long long)ftl_total_page_programs(s),
           (unsigned long long)s->erases,
           ftl_write_amplification(false),
           ftl_write_amplification(true),
           (unsigned long long)s->gc_victims,
           (unsigned long long)s->switch_merges,
           (unsigned long long)s->partial_merges,
           (unsigned long long)s->full_merges,
           emin, emax, emax - emin, emean, estd,
           (unsigned long long)ftl_mapping_table_bytes(),
           ftl_bad_block_count(),
           (unsigned long long)nc->busy_us);
}

/* ---------------------------------------------------------------------------
 * Trace replay
 * ------------------------------------------------------------------------- */
static int run_trace(const config_t *cfg)
{
    FILE    *f = fopen(cfg->trace_path, "r");
    char     op;
    uint32_t lpn;
    uint64_t n = 0u;

    if (f == NULL) {
        fprintf(stderr, "cannot open trace '%s'\n", cfg->trace_path);
        return 1;
    }
    while (fscanf(f, " %c %u", &op, &lpn) == 2) {
        if (lpn >= FTL_LOGICAL_PAGES) {
            continue;
        }
        if (op == 'W' || op == 'w') {
            s_version[lpn]++;
            fill_pattern(s_page, lpn, s_version[lpn]);
            if (ftl_write(lpn, s_page) != FTL_OK) {
                fprintf(stderr, "write failed at trace op %llu\n",
                        (unsigned long long)n);
                fclose(f);
                return 1;
            }
        } else if (op == 'R' || op == 'r') {
            (void)ftl_read(lpn, s_readback);
        } else if (op == 'T' || op == 't') {
            (void)ftl_trim(lpn);
            s_version[lpn] = 0u;
        }
        n++;
    }
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------------------
 * Argument parsing
 * ------------------------------------------------------------------------- */
static void usage(void)
{
    printf(
    "Usage: ftlsim [options]\n"
    "\n"
    "  --mapping    page | block | hybrid      (default page)\n"
    "  --gc         greedy | costbenefit | fifo | random   (default greedy)\n"
    "  --wear       none | dynamic | static    (default dynamic)\n"
    "  --workload   seq | rand | hotcold | zipf | mixed    (default rand)\n"
    "  --ops N              number of measured host writes (default 200000)\n"
    "  --fill PCT           precondition to PCT%% of exposed capacity (default 100)\n"
    "  --seed S             PRNG seed (default 1)\n"
    "  --trace FILE         replay a trace from tools/workload_gen.py\n"
    "  --program-fail-ppm N inject program failures (parts per million)\n"
    "  --erase-fail-ppm N   inject erase failures (parts per million)\n"
    "  --no-checkpoint      disable metadata checkpointing\n"
    "  --no-refresh         disable read-disturb refresh\n"
    "  --verify             read back and verify every logical page at the end\n"
    "  --csv                emit one CSV row instead of a report\n"
    "  --csv-header         print the CSV header and exit\n"
    "  --quiet              suppress progress output\n"
    "  --help\n");
}

static bool parse_args(int argc, char **argv, config_t *cfg)
{
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];

        #define NEXT() (++i < argc ? argv[i] : "")

        if (!strcmp(a, "--help")) { usage(); return false; }
        else if (!strcmp(a, "--csv-header")) { print_csv_header(); return false; }
        else if (!strcmp(a, "--mapping")) {
            const char *v = NEXT();
            if (!strcmp(v, "page"))        cfg->params.mapping = FTL_MAP_PAGE;
            else if (!strcmp(v, "block"))  cfg->params.mapping = FTL_MAP_BLOCK;
            else if (!strcmp(v, "hybrid")) cfg->params.mapping = FTL_MAP_HYBRID;
            else { fprintf(stderr, "unknown mapping '%s'\n", v); return false; }
        }
        else if (!strcmp(a, "--gc")) {
            const char *v = NEXT();
            if (!strcmp(v, "greedy"))            cfg->params.gc = FTL_GC_GREEDY;
            else if (!strcmp(v, "costbenefit"))  cfg->params.gc = FTL_GC_COST_BENEFIT;
            else if (!strcmp(v, "fifo"))         cfg->params.gc = FTL_GC_FIFO;
            else if (!strcmp(v, "random"))       cfg->params.gc = FTL_GC_RANDOM;
            else { fprintf(stderr, "unknown gc policy '%s'\n", v); return false; }
        }
        else if (!strcmp(a, "--wear")) {
            const char *v = NEXT();
            if (!strcmp(v, "none"))          cfg->params.wear = FTL_WEAR_NONE;
            else if (!strcmp(v, "dynamic"))  cfg->params.wear = FTL_WEAR_DYNAMIC;
            else if (!strcmp(v, "static"))   cfg->params.wear = FTL_WEAR_STATIC;
            else { fprintf(stderr, "unknown wear policy '%s'\n", v); return false; }
        }
        else if (!strcmp(a, "--workload")) {
            const char *v = NEXT();
            cfg->label = v;
            if (!strcmp(v, "seq"))          cfg->workload = WL_SEQUENTIAL;
            else if (!strcmp(v, "rand"))    cfg->workload = WL_RANDOM;
            else if (!strcmp(v, "hotcold")) cfg->workload = WL_HOTCOLD;
            else if (!strcmp(v, "zipf"))    cfg->workload = WL_ZIPF;
            else if (!strcmp(v, "mixed"))   cfg->workload = WL_MIXED;
            else { fprintf(stderr, "unknown workload '%s'\n", v); return false; }
        }
        else if (!strcmp(a, "--ops"))   cfg->ops = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--fill"))  cfg->fill_percent = (uint32_t)strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--seed"))  cfg->params.seed = strtoull(NEXT(), NULL, 10);
        else if (!strcmp(a, "--trace")) { cfg->trace_path = NEXT(); cfg->workload = WL_TRACE; cfg->label = "trace"; }
        else if (!strcmp(a, "--program-fail-ppm")) cfg->program_fail_ppm = (uint32_t)strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--erase-fail-ppm"))   cfg->erase_fail_ppm   = (uint32_t)strtoul(NEXT(), NULL, 10);
        else if (!strcmp(a, "--no-checkpoint")) cfg->params.checkpointing = false;
        else if (!strcmp(a, "--no-refresh"))    cfg->params.read_disturb_refresh = false;
        else if (!strcmp(a, "--verify")) cfg->verify = true;
        else if (!strcmp(a, "--csv"))    cfg->csv = true;
        else if (!strcmp(a, "--quiet"))  cfg->quiet = true;
        else { fprintf(stderr, "unknown option '%s'\n", a); usage(); return false; }

        #undef NEXT
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    config_t  cfg;
    uint64_t  i, fill_pages;
    uint32_t  lpn;
    ftl_err_t err;
    uint64_t  verify_errors = 0u, verify_checked = 0u;

    memset(&cfg, 0, sizeof(cfg));
    ftl_params_defaults(&cfg.params);
    cfg.workload     = WL_RANDOM;
    cfg.label        = "rand";
    cfg.ops          = 200000u;
    cfg.fill_percent = 100u;

    if (!parse_args(argc, argv, &cfg)) {
        return 0;
    }

    s_wl_rng = cfg.params.seed ^ 0x1234567890ABCDEFULL;
    memset(s_version, 0, sizeof(s_version));

    err = ftl_format(&cfg.params);
    if (err != FTL_OK) {
        fprintf(stderr, "format failed: %s\n", ftl_strerror(err));
        return 1;
    }

    /* ---- STEP 1: PRECONDITION ------------------------------------------
     * Fill the exposed capacity sequentially. After this the drive is "full"
     * from the FTL's point of view -- every logical page has a physical home --
     * and any further write must displace something, which is the only regime
     * in which write amplification means anything. */
    fill_pages = ((uint64_t)FTL_LOGICAL_PAGES * cfg.fill_percent) / 100u;
    for (i = 0u; i < fill_pages; i++) {
        lpn = (uint32_t)i;
        s_version[lpn] = 1u;
        fill_pattern(s_page, lpn, 1u);
        err = ftl_write(lpn, s_page);
        if (err != FTL_OK) {
            fprintf(stderr, "preconditioning write of LPN %u failed: %s\n",
                    lpn, ftl_strerror(err));
            return 1;
        }
    }
    if (!cfg.quiet) {
        fprintf(stderr, "preconditioned %llu pages (%.1f MiB)\n",
                (unsigned long long)fill_pages,
                (double)fill_pages * NAND_PAGE_SIZE_BYTES / 1048576.0);
    }

    /* ---- STEP 2: RESET THE COUNTERS ------------------------------------
     * Everything before this point was setup. Everything after is measurement. */
    ftl_stats_reset();

    /* Failure injection is switched on only for the measured phase, so that a
     * bad block cannot appear during preconditioning and quietly change the
     * starting conditions. */
    nand_set_failure_rates(cfg.program_fail_ppm, cfg.erase_fail_ppm);

    /* ---- STEP 3: RUN THE WORKLOAD -------------------------------------- */
    if (cfg.workload == WL_TRACE) {
        if (run_trace(&cfg) != 0) {
            return 1;
        }
        cfg.ops = ftl_stats()->host_writes;
    } else {
        for (i = 0u; i < cfg.ops; i++) {
            lpn = next_lpn(cfg.workload, i);
            s_version[lpn]++;
            fill_pattern(s_page, lpn, s_version[lpn]);

            err = ftl_write(lpn, s_page);
            if (err != FTL_OK) {
                fprintf(stderr, "write of LPN %u failed at op %llu: %s\n",
                        lpn, (unsigned long long)i, ftl_strerror(err));
                break;
            }

            if (!cfg.quiet && cfg.ops >= 20u && (i % (cfg.ops / 20u)) == 0u) {
                fprintf(stderr, "\r  %3u%%", (unsigned)(100u * i / cfg.ops));
                fflush(stderr);
            }
        }
        if (!cfg.quiet) {
            fprintf(stderr, "\r  100%%\n");
        }
    }

    /* ---- STEP 4: VERIFY ------------------------------------------------- */
    if (cfg.verify) {
        for (lpn = 0u; lpn < FTL_LOGICAL_PAGES; lpn++) {
            if (s_version[lpn] == 0u) {
                continue;
            }
            verify_checked++;
            if (ftl_read(lpn, s_readback) != FTL_OK ||
                !check_pattern(s_readback, lpn, s_version[lpn])) {
                verify_errors++;
                if (verify_errors <= 5u) {
                    fprintf(stderr, "VERIFY FAILED: lpn %u (expected version %u)\n",
                            lpn, s_version[lpn]);
                }
            }
        }
    }

    /* ---- STEP 5: REPORT ------------------------------------------------- */
    if (cfg.csv) {
        print_csv_row(&cfg);
    } else {
        print_human_report(&cfg);
        if (cfg.verify) {
            printf("-- Data verification --------------------------------------------\n");
            printf("  logical pages checked     %llu\n", (unsigned long long)verify_checked);
            printf("  mismatches                %llu %s\n\n",
                   (unsigned long long)verify_errors,
                   verify_errors ? "*** DATA LOSS ***" : "(clean)");
        }
    }

    return (verify_errors > 0u) ? 2 : 0;
}
