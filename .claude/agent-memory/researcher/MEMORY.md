# Researcher Agent Memory

- [Simultaneous equations research](project_simultaneous_equations.md) — Deep dive into current multi-equation architecture and where substitution/elimination plugs in
- [Milestone E — try/catch vs optional](project_milestone_e_error_handling.md) — 24 bugprone-empty-catch sites; hybrid evaluate_opt/NOLINT approach recommended
- [Symbolic Differentiation Future #6](project_symbolic_diff.md) — symbolic_diff as free fn in expr.h; CLI dC/da syntax invalid; all simplifier patterns probed; ln=log; 9-entry derivative table needed
- [Periodicity Detection Future #12](project_periodicity_detection.md) — CAS all use symbolic ImageSet/Reduce/union; textbook: sin→2 families, tan→1; numeric gap-GCD path also viable; brief at .fwiz-workflow/research-brief.md
- [Nested Formula Calls Future #21](project_nested_calls.md) — Path A (synthetic-alias side-channel) recommended; all infrastructure exists; defer dotted form to #15; brief at .fwiz-workflow/research-brief-21-nested-calls.md
- [Complex/Matrix/Struct CAS external research](project_complex_matrix_struct_external.md) — SymPy/Maxima/Mathematica/Maple/SageMath design patterns for #13/#14/#15; brief at .fwiz-workflow/research-brief-external.md
- [Symbolic Integration CAS research](project_symbolic_integration.md) — 5-CAS survey for integral(f,x) arc; tiered ladder, derivative-divides, IBP/LIATE, no Risch; brief at .fwiz-workflow/research-brief-integration.md
- [Typed-binding predicates Future #53](project_typed_binding_predicates.md) — 4 consumers audited; T3.6 STRAIGHTFORWARD (is_neg_num); T3.5/#31/integration PARTIAL; Approach C recommended; brief at .fwiz-workflow/research-brief.md
