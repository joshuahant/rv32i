BUILD_DIR := build
HDL_DIR := hdl
TB_DIR := tb
SIM_DIR := sim

RM = rm -rf

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2

.PHONY: tb sim-test sim-run core-sim riscv-tests riscv-tests-build clean-tests

all: tb run

clean:
	$(RM) $(BUILD_DIR)

tb:
	mkdir -p $(BUILD_DIR)
	iverilog -o $(BUILD_DIR)/tb -c files.txt

run:
	vvp $(BUILD_DIR)/tb

# ── Cache simulator ───────────────────────────────────────────────────────────

SIM_SRCS := $(SIM_DIR)/cache.cpp
SIM_TEST := $(SIM_DIR)/tests.cpp

sim-test: $(SIM_SRCS) $(SIM_TEST)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SIM_DIR) -o $(BUILD_DIR)/cache_test $^
	./$(BUILD_DIR)/cache_test

sim-build: $(SIM_SRCS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SIM_DIR) -c $(SIM_SRCS) -o $(BUILD_DIR)/cache.o

# ── RV32I core simulator ──────────────────────────────────────────────────────

CORE_SRCS := $(SIM_DIR)/cache.cpp \
             $(SIM_DIR)/rv32i.cpp \
             $(SIM_DIR)/memory.cpp \
             $(SIM_DIR)/pipeline.cpp \
             $(SIM_DIR)/core_sim.cpp

core-sim: $(CORE_SRCS)
	mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I$(SIM_DIR) -o $(BUILD_DIR)/core_sim $^

# ── riscv-tests ───────────────────────────────────────────────────────────────

TESTS_DIR      := tests
RISCV_TESTS    := $(TESTS_DIR)/riscv-tests
RV32UI_OUT     := $(TESTS_DIR)/rv32ui-p
RISCV_CC       := riscv64-unknown-elf-gcc
RISCV_MARCH    := rv32i_zicsr_zifencei
RISCV_ABI      := ilp32
RISCV_CFLAGS   := -march=$(RISCV_MARCH) -mabi=$(RISCV_ABI) \
                  -static -mcmodel=medany \
                  -fvisibility=hidden -nostdlib -nostartfiles \
                  -I$(RISCV_TESTS)/env/p \
                  -I$(RISCV_TESTS)/isa/macros/scalar \
                  -T$(RISCV_TESTS)/env/p/link.ld

# Build all rv32ui-p ELFs from the riscv-tests source directory.
riscv-tests-build:
	mkdir -p $(RV32UI_OUT)
	@for src in $(RISCV_TESTS)/isa/rv32ui/*.S; do \
	    name=$$(basename $$src .S); \
	    $(RISCV_CC) $(RISCV_CFLAGS) $$src -o $(RV32UI_OUT)/rv32ui-p-$$name \
	        && echo "  built  rv32ui-p-$$name" \
	        || echo "  SKIP   rv32ui-p-$$name (build failed)"; \
	done

# Run all rv32ui-p ELFs through the core simulator.
# Prints PASS/FAIL per test and a summary at the end.
riscv-tests: core-sim
	@pass=0; fail=0; skip=0; \
	for elf in $(RV32UI_OUT)/rv32ui-p-*; do \
	    [ -f "$$elf" ] || continue; \
	    name=$$(basename $$elf); \
	    out=$$(./$(BUILD_DIR)/core_sim $$elf 2>&1); \
	    if echo "$$out" | grep -q '\[sim\] PASS'; then \
	        echo "  PASS  $$name"; pass=$$((pass+1)); \
	    else \
	        echo "  FAIL  $$name"; fail=$$((fail+1)); \
	    fi; \
	done; \
	echo ""; \
	echo "Results: $$pass passed, $$fail failed"

clean-tests:
	$(RM) $(RV32UI_OUT)

