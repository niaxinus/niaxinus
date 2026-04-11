CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Wextra -std=c11 -Isrc -MMD -MP
LDFLAGS = -lpthread

# Optional readline support
READLINE_CFLAGS :=
READLINE_LDFLAGS :=
ifeq ($(shell pkg-config --exists readline 2>/dev/null && echo yes),yes)
    READLINE_CFLAGS  := -DHAVE_READLINE $(shell pkg-config --cflags readline)
    READLINE_LDFLAGS := $(shell pkg-config --libs readline)
endif
CFLAGS  += $(READLINE_CFLAGS)
LDFLAGS += $(READLINE_LDFLAGS)

SRC_DIR   = src
BUILD_DIR = build
BIN       = $(BUILD_DIR)/nxsc

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

.PHONY: all clean test test-bash-compat

all: $(BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: $(BIN)
	$(BIN) workspace/hello-world.nxs $(BUILD_DIR)/hello-world
	@echo "--- running compiled binary ---"
	$(BUILD_DIR)/hello-world
	@echo "--- for-loop test ---"
	$(BIN) workspace/for-loop.nxs $(BUILD_DIR)/for-loop-bin
	$(BUILD_DIR)/for-loop-bin

test-bash-compat: $(BIN)
	bash tests/bash-compat/compare.sh

clean:
	rm -rf $(BUILD_DIR)

-include $(DEPS)
