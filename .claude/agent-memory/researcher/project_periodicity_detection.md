---
name: Periodicity Detection — Future #12 external research
description: CAS representations of periodic root sets; textbook conventions; numeric period inference; symbolic vs numeric path tradeoffs
type: project
---

All mainstream CAS use a symbolic path for pure trig equations: SymPy `solveset` returns `Union(ImageSet(Lambda(_n, 2*_n*pi + base), Integers), ...)`, Mathematica `Reduce` returns a logical `Or`/`And` formula with `C[1] ∈ Integers`, Maxima `to_poly_solve` returns `%union([x=2*%pi*%z16 + %pi/6], [x=2*%pi*%z18 + 5*%pi/6])`. All produce both branches; none use numeric post-processing for periodicity.

**Why:** Symbolic path gives exact output (`π/6 + 2kπ`) vs. numeric floats; all CAS were designed with symbolic primacy.

**How to apply:** The design phase should decide between (a) new symbolic trig-recognition strategy and (b) numeric post-process on the sorted roots array. Research brief covers both neutrally. Key implementation observations from Researcher A are also in the brief: the two alternating gaps in the sorted root array encode both families and are machine-detectable O(n).

Brief location: `/run/media/data/users/izzo/Projects/C++/Fwiz/.fwiz-workflow/research-brief.md` (external sections start at line ~233).
