---
name: Milestone E — try/catch vs optional error handling decision
description: Research findings for the 24 bugprone-empty-catch sites in fwiz; hybrid optional/NOLINT approach recommended
type: project
---

Milestone E investigated 24 `catch(...){}` sites flagged by clang-tidy as `bugprone-empty-catch`. All are intentional best-effort probes, semantically correct. Research delivered to `.fwiz-workflow/research-milestone-E.md`.

**Why:** `catch(...)` masks all exception types including logic errors and OOM; and the one HOT site (numeric grid-scan lambda at system.h:938) incurs ~160µs exception overhead per thrown probe — 49,540x vs the optional failure path.

**How to apply:**
- Tier 1: add `evaluate_opt() -> std::optional<double>` to owned `evaluate()` — eliminates 15+ sites
- Tier 2: add `parse_condition_opt()` wrapper — eliminates 3 cold sites
- Tier 3: `NOLINTNEXTLINE(bugprone-empty-catch)` with rationale comment for filesystem/stdlib/sub-system-loader sites
- Do NOT do a wholesale llvm::Expected refactor — too invasive, contradicts fwiz's exception-propagation model
- Do NOT use blanket `.clang-tidy` suppression — hides future accidental empty catches
