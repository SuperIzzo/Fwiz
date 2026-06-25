---
name: dim-algebra-promotion-3c
description: Cycle 3c dim-algebra research — BindingType::dim blast radius (3W+1R sites), compute_dim design, BuiltinMeta.dim_propagate scope, BLOCKING/DESIRABLE split
metadata:
  type: project
---

Blast radius for `BindingType::dim` promotion (`std::string` → `std::map<std::string,int>`):

**Exactly 4 active code sites:**
- W1: `system.h:1099` — `register_dim_section` (equations loop)
- W2: `system.h:1101` — `register_dim_section` (defaults loop)
- W3: `system.h:3308` — `parse_line` `:` annotation parse (DIM_SECTION atom)
- R1: `expr.h:2032` — `check_condition` DIM_SECTION arm (`tm_it->second.dim == set_name`)

**Why:** `using DimName = std::string` at `system.h:440` is the planted cycle-3c hook. `BindingType::dim` field at `expr.h:1800` is typed `std::string` directly (not via typedef).

**compute_dim design:** MUL=add-exponent-maps, DIV=subtract, POW(Num)=scale, NEG=passthrough, ADD/SUB=must-match, Var=lookup type_map_, Num=empty. FUNC_CALL dispatches via `BuiltinMeta.dim_propagate` (new field).

**BuiltinMeta.dim_propagate:** BLOCKING for `sqrt` (halve exponents) only. DESIRABLE for trig/log/exp (enforce dimensionless) and `abs` (pass-through). Field addition is ~4 LOC; sqrt callback ~11 LOC.

**BLOCKING production LOC:** ~95 (promotion W1-W3 15 + DimMap helpers 25 + compute_dim 45 + DIM_SECTION arm R1 10). Under 150-LOC ceiling without DECOMPOSE trigger.

**#82 does NOT fire:** promotion widens an existing field's type, not a new parallel map.

**Key open question:** does #7b FULL require `is_in(compound_expr, force_name)` to match? If yes, requires named-compound-dim aliases (#81 PARKED). If #7b FULL = "ADD/SUB mismatch + compute_dim walks correctly", then #81 is not a prerequisite. Design trio must clarify scope.

**Why:** [[factorial-reverse-solve-94]] cycle established pattern of enumerating all throw sites before design; same discipline applied here — blast radius confirmed minimal, no hidden readers.

Brief at: `.fwiz-workflow/research-brief.md`
