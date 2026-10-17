#!/usr/bin/env python3
"""Run every reference solution and confirm it prints the expected answer."""
import argparse
import sys
import tempfile

import bench


def check(problem):
    with tempfile.TemporaryDirectory(prefix="fwiz-ref-") as wd:
        ref = problem["reference"]
        bench.write_files(wd, ref["files"])
        run = bench.run_fwiz(wd, ref["command"])
    return bench.score(problem, run["stdout"]), run


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--problems", default="*", help="glob over problem ids (default: all)")
    args = ap.parse_args()
    problems = bench.load_problems(args.problems)
    failed = 0
    for p in problems:
        s, run = check(p)
        if s["pass"]:
            print(f"ok    {p['id']}")
        else:
            failed += 1
            print(f"FAIL  {p['id']}: expected {s['expected']}, got {s['got']}\n"
                  f"      fwiz {run['command']}\n      {(run['stdout'] + run['stderr']).strip()}")
    print(f"{len(problems) - failed}/{len(problems)} reference solutions match")
    return 1 if failed or not problems else 0


if __name__ == "__main__":
    sys.exit(main())
