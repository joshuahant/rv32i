BUILD_DIR := build
HDL_DIR := hdl
TB_DIR := tb

RM = rm -rf

.PHONY: tb

all: tb run

clean:
	$(RM) $(BUILD_DIR)

tb:
	mkdir -p $(BUILD_DIR)
	iverilog -o $(BUILD_DIR)/tb -c files.txt

run:
	vvp $(BUILD_DIR)/tb

