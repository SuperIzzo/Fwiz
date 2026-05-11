---
name: Batch/Table Mode Future #5
description: Research findings for --table flag: CLI parse path, range AST options, CAS survey, Option C recommended
type: project
---

Option C (CLI-only range construct) recommended — no Lexer/Parser/expr.h changes.

Key findings:
- `..` token does not exist in lexer.h; `[1..10]` throws at lex time today.
- Detection: `val.starts_with('[') && val.find("..") != npos` in parse_cli_query BEFORE Lexer is called.
- `CLIQuery::range_bindings` (map<string, vector<double>>) — parallel to bindings.
- Table driver in main.cpp only; FormulaSystem unchanged.
- Vec-literal disambiguation: `..` present → range; absent → vec literal (pre-check before Lexer).
- Count-based generation (`start + i * step`) preferred over repeated addition to avoid IEEE 754 drift.
- `--output FILE` reusable for table TSV output (currently only used in --fit mode).
- 15 open questions for design to decide (unsolvable row format, endpoint inclusion, zip mismatch, header columns, etc.).

**Why:** Ranges are iteration drivers, not mathematical objects. CLI layer is the right home. Matches "tiny fast core" principle.
**How to apply:** Brief at .fwiz-workflow/research-brief.md.
