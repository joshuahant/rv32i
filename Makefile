TARGET = test

INC_DIR = include
OBJ_DIR := build
DEP_DIR := $(OBJ_DIR)/.deps

$(DEP_DIR): ; @mkdir -p $@

CC = gcc
CXX = g++
RM = rm -rf
CXXFLAGS = -g -Wall -Werror -O0 -I $(INC_DIR)
LDFLAGS = -g

DEPFLAGS = -MT $@ -MMD -MP -MF $(DEP_DIR)/$*.d

#SRC_FILES = $(wildcard src/*.cpp)
SRC_FILES = main.cpp
OBJ_FILES = $(SRC_FILES:%.cpp=$(OBJ_DIR)/%.o)

COMPILE.cpp = $(CXX) $(DEPFLAGS) $(CXXFLAGS) -c

all: $(TARGET)
default: $(TARGET)

$(TARGET): $(OBJ_FILES)
	$(CXX) $(LDFLAGS) $^ -o $(TARGET)

$(OBJ_DIR)/%.o : src/%.cpp
$(OBJ_DIR)/%.o : src/%.cpp $(DEP_DIR)/%.d | $(DEP_DIR)
	$(COMPILE.cpp) $(OUTPUT_OPTION) $<

DEPFILES := $(SRC_FILES:%.cpp=$(DEP_DIR)/%.d)
$(DEPFILES):

clean:
	$(RM) $(TARGET) build

include $(wildcard $(DEPFILES))
