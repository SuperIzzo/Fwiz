CXX = g++
CXXFLAGS = -std=c++17 -Wall -O2
TARGET = bin/fwiz
TEST = bin/fwiz_tests
HEADERS = src/system.h src/expr.h src/parser.h src/lexer.h src/trace.h

all: $(TARGET)

$(TARGET): src/main.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) -o $(TARGET) src/main.cpp

$(TEST): src/tests.cpp $(HEADERS) | bin
	$(CXX) $(CXXFLAGS) -o $(TEST) src/tests.cpp

test: $(TARGET) $(TEST)
	./$(TEST)

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
analyze:
	@which cppcheck > /dev/null 2>&1 && ( \
		echo "=== cppcheck ===" && \
		cppcheck --enable=warning,style,performance --std=c++17 \
			--suppress=passedByValue --suppress=useStlAlgorithm \
			--error-exitcode=1 src/main.cpp src/tests.cpp 2>&1 \
	) || echo "cppcheck not installed, skipping"
	@which clang-tidy > /dev/null 2>&1 && ( \
		echo "=== clang-tidy ===" && \
		clang-tidy src/main.cpp \
			--checks='bugprone-*,performance-*,clang-analyzer-*,-bugprone-easily-swappable-parameters,-performance-inefficient-string-concatenation' \
			-- -std=c++17 -I src 2>&1 \
	) || echo "clang-tidy not installed, skipping"
	@echo "Static analysis complete."

bin:
	mkdir -p bin

clean:
	rm -f $(TARGET) $(TEST) bin/fwiz_asan bin/fwiz_ubsan
