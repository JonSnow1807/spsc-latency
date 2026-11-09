CXX = clang++
BUILD ?= out
TSAN ?= 0
NATIVE ?= 1
NATIVE_FLAG := $(if $(filter 1,$(NATIVE)),$(shell printf 'int main(){}\n' | $(CXX) -x c++ -std=c++20 -march=native -fsyntax-only - >/dev/null 2>&1 && printf '%s' '-march=native'))
COMMON := -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude -Ibench $(NATIVE_FLAG)
ifeq ($(TSAN),1)
OPT := -O1 -g -fsanitize=thread -fno-omit-frame-pointer
else
OPT := -O3
endif
HEADERS := $(wildcard include/spsc/*.hpp bench/*.hpp)
BUILD_INFO = -DSPSC_BUILD_FLAGS='"$(COMMON) $(OPT) $(CPPFLAGS) $(CXXFLAGS) $(LDFLAGS)"'
.PHONY: all test stress clean
all: $(BUILD)/spsc_tests $(BUILD)/spsc_bench
$(BUILD):
	mkdir -p $@
$(BUILD)/spsc_tests: tests/tests.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(COMMON) $(OPT) $(BUILD_INFO) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@
$(BUILD)/spsc_bench: bench/main.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(COMMON) $(OPT) $(BUILD_INFO) $(CPPFLAGS) $(CXXFLAGS) $< $(LDFLAGS) -o $@
test: all
	$(BUILD)/spsc_tests --items 1000000
stress: all
	$(BUILD)/spsc_tests --items 100000000
clean:
	rm -rf $(BUILD)
