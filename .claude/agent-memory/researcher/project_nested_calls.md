---
name: Nested Formula Calls Future #21
description: Research findings for composable/nested CLI formula calls — Path A recommended, infrastructure exists
type: project
---

Path A (synthetic-alias side-channel, nested form only) is the correct implementation path. Extend `parse_cli_query` (system.h:3572) to detect and extract nested `name(... =? ...)` arg expressions into `FormulaCall` objects, inject them into the loaded system's `formula_calls` before dispatch.

**Why:** All load-bearing infrastructure already exists and is tested: `extract_formula_calls` (system.h:2166), `parse_call_args` (system.h:2107), `prepare_sub_bindings` (system.h:2501), `load_sub_system` (system.h:2419). Path A avoids touching `expr.h`'s ExprType and all ~25 switch sites.

**How to apply:** Defer dotted flat form (`triangle.A=?sin.x`) to Future #15 (Dot Access) cycle — both share a path-qualified-variable grammar that should be designed together. Multi-solution threading not needed; first-successful policy is correct for LLM-determinism. Estimate ~100 net LOC in `system.h` + `main.cpp` only.

Full brief at: `.fwiz-workflow/research-brief-21-nested-calls.md`
