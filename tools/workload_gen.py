#!/usr/bin/env python3
"""Generate deterministic W/R/T traces for the FTL simulator."""

import argparse
import random


def choose_lpn(rng, pattern, index, pages):
    if pattern == "sequential":
        return index % pages
    if pattern == "random":
        return rng.randrange(pages)
    if pattern == "hotcold":
        hot = max(1, pages // 10)
        return rng.randrange(hot) if rng.randrange(100) < 90 else rng.randrange(hot, pages)
    if pattern == "mixed":
        return index % pages if (index // 64) % 2 == 0 else rng.randrange(pages)
    raise ValueError(pattern)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pattern", choices=("sequential", "random", "hotcold", "mixed"), default="random")
    parser.add_argument("--ops", type=int, default=1000)
    parser.add_argument("--pages", type=int, default=5952)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--read-percent", type=int, default=0)
    parser.add_argument("--trim-percent", type=int, default=0)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    if args.ops < 0 or args.pages <= 0:
        parser.error("ops must be non-negative and pages must be positive")
    if args.read_percent < 0 or args.trim_percent < 0 or args.read_percent + args.trim_percent > 100:
        parser.error("read-percent plus trim-percent must be between 0 and 100")

    rng = random.Random(args.seed)
    with open(args.out, "w", encoding="ascii", newline="\n") as trace:
        for index in range(args.ops):
            roll = rng.randrange(100)
            lpn = choose_lpn(rng, args.pattern, index, args.pages)
            if roll < args.trim_percent:
                op = "T"
            elif roll < args.trim_percent + args.read_percent:
                op = "R"
            else:
                op = "W"
            trace.write(f"{op} {lpn}\n")


if __name__ == "__main__":
    main()
