CC      = gcc
CFLAGS  = -O3 -march=native -Wall -Wextra -std=c11 -Isrc
LDFLAGS = -lpthread

SRC_DIR   = src
BUILD_DIR = build
BIN       = $(BUILD_DIR)/nxsc

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

.PHONY: all clean test

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

clean:
	rm -rf $(BUILD_DIR)
