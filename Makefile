CC      := /usr/bin/clang
BUILD   ?= debug

WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wformat=2 \
           -Wnull-dereference -Wimplicit-fallthrough
CFLAGS  := -std=c17 -g3 $(WARN) -pthread -D_GNU_SOURCE -Isrc
LDFLAGS := -pthread
DEPFLAGS = -MMD -MP

ifeq ($(BUILD),debug)
  SAN     := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
  CFLAGS  += -O0 $(SAN)
  LDFLAGS += $(SAN)
else ifeq ($(BUILD),tsan)
  CFLAGS  += -O1 -fsanitize=thread
  LDFLAGS += -fsanitize=thread
else ifeq ($(BUILD),release)
  CFLAGS  += -O2 -D_FORTIFY_SOURCE=2
else
  $(error unknown BUILD '$(BUILD)', use debug|tsan|release)
endif

OUT := build/$(BUILD)

# Programs: one main() each, at the top level of src/
APPS      := client server_poll server_thread
APP_OBJS  := $(addprefix $(OUT)/obj/,$(addsuffix .o,$(APPS)))
APP_BINS  := $(addprefix $(OUT)/bin/,$(APPS))

# Modules: every non-test .c in a subdirectory of src/
LIB_SRCS  := $(shell find src -mindepth 2 -name '*.c' ! -name '*_test.c')
LIB_OBJS  := $(patsubst src/%.c,$(OUT)/obj/%.o,$(LIB_SRCS))
LIB       := $(OUT)/libc10k.a

# Tests: every *_test.c anywhere under src/
TEST_SRCS := $(shell find src -name '*_test.c')
TEST_OBJS := $(patsubst src/%.c,$(OUT)/obj/%.o,$(TEST_SRCS))
TEST_BINS := $(patsubst src/%.c,$(OUT)/test/%,$(TEST_SRCS))

.PHONY: all debug tsan release test clean
.SECONDARY:

all: $(APP_BINS)

debug tsan release:
	@$(MAKE) --no-print-directory BUILD=$@ all

test: $(TEST_BINS)
	@for t in $^; do \
		echo "==> $$t"; \
		./$$t || { echo "FAIL: $$t"; exit 1; }; \
	done
	@echo "All tests passed ($(BUILD))."

$(OUT)/obj/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

$(LIB): $(LIB_OBJS)
	@mkdir -p $(@D)
	@rm -f $@
	$(AR) rcs $@ $^

$(OUT)/bin/%: $(OUT)/obj/%.o $(LIB)
	@mkdir -p $(@D)
	$(CC) $^ $(LDFLAGS) -o $@

$(OUT)/test/%: $(OUT)/obj/%.o $(LIB)
	@mkdir -p $(@D)
	$(CC) $^ $(LDFLAGS) -o $@

clean:
	rm -rf build

-include $(APP_OBJS:.o=.d) $(LIB_OBJS:.o=.d) $(TEST_OBJS:.o=.d)