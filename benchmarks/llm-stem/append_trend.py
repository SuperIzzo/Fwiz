#!/usr/bin/env python3
"""Append one dated row (date, git sha, mode, pass rate, per-category) from a run.py results dir
to the trend file (default .fwiz-workflow/llm-benchmark-history.md; created if missing)."""
import argparse
import datetime
import json
import os
import subprocess
import sys

import bench

DEFAULT_HISTORY = os.path.join(bench.REPO, ".fwiz-workflow", "llm-benchmark-history.md")
HEADER = ("# LLM benchmark history\n\n"
          "One row per `make benchmark` run. Compare a new row with the last one at arc close.\n\n"
          "| Date | Commit | Mode | Passed | Rate | By category |\n"
          "|---|---|---|---|---|---|\n")


def git_sha():
    p = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=bench.REPO,
                       capture_output=True, text=True)
    return p.stdout.strip() if p.returncode == 0 else "unknown"


def make_row(data, sha, today):
    res = data["results"]
    n, ok = len(res), sum(r["pass"] for r in res)
    cats = {}
    for r in res:
        c = cats.setdefault(r["category"], [0, 0])
        c[0] += r["pass"]
        c[1] += 1
    by_cat = ", ".join(f"{c} {a}/{b}" for c, (a, b) in sorted(cats.items()))
    mode = "dry-run" if data.get("dry_run") else data.get("model") or "?"
    rate = f"{100 * ok / n:.0f}%" if n else "n/a"
    return f"| {today} | {sha} | {mode} | {ok}/{n} | {rate} | {by_cat} |\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("results_dir", help="run.py output dir containing results.json")
    ap.add_argument("--history", default=DEFAULT_HISTORY, help="trend file to append to")
    args = ap.parse_args(argv)
    path = os.path.join(args.results_dir, "results.json")
    if not os.path.isfile(path):
        print(f"no results.json in {args.results_dir}; the runner did not finish", file=sys.stderr)
        return 2
    with open(path) as f:
        data = json.load(f)
    if not data.get("results"):
        print("results.json has no results", file=sys.stderr)
        return 2
    row = make_row(data, git_sha(), datetime.date.today().isoformat())
    os.makedirs(os.path.dirname(os.path.abspath(args.history)), exist_ok=True)
    new = not os.path.exists(args.history)
    with open(args.history, "a") as f:
        if new:
            f.write(HEADER)
        f.write(row)
    print(f"appended to {args.history}:\n{row}", end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
