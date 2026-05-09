---
name: Complex/Matrix/Struct CAS external research
description: How SymPy, Maxima, Mathematica, Maple, SageMath represent complex numbers, matrices, and structs in their expression trees — informs Fwiz Future #13 (complex) and #14 (matrix)
type: project
---

Key findings from the 2026-05-09 external CAS survey for the evaluate_symbolic extension arc.

**Why:** Planning a multi-cycle arc to extend Fwiz's `evaluate_symbolic` for complex (#13), matrix (#14), and struct (#15) types.

**How to apply:** Use as the prior-art foundation for the planner when designing Complex and Matrix ExprType extensions.

## Complex numbers

- SymPy: `I` is a singleton atom (`ImaginaryUnit`), NOT a compound node. `3+2*I` is compound `Add(Integer(3), Mul(Integer(2), ImaginaryUnit()))`. No dedicated Complex tree node for symbolic expressions. `i^2 = -1` via `_eval_power` class hook, not a rewrite rule.
- Maxima: `%i` is a named constant atom. No native complex type. Arithmetic via simplifier hard-codes. `rectform`/`polarform` are lazy transforms, not eager.
- Mathematica: Two-level — compound `Plus[3, Times[2, I]]` for symbolic; atomic `Complex[re, im]` leaf (AtomQ=True) when all parts are numeric. Best dual-representation model for CAS.
- Maple: `COMPLEX` DAG node in the kernel for numeric values; compound `a+b*I` tree for symbolic. Mirrors Mathematica's two-level approach.

## Matrices

- SymPy: Two hierarchies — `MatrixExpr` (abstract, symbolic, tree node; `MatMul` inherits `MatrixExpr`+`Mul`) vs `MutableDenseMatrix` (concrete). `ShapeError` on mismatch at construction time.
- Maxima: `((MATRIX) rows...)` list-of-lists. Error on shape mismatch.
- Mathematica: Nested `List` (no matrix type). `Dot[A,B]` validates shape; error on mismatch. `SparseArray` as optional alternative.
- Maple: Typed `Matrix` object with shape metadata. Error on mismatch.
- All CAS raise errors on shape mismatch — not `undefined` propagation. Fwiz's planned `undefined` for mismatch is a deliberate design divergence.

## Structs/Records

- No CAS has algebraic dot-path namespacing (`car.velocity.x`) as expression-tree prior art.
- Mathematica `Association[key->val]` is structured data container for expressions, NOT algebraic.
- Universal approach: flat naming convention or structured containers holding expressions.
- Future #15 "syntactic sugar over flattened names" matches all prior art. Do not over-engineer.

## Cross-cutting

- Branch cuts: ALL CAS use principal-value convention (`log(-1) = i*pi`, branch cut on negative real axis).
- evaluate() widening: CAS widen silently to complex. Fwiz throws from `Checked<double>` — intentional divergence, correct for Fwiz's real-valued solver contract.
- Hot-path threshold: rewrite-rule approach is fine for symbolic-dominant, sparse-complex use cases. Atomic COMPLEX leaf is the optimization for numerically-dense complex arithmetic — defer until profiled.

Brief at: `.fwiz-workflow/research-brief-external.md`
