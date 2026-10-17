# LLM STEM benchmark

Problems an LLM should solve by writing a `.fw` file and running `fwiz`. Wrapper tier: Python stdlib only, no core changes.

## Problem format

One JSON file per problem in `problems/<id>.json`:

| Field | Meaning |
|-------|---------|
| `id` | Unique name, same as the file stem |
| `category` | `algebra`, `physics`, `geometry`, `finance`, `combinatorics`, `calculus`, `recursion`, `reverse-solve`, `multiple-solutions`, `piecewise` |
| `difficulty` | `easy`, `medium`, `hard` |
| `tags` | Extra traits: `reverse`, `units`, `projection`, ... |
| `statement` | Natural-language problem |
| `answer.var` | Variable whose printed value is scored |
| `answer.values` | Every expected solution (order-free) |
| `answer.rel_tol` | Optional relative tolerance (default `1e-6`) |
| `reference.files` | `{filename: [lines]}` written to a scratch dir |
| `reference.command` | Arguments after `fwiz`, shell-quoted |
| `note` | Optional: known pitfalls or fwiz weaknesses the problem exposes |

## Scoring

fwiz runs in a scratch dir with `FWIZ_PATH` set to the repo root, so `@include "stdlib/..."` works. Every line `var = value` or `var ~ value` for `answer.var` is collected; exact forms such as `200 / 9`, `8 * e^2` or `log(10)` are evaluated numerically. A problem passes when the collected values match `answer.values` one-to-one within tolerance: missing or extra roots fail.

## Checking references

```bash
make                                   # build ./bin/fwiz
python3 benchmarks/llm-stem/check_reference.py            # all problems
python3 benchmarks/llm-stem/check_reference.py --problems 'rev-*'
```

## Running an LLM

```bash
python3 benchmarks/llm-stem/run.py --dry-run                      # score references, no LLM (must be 100%)
python3 benchmarks/llm-stem/run.py --problems 'alg-*' --model haiku
python3 benchmarks/llm-stem/test_run.py                           # offline tests with a mock LLM
```

Flags: `--problems <glob>`, `--model` (default `haiku`), `--dry-run`, `--docs a.md,b.md` (repo docs given to the model; default `README.md,docs/Language.md`), `--max-runs` (fwiz runs, default 3), `--max-turns` (LLM turns, default 4), `--out DIR`.

The model runs through `claude -p --safe-mode --setting-sources "" --tools ""` from a temp dir outside the repo, so it cannot read `CLAUDE.md` or project memory. It replies with ```` ```fw name.fw ```` file blocks and ```` ```fwiz <args> ```` command blocks; the runner writes the files, runs fwiz (no shell), and feeds the output back. It stops once fwiz prints a value for the answer variable. The backend is the single function `call_claude` in `run.py`.

Output goes to `results/<timestamp>/` (git-ignored): `results.json` (per attempt: verdict, `.fw` files, commands, fwiz stdout/stderr, replies, turns, seconds) and `summary.md`.

## Trend tracking (`make benchmark`)

```bash
make benchmark                          # full set with the default model; costs LLM tokens
make benchmark BENCH_ARGS=--dry-run     # no LLM, scores references (wiring check; adds a dry-run row)
```

Runs `run.py` into `results/trend-latest/`, then `append_trend.py` appends one row (date, git short sha, model, pass rate, per-category) to `.fwiz-workflow/llm-benchmark-history.md` (git-ignored; created with a header on first run). The target fails with a clear message if the `claude` CLI is not on PATH; it ignores `run.py`'s exit 1 (some problems failed) but stops on any other error. Run it at arc close and compare with the previous row.
