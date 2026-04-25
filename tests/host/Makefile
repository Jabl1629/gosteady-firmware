# Host-side regression test for the M10 V1 algorithm C port.
#
# Builds the algo modules from src/algo/ + a host test driver, runs
# against the .bin reference vectors in src/algo/test_vectors/. No
# Zephyr/CMSIS dependency — pure C99 + libm + the host's compiler.
#
#     make           # build + run
#     make build     # build only
#     make run       # run only (assumes build is current)
#     make clean
#     make verbose   # print every per-sample max-diff

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-function -g
LDFLAGS ?=
LDLIBS  ?= -lm

REPO_ROOT := $(abspath ../..)
ALGO_DIR  := $(REPO_ROOT)/src/algo
INC       := -I$(ALGO_DIR) -I.

ALGO_SRCS := \
	$(ALGO_DIR)/gs_filters.c \
	$(ALGO_DIR)/gs_motion_gate.c \
	$(ALGO_DIR)/gs_step_detector.c \
	$(ALGO_DIR)/gs_roughness.c \
	$(ALGO_DIR)/gs_pipeline.c

TEST_SRCS := test_loader.c test_main.c

BUILD_DIR := build
BIN := $(BUILD_DIR)/test_algo

ALGO_OBJS := $(patsubst $(ALGO_DIR)/%.c,$(BUILD_DIR)/algo_%.o,$(ALGO_SRCS))
TEST_OBJS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(TEST_SRCS))
OBJS := $(ALGO_OBJS) $(TEST_OBJS)

.PHONY: all build run verbose clean
all: build run

build: $(BIN)

run: $(BIN)
	cd $(REPO_ROOT) && tests/host/$(BIN)

verbose: $(BIN)
	cd $(REPO_ROOT) && tests/host/$(BIN) 2>&1 | tee tests/host/last_run.log

clean:
	rm -rf $(BUILD_DIR)

$(BIN): $(OBJS) | $(BUILD_DIR)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD_DIR)/algo_%.o: $(ALGO_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
