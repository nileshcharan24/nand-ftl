#!/usr/bin/env python3
"""Run a small reproducible FTL policy sweep and write CSV results."""

import argparse
import csv
import os
import subprocess
import sys


def run(binary, mapping, gc, wear, workload, ops, seed):
    command = [
        binary,
        "--mapping", mapping,
        "--gc", gc,
        "--wear", wear,
        "--workload", workload,
        "--ops", str(ops),
        "--seed", str(seed),
        "--csv",
        "--quiet",
    ]
    result = subprocess.run(command, check=True, capture_output=True, text=True)
    rows = list(csv.DictReader(result.stdout.splitlines()))
    if len(rows) != 1:
        raise RuntimeError("benchmark did not produce exactly one CSV row")
    return rows[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--ops", type=int, default=20000)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    mappings = ("page", "block", "hybrid")
    policies = ("greedy", "costbenefit", "fifo", "random")
    workloads = ("seq", "rand", "hotcold")
    rows = []
    for mapping in mappings:
        selected_policies = policies if mapping == "page" else ("greedy",)
        for gc in selected_policies:
            for workload in workloads:
                rows.append(run(args.binary, mapping, gc, "dynamic", workload,
                                 args.ops, args.seed))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", newline="", encoding="ascii") as output:
        writer = csv.DictWriter(output, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    print(f"wrote {len(rows)} benchmark rows to {args.out}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"experiment failed: {error}", file=sys.stderr)
        sys.exit(1)
