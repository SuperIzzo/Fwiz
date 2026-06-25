---
name: collections-aggregation-external
description: External prior-art research for gen-6 continuation arc — map/fold/reduce primitives + {} collections + six-reducer stdlib defs + equivalence proof strategy
metadata:
  type: project
---

Gen-6 continuation arc: "Collections & first-class aggregation". Brief at `.fwiz-workflow/research-brief-collections-aggregation.md`.

**Key findings:**

1. **Fold is theoretically sufficient** (map, count, sum, product, min, max all expressible as folds) but EVERY CAS/language provides map AND fold as independent user-facing primitives because users think in terms of both independently. Theoretical minimalism ≠ practical design.

2. **Identity element table (universal across J/Haskell/NumPy/APL):**
   - sum → 0, product → 1, count → 0, max → first element (or -inf), min → first element (or +inf)
   - mean → NO identity (two-pass: sum/count)
   - max/min on empty domain: universally special (error, ±inf, or unevaluated). fwiz's `nullptr` (unevaluated) is the most honest approach.

3. **Materialized vs fused:** Fused/stream model (Clojure transducers, Haskell stream fusion, Rust iterators) avoids intermediate allocation but treats the intermediate as an implementation detail — NOT a first-class user value. Since the arc requires `{}` collections as first-class values, materialized is the correct default. C++ fast-paths are fwiz's native "fusion."

4. **Prior-art alignment:** Coexist model IS the CAS standard. No CAS removes named reducers in favor of stdlib fold definitions. SymPy/Mathematica/Maxima all have both. Named reducers stay because: known identities, intent communication, performance, reverse-solvability.

5. **Equivalence proof strategy:** `fingerprint_expr` (Schwartz-Zippel evaluator already in system.h) is the symbolic-equality oracle. Three test layers: (a) named edge cases (empty domain × all 6 reducers, exact-rational mean, max/min unevaluated), (b) differential sweep (domain × body × reducer), (c) symbolic fingerprinting for free-variable bodies. No new infrastructure needed.

6. **AC8 → tested invariant:** Mathematica's equivalence of `Total[list] == Fold[Plus, 0, list]` is documented but never tested. fwiz goes beyond all CAS by making this a regression guard.

**Why:** [[collections-aggregation-arc]]
