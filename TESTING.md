# Testing Methodology

A practical guide to writing rigorous tests, based on the process used to test fwiz from 0 to 1700+ tests and 10+ bug fixes across the codebase.

---

## The Process

Testing is not a single pass. It's a series of increasingly adversarial rounds, each targeting a different class of failure. The sequence matters — early rounds build the safety net that lets later rounds push harder.

### Round 1: Functional Tests

Write tests for what the code is supposed to do. Cover every public function, every code path, every feature. These are the tests you'd write on day one.

**Method:** For each function, ask "what are the inputs, what should the output be?" Write the obvious cases first, then add variety.

**What this catches:** Basic logic errors, wrong formulas, off-by-one errors, missing features.

**What this misses:** Everything that "works" but shouldn't.

### Round 2: Edge Cases

For every input type, ask: what are the boundaries?

**Systematic boundary questions:**
- What's the smallest valid input? The largest?
- What happens at zero? At one? At negative one?
- What happens with empty input? With a single element?
- What if two things that should be different are the same?
- What if something that should be present is missing?
- What if something appears twice?

**For numbers:** 0, -0, 1, -1, MAX, MIN, inf, NaN, very small (subnormal), very large
**For strings:** empty, single char, very long, whitespace only
**For collections:** empty, single element, duplicates, very large
**For trees/graphs:** leaf, single node, very deep, very wide, cycles

**What this catches:** Boundary errors, missing null checks, overflow, underflow.

### Round 3: Garbage Input

Feed the code input it was never designed to handle. This is where robustness lives.

**Method:** Before writing tests, probe the code with garbage and observe what actually happens. Don't guess — run it.

```
probe("backslash", "x \\ y");     // what does the lexer do?
probe("binary junk", "\x01\x02"); // what does the file parser do?
probe("inf value", "f(x=?, y=inf)"); // what does the CLI parser do?
```

**Categories of garbage:**
- Wrong types (number where string expected, string where number expected)
- Special characters (unicode, null bytes, control characters, emoji)
- Malformed structure (missing delimiters, extra delimiters, wrong order)
- Hostile input (extremely long strings, deeply nested structures)
- Platform variations (different line endings, different encodings, BOM)

**The key insight:** When garbage causes a crash or wrong answer, fix the code first, THEN write the test. The test locks in the fix. When garbage is handled gracefully, the test documents that it stays graceful.

**What this catches:** Crashes on unexpected input, missing error handling, security issues. In our case this found: file parser crashing on bad lines, directory accepted as file, BOM eating first line, CLI giving opaque "stod" errors.

### Round 4: Adversarial Properties

Think about what invariants the system should maintain, then try to violate them.

**Invariants to test:**
- **Roundtrip consistency:** If you transform data and reverse the transformation, do you get back to the start? (parse→print→reparse, forward solve→inverse solve)
- **Idempotency:** Does applying an operation twice give the same result as once? (simplify should reach a fixpoint)
- **Independence:** Do unrelated operations interfere? (solving for X shouldn't affect solving for Y)
- **Determinism:** Same input, same output, every time?
- **Ordering independence:** Does the order of unrelated inputs matter? (it shouldn't, unless documented)

**What this catches:** Subtle semantic bugs. In our case: `-a^2` parsed with wrong precedence (found via exhaustive operator precedence testing), and NaN silently accepted as valid answer (found via roundtrip consistency).

### Round 5: State and Mutation

Test what happens between calls, not just within a single call.

**Questions:**
- Does calling function A affect the results of function B?
- Do internal caches or state leak between invocations?
- Does the caller's data get mutated when it shouldn't?
- Does calling the same function twice give the same result?
- Can you reuse an object safely after an error?

**Method:** Make multiple calls with different inputs and verify each result is independent of the others.

**What this catches:** Stale state, cache corruption, reference leaks, accumulating errors.

### Round 6: Numeric Precision

If the code does any arithmetic, systematically attack the floating point edge cases.

**Specific traps:**
- `0.1 + 0.2 - 0.3 != 0` — near-zero results from floating point
- Overflow to infinity
- Operations producing NaN (sqrt of negative, log of negative, 0/0)
- Loss of significance in subtraction of nearly-equal values
- Casting doubles to integers (UB if out of range)
- Negative zero vs positive zero

**What this catches:** In our case: near-zero coefficient from float imprecision producing wildly wrong answers (5.5e-17 treated as valid coefficient), and `(long long)v` evaluated before range check (UB for inf/NaN/large doubles).

### Round 7: Scale and Depth

Push the system to its resource limits.

**Dimensions to scale:**
- Recursion depth (stack overflow?)
- Input size (memory? performance?)
- Number of iterations (convergence? infinite loops?)

**Method:** Start at 10, 100, 1000, 10000 and observe where things break. The exact limit matters less than knowing it exists and behaving gracefully at it.

**What this catches:** Stack overflow, performance cliffs, non-termination.

### Round 8: Error Message Quality

Every error path should produce a message that tells the user what went wrong and what to do about it.

**Method:** Trigger every error path and read the message. Ask: "If I saw only this message, would I know what to fix?"

**Red flags:**
- Generic messages that cover multiple failure modes ("Error: failed")
- Messages that mention internal implementation details
- Messages that don't name the problematic input
- Missing context (which file? which line? which variable?)

**What this catches:** In our case: every solver failure produced the same "Cannot solve for 'x'" regardless of whether the variable was missing, circular, or producing NaN. Fixing this required changing the solver internals.

### Round 9: Code Audit

Read every line of production code, looking for classes of bugs the tests haven't covered.

**What to look for:**
- **Undefined behavior:** Signed overflow, null dereference, out-of-bounds access, bad casts, signed char in ctype functions. Use sanitizers to verify.
- **Switch fallthrough:** Missing `break` statements, missing `default` cases.
- **Resource leaks:** Opened files not closed, allocated memory not freed.
- **Dead code:** Unused variables, unreachable branches, declared-but-never-used types.
- **Duplicated logic:** Same code in two places — if one gets fixed, the other won't.
- **Implicit assumptions:** Code that works today but will break if a new enum value, type, or feature is added.

**After finding issues:** Write tests that would have caught each bug, then fix the code, then verify the tests pass.

---

## Probing Before Writing Tests

Don't write tests from imagination. Write a probe, run it, observe behavior, then write tests for what you observed.

```cpp
void probe(const char* label, auto fn) {
    std::cout << label << ": ";
    try {
        fn();
        std::cout << "OK\n";
    } catch (const std::exception& e) {
        std::cout << "THROW: " << e.what() << "\n";
    }
}
```

The probe tells you what the code actually does. The test asserts that it keeps doing it. This prevents writing tests that encode your assumptions rather than the code's behavior.

**Probe → Fix → Test** is the cycle:
1. Probe reveals unexpected behavior (crash, wrong answer, bad error message)
2. Fix the code to behave correctly
3. Write a test that locks in the correct behavior

If the probe shows the code already behaves correctly, write the test anyway — it prevents regression.

---

## Sanitizers as Test Amplifiers

Sanitizers don't find bugs on their own. They amplify your tests — the same test that "passes" without a sanitizer might reveal UB, memory leaks, or buffer overflows with one.

```makefile
asan: tests.cpp
    g++ -fsanitize=address -fno-omit-frame-pointer -O1 -g -o test_asan tests.cpp
    ASAN_OPTIONS=detect_leaks=1 ./test_asan

ubsan: tests.cpp
    g++ -fsanitize=undefined -O1 -g -o test_ubsan tests.cpp
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./test_ubsan
```

**What each catches:**
- **AddressSanitizer:** Memory leaks, use-after-free, buffer overflow, double-free
- **UndefinedBehaviorSanitizer:** Integer overflow, bad casts, null dereference, shift overflow

**When to run:** After every test round, after every code change, before every release.

**Caveat:** Sanitizers change stack frame size. Tests that push recursion limits may need reduced depths under sanitizers. Use compile-time detection:

```cpp
#if defined(__SANITIZE_ADDRESS__)
    constexpr int DEPTH = 500;    // reduced for ASan
#else
    constexpr int DEPTH = 10000;  // normal
#endif
```

---

## Organizing Tests

Group tests by what they defend against, not by what code they call:

```
Functional tests         → "Does it work?"
Edge cases               → "Does it work at the boundaries?"
Garbage input            → "Does it survive bad input?"
Roundtrip consistency    → "Is it self-consistent?"
Statefulness             → "Does it stay clean between calls?"
Numeric extremes         → "Does it handle weird numbers?"
Scale and depth          → "Does it handle large inputs?"
Error messages           → "Does it explain failures clearly?"
Code audit regressions   → "Are the subtle bugs fixed?"
```

Each group targets a different failure mode. If you only write functional tests, you'll miss boundary errors. If you only write edge cases, you'll miss state leaks. Coverage comes from diversity of approach, not from line coverage metrics.

---

## What Good Test Coverage Looks Like

Not a number. A set of properties:

- **Every public function** has tests for its normal behavior
- **Every error path** has a test that triggers it and checks the error message
- **Every boundary condition** (zero, empty, max, negative) is tested
- **Every code change that fixed a bug** has a regression test
- **Sanitizers pass clean** on the full test suite
- **Probe results** are documented as tests, not just run once and forgotten
- **Invariants** (roundtrip, idempotency, independence) are tested systematically

The goal is not 100% line coverage. The goal is that when something breaks, a test catches it before a user does.
