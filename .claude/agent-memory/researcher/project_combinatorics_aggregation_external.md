---
name: combinatorics-aggregation-external
description: External CAS research for PNF arc — bounded aggregation, bignum, combinatorics math, invertibility thesis. All freshly measured for N=54 deck.
metadata:
  type: project
---

PNF arc external research: no CAS provides algebraic reverse-solve for a symbolic Sum bound; eager fold on concrete ranges IS the inversion mechanism.

**Why:** PNF needs sum/product over integer ranges to express nCr, hypergeometric PMF, EV, order stats as `.fw` formulas that fwiz can then invert. The question was whether special Sum-inverse logic is needed — it is not.

**How to apply:** When designing P0a (sum/product FUNC_CALL), the key design decision is: eager fold in `evaluate()` when lo/hi are concrete; return empty (unevaluated) when symbolic. No Gosper/Zeilberger needed. This matches how all major CAS actually achieve invertibility — they reduce Sum to a closed form, THEN solve the closed form. fwiz does the same via eager fold.

Key findings (all empirically verified):
- **All PNF binomial coefficients fit double exactly (< 2^53)**: C(54,27) = 1.95e15 < 2^53 = 9.0e15. No bignum needed for PNF.
- **GCD-interleaved nCr (Option A, ~15 LOC)**: sufficient for N ≤ 66, max intermediate stays within int64.
- **E[X_(k:n)] = k*(N+1)/(n+1)**: exact closed form for discrete uniform {1..N}, not an approximation. Verified computationally.
- **P(>=2 clubs in 5 of 54) = 0.3467505241**: reproduced exactly from hypergeom PMF formula.
- **C(66,33) fits int64 (63 bits); C(67,33) does not (64 bits)**: int64 threshold confirmed.

[[combinatorics-aggregation]] (internal counterpart)

Brief at `.fwiz-workflow/research-brief-external.md`
