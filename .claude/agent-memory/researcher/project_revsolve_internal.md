---
name: revsolve-internal
description: Internal substrate map for numeric reverse-solve / design inversion (F1 root-find, F2 monotonic threshold); multi-variable gate is the root blocker for both; brief at .fwiz-workflow/research-brief-revsolve-internal.md
metadata:
  type: project
---

F1 (K=?) and F2 (r=?max) both fail at the multi-variable gate (system.h:3401–3411) — not at the numeric solver. The NUMERIC candidate for K is `P - K - W` (residual var P not in user bindings); for r it is `can_pay - sum(…, range(r,h)…)` (residual var P same reason). Gate checks `cvars.empty()` after erasing target+bindings — P survives → skip.

**Why:** P = K + W is a derived var, not user-supplied. The gate does not attempt to resolve derived residual vars before deciding to skip.

**What would fix F1:** Either (A) pre-resolve derived vars before the gate, or (B) compose two-equation NUMERIC candidates at emit time (Strategy-7 analog for the numeric layer). Option B is more surgical.

**Aggregation unroll:** NOT the blocker. At load time `extract_range_values(range(r,h))` fails because r is symbolic — aggregation stays unevaluated in the AST. But inside `try_resolve_numeric`, after substituting all bindings, `simplify()` would fire the unroll. The issue is the gate never lets `try_resolve_numeric` be called.

**F2 substrate:**
- `?max`/`?min` modifier: new parse branch in parse_cli_query (system.h:5936); ~15 LOC + CLIQueryVar field extension.
- Comparator constraint CLI arg (`can_pay>=0.40`): NOT supported; `arg.find('=')` hits `>=` and produces garbage binding. New parse branch needed (~20 LOC).
- Sweep infrastructure: `--table`'s emit_row + range_bindings reusable for sweep-and-threshold.
- Domain inference from `range(r,h)` structure: NOT supported; minimal F2 path requires user to pass explicit range (`r=[1..5]`).
- F2 result is exact integer (= not ~); no new contract needed.

**Key anchors:** multi-variable gate=system.h:3401–3411; Strategy 6 emit=4616–4642; try_resolve_numeric=4979–5150; find_numeric_roots=expr.h:4629; extract_range_values=expr.h:2847; parse_cli_query=system.h:5703; CLIQueryVar=system.h:5554; emit_row=main.cpp:330.

**Why:** F1 and F2 are separable. F2 minimal path (explicit range + comparator constraint + ?max projection) reuses almost all existing infrastructure. Brief at .fwiz-workflow/research-brief-revsolve-internal.md.

**How to apply:** Design phase should target the multi-variable gate for F1 (not the aggregation unroll — that's already handled). For F2, clarify whether domain inference is required or user-supplied range is acceptable for the first iteration.
