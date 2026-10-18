#!/usr/bin/env python3
"""Run STEM problems through an LLM that writes .fw files and fwiz commands, then score them.

The LLM replies with fenced blocks:
    ```fw name.fw        -> file written to the scratch dir
    ```fwiz <arguments>  -> run as `fwiz <arguments>` (no shell)
The runner feeds fwiz output back, up to --max-runs fwiz runs, and stops early once
the last output scores as correct.
"""
import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import tempfile
import time

import bench

DEFAULT_DOCS = "README.md,docs/Language.md"
LLM_TIMEOUT = 300

SYSTEM_PROMPT = """You solve STEM problems with fwiz, a bidirectional equation solver. Reference docs follow.

Reply protocol (plain text, no tools available):
- To create a file, write a fenced block whose first line is "```fw NAME.fw" followed by the file contents.
- To run fwiz, write a fenced block whose first line is "```fwiz ARGS" (arguments after `fwiz`, shell-quoted, e.g. "```fwiz 'NAME.fw(x=?, y=3)'"). The block body is ignored.
- You may use at most {max_runs} fwiz runs; outputs are returned to you so you can fix errors.
- Stop once fwiz prints the correct value for the answer variable. The final answer must be printed by fwiz as `{var} = value`.

=== DOCS ===
{docs}
"""


def read_docs(names):
    parts = []
    for n in filter(None, (s.strip() for s in names.split(","))):
        path = os.path.join(bench.REPO, n)
        with open(path) as f:
            parts.append(f"--- {n} ---\n{f.read()}")
    return "\n\n".join(parts)


def call_claude(system, prompt, model, cwd):
    """The one LLM backend: Claude Code headless, no settings/tools, outside the repo."""
    cmd = ["claude", "-p", "--model", model, "--safe-mode", "--setting-sources", "",
           "--tools", "", "--no-session-persistence", "--system-prompt", system]
    p = subprocess.run(cmd, input=prompt, cwd=cwd, capture_output=True, text=True,
                       timeout=LLM_TIMEOUT)
    if p.returncode != 0:
        raise RuntimeError(f"claude exited {p.returncode}: {p.stderr.strip()[:500]}")
    return p.stdout


_BLOCK = re.compile(r"```(fwiz|fw)[ \t]+([^\n]*)\n(.*?)```", re.S)


def parse_reply(text):
    """Return (files {name: body}, commands [str]) in order of appearance."""
    files, cmds = {}, []
    for m in _BLOCK.finditer(text):
        kind, head, body = m.groups()
        if kind == "fw":
            files[os.path.basename(head.strip())] = body
        else:
            cmds.append(head.strip())
    return files, cmds


def format_feedback(runs):
    out = []
    for r in runs:
        out.append(f"$ fwiz {r['command']}\n(exit {r['exit']})\n{r['stdout']}{r['stderr']}")
    return "\n".join(out)


def llm_attempt(problem, args, docs):
    """Multi-turn conversation; each turn resends the full transcript (stateless backend)."""
    var = problem["answer"]["var"]
    system = SYSTEM_PROMPT.format(max_runs=args.max_runs, var=var, docs=docs)
    transcript = f"Problem: {problem['statement']}\n\nAnswer variable: {var}\n"
    files, runs, turns, replies = {}, [], 0, []
    with tempfile.TemporaryDirectory(prefix="fwiz-llm-") as llm_cwd, \
            tempfile.TemporaryDirectory(prefix="fwiz-work-") as wd:
        while turns < args.max_turns and len(runs) < args.max_runs:
            turns += 1
            reply = args.backend(system, transcript, args.model, llm_cwd)
            replies.append(reply)
            new_files, cmds = parse_reply(reply)
            files.update(new_files)
            bench.write_files(wd, new_files)
            if not cmds:
                break
            batch = [bench.run_fwiz(wd, c) for c in cmds[:args.max_runs - len(runs)]]
            runs.extend(batch)
            if bench.score(problem, batch[-1]["stdout"])["pass"]:
                break
            transcript += f"\n[assistant]\n{reply}\n\n[fwiz output]\n{format_feedback(batch)}\n"
    return files, runs, turns, replies


def dry_attempt(problem, args, docs):
    ref = problem["reference"]
    with tempfile.TemporaryDirectory(prefix="fwiz-work-") as wd:
        bench.write_files(wd, ref["files"])
        runs = [bench.run_fwiz(wd, ref["command"])]
    files = {os.path.basename(k): (v if isinstance(v, str) else "\n".join(v) + "\n")
             for k, v in ref["files"].items()}
    return files, runs, 0, []


def run_problem(problem, args, docs):
    t0 = time.time()
    try:
        files, runs, turns, replies = (dry_attempt if args.dry_run else llm_attempt)(problem, args, docs)
        error = None
    except Exception as e:  # recorded as a failed attempt, not swallowed
        files, runs, turns, replies, error = {}, [], 0, [], f"{type(e).__name__}: {e}"
    final = bench.score(problem, runs[-1]["stdout"]) if runs else \
        {"pass": False, "got": [], "expected": problem["answer"]["values"]}
    return {"id": problem["id"], "category": problem.get("category"), "pass": final["pass"],
            "got": final["got"], "expected": final["expected"], "turns": turns,
            "fwiz_runs": len(runs), "seconds": round(time.time() - t0, 2), "files": files,
            "commands": [r["command"] for r in runs], "runs": runs, "replies": replies,
            "error": error}


def summarize(results, args):
    n = len(results)
    ok = sum(r["pass"] for r in results)
    lines = [f"# LLM STEM run", "",
             f"- Mode: {'dry-run (reference solutions)' if args.dry_run else 'model ' + args.model}",
             f"- Passed: {ok}/{n}", "",
             "| Problem | Category | Result | Turns | fwiz runs | Seconds |", "|---|---|---|---|---|---|"]
    for r in results:
        lines.append(f"| {r['id']} | {r['category']} | {'pass' if r['pass'] else 'FAIL'} | "
                     f"{r['turns']} | {r['fwiz_runs']} | {r['seconds']} |")
    cats = {}
    for r in results:
        c = cats.setdefault(r["category"], [0, 0])
        c[0] += r["pass"]
        c[1] += 1
    lines += ["", "| Category | Passed |", "|---|---|"]
    lines += [f"| {c} | {a}/{b} |" for c, (a, b) in sorted(cats.items())]
    return "\n".join(lines) + "\n"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--problems", default="*", help="glob over problem ids (default: all)")
    ap.add_argument("--model", default="haiku", help="model passed to claude -p (default: haiku)")
    ap.add_argument("--dry-run", action="store_true", help="score reference solutions, no LLM")
    ap.add_argument("--docs", default=DEFAULT_DOCS, help=f"comma list of repo docs (default: {DEFAULT_DOCS})")
    ap.add_argument("--max-runs", type=int, default=3, help="fwiz runs per problem (default 3)")
    ap.add_argument("--max-turns", type=int, default=4, help="LLM turns per problem (default 4)")
    ap.add_argument("--out", default=None, help="output dir (default: results/<timestamp>)")
    args = ap.parse_args(argv)
    args.backend = call_claude

    problems = bench.load_problems(args.problems)
    if not problems:
        print("no problems match", args.problems, file=sys.stderr)
        return 2
    docs = "" if args.dry_run else read_docs(args.docs)
    out = args.out or os.path.join(bench.HERE, "results",
                                   datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    os.makedirs(out, exist_ok=True)
    results = []
    for p in problems:
        r = run_problem(p, args, docs)
        results.append(r)
        print(f"{'ok  ' if r['pass'] else 'FAIL'}  {r['id']}" + (f"  [{r['error']}]" if r["error"] else ""))
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump({"model": None if args.dry_run else args.model, "dry_run": args.dry_run,
                   "results": results}, f, indent=2)
    summary = summarize(results, args)
    with open(os.path.join(out, "summary.md"), "w") as f:
        f.write(summary)
    ok = sum(r["pass"] for r in results)
    print(f"{ok}/{len(results)} passed -> {out}")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
