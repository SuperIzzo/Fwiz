# Rejected: Out-of-Vision Ideas

Case-law sidecar to `docs/Future.md`. Items land here when a visionary audit
determines they fall outside Fwiz's vision (universal math inference engine;
tiny fast core; tools wrap around the engine, not inside it).

> **Numbering matches `Future.md` and `COMPLETED.md`.** A number that ends up
> here is retired from active planning unless explicitly reopened. If a reopen
> trigger fires (vision shifts, a new dependent surfaces), move the entry back
> to `Future.md` under the appropriate tier.

## Entry format

Each rejection records the original proposal, the vision principle violated,
the date, and (optionally) a concrete reopen condition.

    ## #N. Title (rejected YYYY-MM-DD)

    **Original proposal:** short summary, or a brief paste from Future.md.

    **Vision principle violated:** which principle (cite visionary.md or CLAUDE.md).

    **Rationale:** why this falls outside Fwiz's vision.

    **Reopen trigger (optional):** the concrete condition that would change the verdict.

## Entries

## #77. `_` as multiplication separator for reserved-prefix identifiers (rejected 2026-05-14)

**Original proposal:** Reserve `_` as a prefix-multiplication separator for specific reserved single-character builtins: `i_km → i * km`, `e_X → e * X`, possibly `pi_X` etc. The lexer or parser would split the IDENT at the first `_` if the prefix is a reserved single-char. Motivating use case: imaginary-unit-prefixed quantities common in complex/AC-circuit math where the user can't easily type the `*` (`ikm` parses as one IDENT due to greedy `read_ident`).

**Vision principle violated:** "Remove > Add." Per CLAUDE.md core principles: "a general pattern replacing two specializations beats adding a third." The `_` separator adds a specialized prefix-multiplication syntax where the general mechanism (`*` operator + `:` annotation) already covers it.

**Rationale:**
The hybrid dim model decided in gen-3 cycle 1 (Future #78 DONE-by-design) covers both motivating use cases without requiring a new separator:
- **Expression position**: `i * km` works directly. After cycle 2 of the gen-3 arc lands, dim propagation gives `dim(i * km) = dim(i) × dim(km) = dimensionless × length = length`. The complex nature is preserved via `i`'s NaN binding and the `i^2 = -1` rewrite rule.
- **Binding-level annotation**: `c:(complex, length) = i * km` uses the intersection annotation grammar. Declares `c` as both complex-typed AND length-dimensioned via two AND-connected predicate clauses.

Adding the `_` separator would:
- Conflict with the established `<thing>_<thing>` convention for user-defined unit names (`km_per_hr = km / hr` — the Units cycle 1 stdlib pattern). Disambiguation requires "rule only fires for `e_` at start of expression" which complicates the lexer.
- Be asymmetric: `i_km` works but `i.km` doesn't.
- Risk scope creep: once `i_` and `e_` are reserved, does the rule extend to `pi_`, `phi_`, user-defined `mu0` (vacuum permeability)?
- Risk backward-compat breakage: existing `.fw` files may contain `i_*` identifiers (user-defined) — silent collision.

The user's original framing acknowledged the uncertainty: *"I'm not saying it's correct btw, it just sort of makes sense — we should have a proper plan-critic-visionary cycle for that."* The gen-3 cycle 1 verdict is that the hybrid model's general `*`/`:` mechanism makes the separator unnecessary.

**Reopen trigger (optional):** Enough user reports of complex-number arithmetic friction accumulate to push priority back up AFTER the hybrid surface (gen-3 cycle 2) ships and demonstrates the friction remains in practice. Concrete signal: 3+ separate user reports of `i_km`-style friction, OR an LLM benchmark surfaces it as a recurring failure mode.
