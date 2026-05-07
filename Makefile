CXX = g++
CXX_CLANG = clang++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wuninitialized -Wnull-dereference -Wimplicit-fallthrough -Wdouble-promotion -O2
TARGET = bin/fwiz
TEST = bin/fwiz_tests
HEADERS = src/system.h src/expr.h src/fit.h src/parser.h src/lexer.h src/trace.h

all: $(TARGET)

$(TARGET): src/main.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) -o $(TARGET) src/main.cpp

$(TEST): src/tests.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) -o $(TEST) src/tests.cpp

test: $(TARGET) $(TEST)
	./$(TEST)

# Compiles + runs the test suite under clang++ with the same warning flag set
# as GCC. Catches Clang-specific issues (e.g. stricter [[nodiscard]] handling,
# C++20 extension warnings). Soft-skip if clang++ is not on PATH.
test-clang: src/tests.cpp $(HEADERS) | bin
	@if which $(CXX_CLANG) > /dev/null 2>&1; then \
		$(CXX_CLANG) $(CXXFLAGS) -o bin/fwiz_clang_tests src/tests.cpp && \
		./bin/fwiz_clang_tests; \
	else \
		echo "clang++ not found, skipping test-clang"; \
	fi

# --- Sanitizer targets ---
# AddressSanitizer + LeakSanitizer: catches memory leaks, use-after-free,
# buffer overflows, double-free, stack overflow
asan: src/tests.cpp $(HEADERS) | bin
	$(CXX) -std=c++17 -Wall -O1 -g -fsanitize=address -fno-omit-frame-pointer \
		-o bin/fwiz_asan src/tests.cpp
	ASAN_OPTIONS=detect_leaks=1 ./bin/fwiz_asan

# UndefinedBehaviorSanitizer: catches signed overflow, null deref,
# misaligned access, shift overflow, integer division by zero
ubsan: src/tests.cpp $(HEADERS) | bin
	$(CXX) -std=c++17 -Wall -O1 -g -fsanitize=undefined -fno-omit-frame-pointer \
		-o bin/fwiz_ubsan src/tests.cpp
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./bin/fwiz_ubsan

# Run all sanitizers
sanitize: asan ubsan
	@echo "All sanitizer checks passed."

# --- Static analysis ---
# Tiered oracles. Per-cycle gate: `make analyze-fast` (cppcheck, ~1-2 min).
# User-triggered batch (run during PC idle windows): `make analyze-full`
# (clang-tidy, ~1-2h on this header-heavy codebase). `make analyze` runs both.

# tests.cpp uses inline test loops where readability is local; file-level skip
# for syntaxError / containerOutOfBounds / useStlAlgorithm — same pattern.
analyze-fast:
	@which cppcheck > /dev/null 2>&1 && ( \
		echo "=== cppcheck ===" && \
		cppcheck --enable=warning,style,performance --std=c++17 \
			--inline-suppr \
			--suppress=passedByValue \
			--suppress=throwInEntryPoint \
			--suppress=syntaxError:src/tests.cpp \
			--suppress=containerOutOfBounds:src/tests.cpp \
			--suppress=useStlAlgorithm:src/tests.cpp \
			--error-exitcode=1 src/main.cpp src/tests.cpp 2>&1 \
	) || echo "cppcheck not installed, skipping"
	@echo "Fast static analysis complete (cppcheck)."

# Excludes:
# -bugprone-easily-swappable-parameters: noisy, low signal on this codebase.
# -bugprone-exception-escape: hangs indefinitely on header-heavy TUs (whole-
#   call-graph noexcept analysis explodes on inlined templates). Documented
#   pre-2026-05-07 hang bisection in `.fwiz-workflow/debug-analyze-full-hang.md`.
# -bugprone-unchecked-optional-access: known LLVM regression hang since
#   clang-16 (LLVM issues #55530, #69298). Defensive exclude — not directly
#   observed hanging on this codebase but the dataflow engine is the same one
#   that broke on bugprone-exception-escape, so we pre-empt.
# -performance-inefficient-string-concatenation: stylistic; we prefer + over
#   stringstream for simple cases.
analyze-full:
	@which clang-tidy > /dev/null 2>&1 && ( \
		echo "=== clang-tidy ===" && \
		clang-tidy src/main.cpp \
			--checks='bugprone-*,performance-*,clang-analyzer-*,modernize-use-nodiscard,misc-const-correctness,-bugprone-easily-swappable-parameters,-bugprone-exception-escape,-bugprone-unchecked-optional-access,-performance-inefficient-string-concatenation' \
			-- -std=c++17 -I src 2>&1 \
	) || echo "clang-tidy not installed, skipping"
	@echo "Full static analysis complete (clang-tidy)."

analyze: analyze-fast analyze-full
	@echo "All static analysis complete."

bin:
	mkdir -p bin

clean:
	rm -f $(TARGET) $(TEST) bin/fwiz_asan bin/fwiz_ubsan bin/fwiz_clang_tests
