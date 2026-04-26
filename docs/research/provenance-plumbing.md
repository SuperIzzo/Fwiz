# Provenance Plumbing — Research Anchor

**Status:** research complete; design pending.
**Outcome:** Path B shipped 2026-04-26 (commit pending). Known-Issues #6 resolved.
**Tracks:** Known-Issues #6, Future #6.
**Date:** 2026-04-26.

This document records research done before the provenance-plumbing cycle was implemented, so that future cycles (and re-evaluations of related work) have the prior-art and codebase-audit findings without re-running the research.

## Symptom

`--steps` and `--calc` traces render intermediate values via `fmt_num(double)` while the final answer renders via `fmt_solve_result` → `fmt_exact_double`. Trace and final disagree on the same value:

```
$ ./bin/fwiz --steps "/tmp/foo.fw(height=?, weight=981)"
result: height = 98.1     # decimal in trace
height = 981 / 10         # exact in final

$ ./bin/fwiz --steps "/tmp/foo2.fw(angle=?)"
result: deg = 0.01745329252  # decimal in trace
result: angle = 0.5235987756  # decimal in trace
angle = 1 / 6 * pi            # exact in final
```

Root cause: the solver carries solved values as `std::map<std::string, double>` and the trace sites have already lost the symbolic form by the time they're called.

## Codebase audit (2026-04-26)

### Carrier-flow sites (in scope, 9)

| Site | Role |
|---|---|
| `main.cpp:253` | top-level result accumulator (`std::map<std::string, double> solved`) |
| `system.h:1648` | `resolve(...)` primary public entry |
| `system.h:1660` | `resolve_all(...)` multi-result entry |
| `system.h:1764` | `resolve_one(...)` delegates to `resolve_all` |
| `system.h:1778-1779` | `prepare_bindings(...)` injects defaults |
| `system.h:1797` | `solve_all(...)` multi-result recursion |
| `system.h:2405` | `prepare_sub_bindings(...)` cross-file formula binding |
| `system.h:3034 / 3401` | `try_resolve(...)` primary solve step |
| `system.h:3198` | `solve_recursive(...)` recursive solver |

### Trace call sites using `fmt_num` (10)

| Tag | Line | Call | Semantic |
|---|---|---|---|
| T1 | `system.h:667` | `trace.step("  default: " + k + " = " + fmt_num(v))` | File-level constant from `defaults` at load time |
| T2 | `system.h:1784` | `trace.step("  using default: " + k + " = " + fmt_num(v))` | Default injected at solve start |
| T3 | `system.h:1790` | `trace.step("    " + k + " = " + fmt_num(v))` | User-supplied given value |
| T4 | `system.h:3215` | `trace.calc("known: " + target + " = " + fmt_num(it->second))` | Cache hit — already-solved variable |
| T5 | `system.h:3261` | `trace.calc("  binding: " + sv + " = " + fmt_num(val))` | Cross-file formula argument |
| T6 | `system.h:3276-3277` | `trace.step("  @extern …(" + fmt_num(arg) + ") = " + fmt_num(result))` | C++ extern function call arg + return |
| T7 | `system.h:3287` | `trace.step("  result: " + target + " = " + fmt_num(result))` | Cross-file formula result |
| T8 | `system.h:3422` | `trace.calc("substitute " + v + " = " + fmt_num(it->second))` | Substituting known binding |
| T9 | `system.h:3435` | `trace.calc("substitute " + v + " = " + fmt_num(val))` | Substituting freshly-resolved dependency |
| T10 | `system.h:3475` | `trace.step("result: " + target + " = " + fmt_num(result))` | Final solved value for this candidate |

### Where symbolic form still exists at trace time

- **T10 is the primary recovery site.** At line 3475, `ExprPtr simplified` (defined at line 3448) is in scope — it's the simplified symbolic tree before numeric evaluation. Recording `simplified` into a parallel symbolic map at this point is zero extra work.
- **T4, T8, T9** can read from a parallel symbolic map populated at T10 — they're downstream reads of values written there.
- **T1, T2, T3** (defaults, given): data loss is upstream (parser / CLI). The existing `build_alias_table()` mechanism handles their final-output rendering.
- **T5, T7** (cross-file formula): data loss is inside `prepare_sub_bindings` and `sub_sys.resolve()`. Requires either alias-table lookup or a `resolve_symbolic()` overload.
- **T6** (`@extern`): result is a C++-computed double. Alias-table lookup at format time covers recognizable cases.

### Sites where `double` arithmetic must stay (11)

Not to be touched — these are correctness-critical numeric computations:
- `substitute(..., Expr::Num(it->second))` at lines 3423 and 3436
- `evaluate(simplified)` at line 3452
- `std::isinf` guard at line 3469
- Cross-validation loop in `resolve_all` lines 1684-1706
- `numeric_results_` exact-check line 1710
- Condition-check numeric projection in `derive_recursive` line 2804
- `evaluate(*simplify(resolved))` in `prepare_sub_bindings` line 2437
- `@extern` C++ call line 3274
- Numeric solver block in `try_resolve_numeric`
- Cache key builder in `resolve_memoized` line 2982

### `fmt_solve_result` reuse

Current signature (`main.cpp:11-13`):

```cpp
static std::string fmt_solve_result(double v, bool try_exact,
        const std::map<std::string, double>& aliases = {}) {
    return try_exact ? fmt_exact_double(v, aliases) : fmt_num(v);
}
```

Replacing `fmt_num(v)` at trace sites T2-T10 with `fmt_exact_double(v, aliases)` covers both reproducer cases via the existing heuristic recognizer with zero new abstraction. The aliases map must reach trace sites via a new `FormulaSystem::display_aliases_` field set once from `main.cpp` after `build_alias_table()`.

### Test coverage

Zero existing tests assert on numeric value strings inside `--steps`/`--calc` output. The 3 test groups that touch trace flags check presence/absence of "solving" keywords or exit codes. **Test breakage from the fix: zero.**

### Performance

`--steps` is O(n_trace_lines × constant_table_size). Even at 1000 trace lines, `expr_recognize_constants` costs microseconds. Negligible.

### `ExprArena::Scope` dependency

`fmt_exact_double` requires an active scope. `resolve()` opens one at `system.h:1649` — all T2-T10 trace calls are within that scope. T1 (`trace_loaded`) is called outside `resolve()` and would need its own scope (cheap) or a fallback to `fmt_num` for the load-time message.

## Prior art (CAS architecture)

All four major CAS — Mathematica, SymPy, Maxima, Maple — use the same fundamental design: **numeric values are leaf nodes in the symbolic tree.** There is no separate "numeric layer." A `Rational(1, 3)` and a `Float(0.333)` are structurally identical positions in the tree; only the type tag differs. Floats are produced **only** by explicit user-facing operations (`evalf()`, `N[]`, `float()`, `numer:true`).

Why "float-in-loop, recover-at-boundary" doesn't work: PSLQ-style integer-relation algorithms need ~50+ digits of precision to recover exact symbolic forms reliably (Odrzywolek 2021, arXiv:2002.12690). IEEE double's 15-16 digits is insufficient. So mainstream CAS keep the symbolic form alive throughout and project to double on demand at the user-facing boundary.

### Two architectural patterns for trace output

1. **Evaluator-level hook** (Mathematica `Trace[]`): wraps every intermediate in `HoldForm`, records the symbolic side-effect during evaluation. Works automatically because every intermediate is already a symbolic `Expr`.
2. **Separate recording algorithm** (SymPy `manualintegrate`): explicit `Rule` tree built during a hand-written tracing variant of the algorithm. The general `solve()` has no step output — would require reimplementing the solver. SymPy issue #6293 has been open since 2014.

**Fwiz's trace sites are already the second pattern** — they're explicit `trace.step()` / `trace.calc()` calls inside the solver. The architectural gap is narrow: those calls receive `double`, not `ExprPtr`. The fix is much smaller for Fwiz than for SymPy because Fwiz's solver already has the trace-recording calls; only the data passed to them needs to change.

### Exact-vs-numeric mode boundary

| System | Per-call boundary | Per-expression | Global flag |
|---|---|---|---|
| SymPy | `.evalf()`, `N()` | float literal | (none) |
| Mathematica | `N[expr]`, `N[expr,n]` | decimal literal | (none) |
| Maxima | `float()`, `bfloat()` | decimal literal | `numer:true` |
| Maple | `evalf()` | decimal literal | (none) |
| **Fwiz** | (none) | (none) | **`--approximate`** |

Fwiz's `--approximate` flag maps exactly to Maxima's `numer:true` — the coarsest boundary, appropriate for CLI tools where mode is selected once per invocation. The new fix must respect `--approximate`: in that mode, traces should continue to use `fmt_num`, not the exact renderer. This is naturally handled by routing through `fmt_solve_result(v, try_exact, ...)` instead of `fmt_num(v)` directly.

### Common CAS failure modes (and how mainstream tools mitigate)

1. **Expression swell** — intermediate forms grow exponentially. Mitigated by always-on simplification (Mathematica), hash-consing / DAG sharing (SymPy via Python interning), CSE extraction.
2. **Undecidable equality** — `a == b` symbolic equality is undecidable in general (Richardson's theorem). SymPy uses three-valued logic (`True` / `False` / `None`); refuses to simplify when uncertain. Fwiz's `iff` + global conditions is the equivalent mechanism.
3. **Recognition limits** — the float recognizer table has a finite horizon. `fmt_exact_double`'s `RECOGNIZE_FRACTION_MAX_DEN = 360` is correct for final answers (one arithmetic step from source) but fundamentally cannot recover compound or user-defined constants beyond the table. Mid-pipeline recovery from double is architecturally unsound (Odrzywolek 2021's `1/sigma` formula-candidate bound).

## Two paths

### Path A — Minimal: route trace sites through `fmt_exact_double`

- ~30 LOC; ~10 trace sites + new `FormulaSystem::display_aliases_` field.
- Both reproducers fixed via existing heuristic recognizer.
- Limit: anything outside `RECOGNIZE_FRACTION_MAX_DEN` or the named-constant table still falls back to `fmt_num`. Trace and final still disagree on those, but the same way they disagree today.
- Zero test breakage; negligible perf cost.

### Path B — Structural: parallel `std::map<std::string, ExprPtr>`

- ~50-70 LOC; ~9 function signatures updated; 4 call sites in `main.cpp`.
- Invariant: every `bindings[k] = v` write at T10 paired with `solved_symbolic[k] = simplified`. Display-only — arithmetic still reads from `bindings`.
- Trace and final use the same data, so they cannot disagree by construction. No recognizer-horizon limit.
- This is what every mainstream CAS does. Aligned with what Future #10a (extending `evaluate_symbolic` for new number types) and Future #14 (matrices, complex) will eventually require anyway.

## Open design questions

1. **Pre-eval vs post-recognize symbolic form.** Should `solved_symbolic` carry the `simplified` ExprPtr from T10 (true provenance) or the user-recognized form (after `expr_recognize_constants`)? Display layer wants post-recognize (`pi/6` not `0.5235987756`); architecture wants pre-eval. May need both, or apply recognizer at print time.
2. **Cross-file formula calls (T5, T7) and `@extern` (T6).** Symbolic form crosses a sub-system boundary or a C++ function. Decide whether sub-system traces also get the structural treatment or settle for alias-table lookup.
3. **`ExprArena::Scope` lifetime at T1.** Either open a scope at T1 (cheap) or skip T1 in the fix (it's a load-time message, less critical).
4. **`--approximate` interaction.** Today `fmt_solve_result` checks `try_exact`; the new code must thread the same flag.

## Sources

External CAS prior art:

- [SymPy Core docs](https://docs.sympy.org/latest/modules/core.html)
- [SymPy evalf docs](https://docs.sympy.org/latest/modules/evalf.html)
- [SymPy Assumptions guide](https://docs.sympy.org/latest/guides/assumptions.html)
- [SymPy manualintegrate source](https://github.com/sympy/sympy/blob/master/sympy/integrals/manualintegrate.py)
- [SymPy show-steps issue #6293](https://github.com/sympy/sympy/issues/6293)
- [mpmath identification docs](https://mpmath.readthedocs.io/en/stable/identification.html)
- [Wolfram Numbers Tutorial](https://reference.wolfram.com/language/tutorial/Numbers.html)
- [Wolfram Trace docs](https://reference.wolfram.com/language/ref/Trace.html)
- [Maxima numer/bfloat tutorial](https://feb.kuleuven.be/public/u0003131/WBT23/wxMaxima/wxM_intro/numer_float_bfloat.html)
- [Maxima step-by-step feature request](https://sourceforge.net/p/maxima/feature-requests/103/)
- [Hash consing paper arXiv:2509.20534](https://arxiv.org/html/2509.20534v2)
- [Odrzywolek constant recognition criteria arXiv:2002.12690](https://arxiv.org/abs/2002.12690)

Internal:

- `src/system.h` — call-site audit
- `src/main.cpp` — `fmt_solve_result` definition, `solved` map
- `src/expr.h` — `fmt_num`, `fmt_exact_double`, `expr_recognize_constants`
- `docs/Known-Issues.md` #6 — original problem statement
- `docs/Future.md` #6 — symbolic differentiation (downstream consumer of structural fix)
