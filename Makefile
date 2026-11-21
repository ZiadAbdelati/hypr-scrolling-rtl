CXX ?= g++
BUILD_DIR := build
PLUGIN := $(BUILD_DIR)/hypr-scrolling-rtl.so

COMMON_FLAGS := -std=c++23 -Wall -Wextra -Wpedantic
PLUGIN_FLAGS := -O2 -shared -fPIC
HYPR_CFLAGS := $(shell pkg-config --cflags hyprland)

ifeq ($(notdir $(CXX)),g++)
PLUGIN_FLAGS += -fno-gnu-unique
endif

.PHONY: all clean

all: $(PLUGIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PLUGIN): src/main.cpp | $(BUILD_DIR)
	$(CXX) $(COMMON_FLAGS) $(PLUGIN_FLAGS) $(CXXFLAGS) $(HYPR_CFLAGS) \
		src/main.cpp -o $(PLUGIN) $(LDFLAGS)

clean:
	rm -f $(PLUGIN)
