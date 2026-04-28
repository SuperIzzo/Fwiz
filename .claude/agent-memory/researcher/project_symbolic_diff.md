---
name: Symbolic Differentiation (Future #6) research findings
description: Key architectural decisions and empirical findings from the internal codebase audit for Future #6
type: project
---

Key architectural decisions confirmed by codebase audit + probe runs (2026-04-26):

**Fact:** `symbolic_diff(ExprPtr, string) -> ExprPtr` should be a free function in `expr.h`, analogous to `solve_for`. No new ExprType node needed — Future.md #6 explicitly says "the derivative is itself an ExprPtr that goes through the simplifier."

**Why:** Adding `ExprType::DIFF` would require updating 6 dispatch switch sites and breaks the functional-construction pattern. Option B (builtin registry) doesn't fit — `builtin_functions()` maps to `double(*)(double)`, not ExprPtr.

**Fact:** The simplifier handles ALL standard differentiation output patterns correctly. Probed and verified: `2*x^1*1 → 2*x`, `(2*x^1*1) + (3*1) + 0 → 2*x+3`, `sin(x) + x*cos(x)` (product rule), `(-2)/(x-1)^2` (quotient rule), `6*x` (second derivative), `cos(sin(x))*cos(x)` (chain rule). Rewrite rules MUST be active (`RewriteRulesGuard`) for `(x^a)^b = x^(a*b)` to fire — but `derive_all` always sets this up.

**Fact:** `dC/da=?` CLI syntax DOES NOT parse as a derivative query. `parse_cli_query` reads `name = "dC/da"` (contains `/`), which is not a valid variable. Use `diff(C,a)=?` or `--diff` flag instead.

**Fact:** Rewrite rules are thread-local and only active when `RewriteRulesGuard` is in scope (set up inside `derive_all` and `resolve_all`). Standalone `simplify()` calls in `symbolic_diff` without the guard will miss rules like `(x^a)^b = x^(a*b)`. The guard IS active wherever `symbolic_diff` will be called in practice.

**Fact:** `ln` is not a builtin. The natural log builtin is `log`. `d/dx(c^x) = c^x * log(c)`.

**Fact:** Per-function derivative table is independent of the inverse-equations in `builtin_function_defs()`. 9 entries needed: sin, cos, tan, asin, acos, atan, sqrt, log, abs (abs uses `f/abs(f)` form).

**How to apply:** When researching any follow-on symbolic diff cycle, start from these confirmed findings rather than re-probing basic simplifier behavior.
