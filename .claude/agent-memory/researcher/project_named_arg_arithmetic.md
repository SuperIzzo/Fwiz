---
name: Named-arg in arithmetic Future #91
description: Parser surface research for func(arg=value) inside arithmetic expressions — Option A token pre-pass in parse_line; ~50-80 LOC; positional form is a distinct bug
type: project
---

Named-arg in arithmetic (Future #91) — parser surface research, cycle 3i 2026-05-17.

**Key findings:**
- Parse error fires at parser.h:163 (`expect(RPAREN)`) when `=` appears inside a FUNC_CALL arg list without `?`
- Error is SILENTLY swallowed by load_lines (system.h:568-579) — no user-visible diagnostic
- `extract_formula_calls` gates on `has_question_in_range` — no `?`, no extraction
- `parse_call_args` requires `query_var` — throws "no query variable" without `?`
- Fix window: between `extract_formula_calls` call (system.h:3087) and Parser construction (system.h:3256) in `parse_line`

**Option A (recommended):** new NON-STATIC member function in system.h (~50-70 LOC). Scans tokens for `IDENT LPAREN ... EQUALS(depth=1) ... RPAREN` without `?`, loads sub-system for return_var, builds FormulaCall, replaces token range with `IDENT(output_var)`. Parser.h untouched.

**Separate bug found:** positional form `fibonacci(n-1) + fibonacci(n-2)` in section body parses but fails at solve time (NaN). `resolve_positional_calls()` is never called for FUNCTION_SECTION subs built by `register_function_section` (uses `load_lines`, not `load_with_sections`). Fix: call `resolve_positional_calls()` after `load_lines` in `register_function_section`. ~2 LOC but separate from #91.

**No existing tests** assert the REJECT behavior — implementer can add passing tests without removing anything.

**Why:** brief at .fwiz-workflow/research-brief.md (overwritten from cycle 3h brief)
**How to apply:** planner designs Option A; implementer adds member function + parse_line call site + tests
