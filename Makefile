# Hotpath: tick-to-trade engine. Zero dependencies: a C++20 compiler and make.
# Reuses project 1 (the market-data handler) as a library from $(P1).
#
#   make            build everything (-O2, warnings are errors)
#   make test       unit, integration and end-to-end tests
#   make asan       tests under AddressSanitizer + UndefinedBehaviorSanitizer
#   make tsan       tests under ThreadSanitizer
#   make bench      queue, fast-path and dispatch microbenchmarks
#   make demo       sim + engine + control + replay over loopback (scripts/run_demo.sh)
#   make fuzz       libFuzzer target for the order-entry decoders (clang with libFuzzer)

CXX      ?= c++
P1       ?= ../Low-Latency-Data-Handler
BUILD    ?= build
OPT      ?= -O2
SAN      ?=
EXTRA    ?=
CXXFLAGS := -std=c++20 $(OPT) -g -Wall -Wextra -Wpedantic -Werror -pthread -I. -Isrc -I$(P1)/src $(SAN) $(EXTRA)
LDFLAGS  := -pthread $(SAN)
LDLIBS   :=
ifeq ($(shell uname),Linux)
LDLIBS   += -lrt
endif

P1_SRC   := order_book.cpp net.cpp handler.cpp
P1_OBJ   := $(P1_SRC:%.cpp=$(BUILD)/p1/%.o)
LIB_SRC  := src/shm/region.cpp src/engine/gateway.cpp src/engine/feed_adapter.cpp \
            src/sim/sim_exchange.cpp src/replay/replay.cpp src/common/alloc_hook.cpp
LIB_OBJ  := $(LIB_SRC:%.cpp=$(BUILD)/%.o) $(P1_OBJ)
TEST_SRC := $(wildcard tests/*.cpp)
TEST_OBJ := $(TEST_SRC:%.cpp=$(BUILD)/%.o)
BENCH_SRC:= $(wildcard bench/*.cpp)
BENCH_OBJ:= $(BENCH_SRC:%.cpp=$(BUILD)/%.o)
BINS     := $(BUILD)/hp_engine $(BUILD)/hp_control $(BUILD)/hp_sim $(BUILD)/hp_replay \
            $(BUILD)/hp_bench $(BUILD)/run_tests

all: $(BINS)

$(BUILD)/p1/%.o: $(P1)/src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/hp_engine: $(BUILD)/src/engine/engine_main.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)
$(BUILD)/hp_control: $(BUILD)/src/control/control_main.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)
$(BUILD)/hp_sim: $(BUILD)/src/sim/sim_main.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)
$(BUILD)/hp_replay: $(BUILD)/src/replay/replay_main.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)
$(BUILD)/hp_bench: $(BENCH_OBJ) $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)
$(BUILD)/run_tests: $(TEST_OBJ) $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) $(LDLIBS)

test: $(BUILD)/run_tests
	./$(BUILD)/run_tests

# Sanitizers install their own allocator, so the allocation-counting hook is off.
asan:
	$(MAKE) BUILD=build/asan OPT=-O1 EXTRA=-DHP_NO_ALLOC_HOOK \
	  SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all" test

tsan:
	$(MAKE) BUILD=build/tsan OPT=-O1 EXTRA=-DHP_NO_ALLOC_HOOK \
	  SAN="-fsanitize=thread -fno-omit-frame-pointer" test

bench: $(BUILD)/hp_bench
	./$(BUILD)/hp_bench all

demo: all
	./scripts/run_demo.sh

FUZZ_SECONDS ?= 60
FUZZ_FLAGS := -std=c++20 -O1 -g -Isrc -I$(P1)/src -fsanitize=fuzzer,address,undefined
fuzz:
	@mkdir -p $(BUILD)
	$(CXX) $(FUZZ_FLAGS) fuzz/fuzz_order_entry.cpp -o $(BUILD)/fuzz_order_entry
	./$(BUILD)/fuzz_order_entry -max_total_time=$(FUZZ_SECONDS)

clean:
	rm -rf build journal*.bin *.csv results

.PHONY: all test asan tsan bench demo fuzz clean
-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
