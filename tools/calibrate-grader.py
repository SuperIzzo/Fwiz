#!/usr/bin/env python3
"""
calibrate-grader.py — Compare local LLM grader responses on a single prompt.

Used to calibrate the blind-spot-critic's Haiku grader against alternative
local models via llama-swap. Runs models sequentially — local-LLM RAM/VRAM
constraints don't tolerate parallel runs.

For Haiku/Claude comparison, spawn the `code-explainer-purpose` agent (or any
of the explainer agents) directly via Claude Code's Agent tool with the same
prompt. This script handles only the llama-swap side.

Usage:
  ./tools/calibrate-grader.py --prompt /tmp/calibration-prompt.txt
  ./tools/calibrate-grader.py --prompt prompt.txt --models qwen3-6-35b-a3b-iq4-nl-xl gemma-4-e2b-q4-xl
  ./tools/calibrate-grader.py --prompt prompt.txt --out ./calibration-2026-05-10

Writes per-model outputs to <out-dir>/<model>.md plus a summary table at
<out-dir>/summary.md.

Default model list comes from ~/Documents/LogSeq/Notebook/pages/Local LLM
Benchmarks.md — review-bench winners + the Gemma E2B floor candidate.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

LLAMA_SWAP_URL = os.environ.get("LLAMA_SWAP_URL", "http://localhost:8080/v1/chat/completions")

DEFAULT_MODELS = [
    "qwen3-6-35b-a3b-iq4-nl-xl",  # review winner; strong calibrated baseline
    "gemma-4-26b-a4b-q5",          # mid-Gemma
    "gemma-4-e2b-q4-xl",           # likely real floor (Q4 may calibrate better than Q8)
    "gemma-4-e2b-q8-xl",           # cross-check vs opencode-tested run
]


def llama_swap_alive() -> bool:
    """Check if llama-swap is responding at LLAMA_SWAP_URL."""
    base = LLAMA_SWAP_URL.split("/v1/")[0]
    try:
        with urllib.request.urlopen(f"{base}/v1/models", timeout=2) as resp:
            return resp.status == 200
    except (urllib.error.URLError, urllib.error.HTTPError, OSError):
        return False


def ensure_llama_swap_running() -> None:
    """Start llama-swap via systemd user unit if it's not already responding."""
    if llama_swap_alive():
        return
    print("llama-swap not responding — starting via `systemctl --user start llama-swap.service`...", flush=True)
    try:
        subprocess.run(
            ["systemctl", "--user", "start", "llama-swap.service"],
            check=False, timeout=10,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as e:
        print(f"systemctl unavailable or timed out ({e}); manual start required.", file=sys.stderr)
        sys.exit(1)
    # poll for readiness
    for _ in range(20):
        time.sleep(1)
        if llama_swap_alive():
            print("llama-swap is up.", flush=True)
            return
    print("ERROR: llama-swap did not come up within 20s.", file=sys.stderr)
    sys.exit(1)


def call_llama_swap(model: str, prompt: str, max_tokens: int, timeout: int) -> tuple[str, float]:
    body = json.dumps({
        "model": model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0,
        "max_tokens": max_tokens,
    }).encode("utf-8")
    req = urllib.request.Request(
        LLAMA_SWAP_URL,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            data = json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", errors="replace")[:500]
        return f"[HTTP {e.code}: {body}]", time.time() - t0
    except urllib.error.URLError as e:
        return f"[CONNECTION ERROR: {e}]", time.time() - t0
    elapsed = time.time() - t0
    if "error" in data:
        return f"[API ERROR: {data['error']}]", elapsed
    try:
        return data["choices"][0]["message"]["content"], elapsed
    except (KeyError, IndexError):
        return f"[UNEXPECTED RESPONSE: {json.dumps(data)[:500]}]", elapsed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--prompt", required=True, help="prompt file")
    ap.add_argument("--models", nargs="*", default=DEFAULT_MODELS, help="llama-swap model IDs")
    ap.add_argument("--out", default="/tmp/grader-calibration", help="output directory")
    ap.add_argument("--max-tokens", type=int, default=1024)
    ap.add_argument("--timeout", type=int, default=600, help="per-model timeout in seconds")
    ap.add_argument("--no-auto-start", action="store_true", help="skip starting llama-swap if it isn't running")
    args = ap.parse_args()

    if not args.no_auto_start:
        ensure_llama_swap_running()
    elif not llama_swap_alive():
        print(f"ERROR: llama-swap not responding at {LLAMA_SWAP_URL}", file=sys.stderr)
        return 1

    prompt_path = Path(args.prompt)
    if not prompt_path.exists():
        print(f"prompt file not found: {prompt_path}", file=sys.stderr)
        return 1
    prompt = prompt_path.read_text()

    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    results: list[dict] = []
    for model in args.models:
        print(f"\n==== {model} ====", flush=True)
        text, elapsed = call_llama_swap(model, prompt, args.max_tokens, args.timeout)
        print(text)
        print(f"\n[elapsed: {elapsed:.1f}s]")
        (out_dir / f"{model}.md").write_text(
            f"# {model}\n\nelapsed: {elapsed:.1f}s\n\n---\n\n{text}\n"
        )
        results.append({"model": model, "response": text, "elapsed_s": round(elapsed, 1)})

    summary_lines = [
        "# Grader calibration summary",
        "",
        f"Prompt: `{prompt_path}`",
        f"Generated: {time.strftime('%Y-%m-%dT%H:%M:%S')}",
        "",
        "| Model | Elapsed (s) | First 240 chars (verdict snippet) |",
        "|---|---|---|",
    ]
    for r in results:
        snippet = r["response"][:240].replace("\n", " ").replace("|", "\\|")
        summary_lines.append(f"| `{r['model']}` | {r['elapsed_s']} | {snippet} |")
    summary_lines += ["", "Per-model full outputs in this directory:"]
    for r in results:
        summary_lines.append(f"- `{r['model']}.md`")

    summary_path = out_dir / "summary.md"
    summary_path.write_text("\n".join(summary_lines) + "\n")
    print(f"\nSummary saved to {summary_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
