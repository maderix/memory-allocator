CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic
RELEASE_FLAGS = -O2 -DNDEBUG
DEBUG_FLAGS = -g -O0 -DDEBUG

TARGET = memory_allocator_test
SRCS = main.cpp
HEADERS = memory_allocator.h
BUILD_DIR = build

TEST_TARGET = allocator_basic_test
TEST_SRCS = tests/allocator_basic_test.cpp

.PHONY: all clean debug release test

all: release

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

release: CXXFLAGS += $(RELEASE_FLAGS)
release: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(BUILD_DIR)/$(TARGET)

debug: CXXFLAGS += $(DEBUG_FLAGS)
debug: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $(BUILD_DIR)/$(TARGET)_debug

test: CXXFLAGS += $(DEBUG_FLAGS)
test: $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -I. $(TEST_SRCS) -o $(BUILD_DIR)/$(TEST_TARGET)
	$(BUILD_DIR)/$(TEST_TARGET)

clean:
	rm -rf $(BUILD_DIR)
