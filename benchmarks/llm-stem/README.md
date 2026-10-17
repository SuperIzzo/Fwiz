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
