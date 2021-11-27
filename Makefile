TARGET = test

INC_DIR := include
BUILD_DIR := build
SRC_DIR := src

$(shell mkdir -p $(dir $(BUILD_DIR)) >/dev/null)

CC = gcc
CXX = g++
RM = rm -rf
CXXFLAGS = -g -Wall -Werror -O0 -I $(INC_DIR)
LDFLAGS = -g
DEPFLAGS = -MMD -MP

SRC_FILES := $(shell find $(SRC_DIR) -name '*.cpp')
OBJ_FILES := $(SRC_FILES:$(SRC_DIR)/%.cpp=$(BUILD_DIR)/%.o)
DEP_FILES := $(OBJ_FILES:%.o=%.d)

COMPILE.cpp = $(CXX) $(DEPFLAGS) $(CXXFLAGS) -c

all: $(TARGET)
default: $(TARGET)

$(TARGET): $(OBJ_FILES)
	$(CXX) $(LDFLAGS) $(OBJ_FILES) -o $(TARGET)

-include $(DEP_FILES)

$(BUILD_DIR)/%.o : $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(COMPILE.cpp) $< -o $@

clean:
	$(RM) $(TARGET) build

