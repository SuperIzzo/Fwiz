// libFuzzer entry point for the fwiz lexer + parser + simplifier pipeline.
//
// Build: `make fuzz` (Clang only — libFuzzer is a Clang/LLVM feature).
// Run:   ./bin/fwiz_fuzz fuzz_corpus/ -max_total_time=60
//
// Why this harness exists: hand-written tests cover known-good and a small
// set of known-bad inputs. libFuzzer + ASan find parser bugs that no human
// thinks to write — bytes that round-trip through the lexer's UTF-8 handling,
// integer-overflow corners in number parsing, recursion-depth blowups in
// nested formula calls, and stack-frame UAFs in the arena allocator.
//
// What we fuzz: `FormulaSystem::load_string(source)`. That single call
// exercises the lexer (all token classes), the parser (every grammar rule),
// the simplifier (rewrite-rule application during load), and the arena
// allocator (every Expr node allocation path). We deliberately do NOT call
// resolve() — solving requires well-formed variable bindings, and the goal
// is to fuzz the front end. Solve-path fuzzing is a separate harness.
//
// Why we catch all exceptions: the parser throws std::runtime_error on
// fatal syntax errors. That is *expected* behavior — a malformed input
// surfacing as a thrown exception is not a crash, it is correct rejection.
// libFuzzer treats an uncaught exception that escapes LLVMFuzzerTestOneInput
// as a finding, so we swallow them. Real bugs (ASan reports, abort, SIGSEGV,
// failed assertions) bypass C++ EH and are still surfaced by libFuzzer.

#include "system.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string source(reinterpret_cast<const char*>(data), size);
    FormulaSystem sys;
    try {
        sys.load_string(source);
    } catch (...) {
        // Expected: parser rejects malformed input via std::runtime_error.
        // Not a crash — return success so libFuzzer keeps mutating.
        return 0;
    }
    return 0;
}
