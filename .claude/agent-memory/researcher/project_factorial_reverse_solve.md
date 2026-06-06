---
name: Factorial reverse-solve Future #94
description: Trap diagnosis and Fix (d) recommendation for is_in(6,factorial)=false — exists_for_function_section uses resolve instead of resolve_all; depth re-throw bypasses Strategy 6
metadata:
  type: project
---

Factorial-style reverse-solve (Future #94) — cycle 3j research, 2026-06-06.

The failure: `is_in(6, factorial)` returns false. `exists_for_function_section` calls `sub.resolve("n", {result:6})`. Strategy 2 emits `n = result/prev`, which chains into factorial(n=n-1), saturates `formula_depth_` (1000), and the depth exception is RE-THROWN at system.h:4457 — bypassing Strategy 6 (NUMERIC) entirely.

**Fix (d) recommended** (5 LOC, in `exists_for_function_section` only): change `sub.resolve(sd.parameter, sub_binds)` to `sub.resolve_all(sd.parameter, sub_binds)` and return `!vset.discrete().empty() || vset.covers_reals()`. The `resolve_all` path handles depth blowout via `return false` at line 2671 (not re-throw). This is proven by tests.cpp:7276-7296 where `resolve_all("n", {result:120})` already finds factorial inverses correctly.

**Backup Fix (b)** (2 LOC at lines 4457 and 4579): change depth re-throw to only re-throw when `depth > 0`. Conceptually impure but correct.

**Open questions:**
1. Does `resolve_all`'s EXPR candidates (via `try_derive`) also re-throw depth exceptions? If so, Fix (b) also needed.
2. Does `BudgetGuard` nesting reset budget when `resolve_all` is called from `check_condition`?

**Test shapes recommended:**
- Shape 1: factorial (BLOCKING) — `is_in(6, factorial)` = true
- Shape 2: triangular numbers (DESIRABLE) — `is_in(10, triangular)` = true

Extend `test_gen5_cycle3g_recursive_function_sections` (tests.cpp:16713). Add after D3 sentinel at line 16971.

**Why:** brief at .fwiz-workflow/research-brief.md (overwritten from cycle 3i brief)
**How to apply:** planner designs Fix (d); implementer changes exists_for_function_section + adds test shapes 1 and 2
