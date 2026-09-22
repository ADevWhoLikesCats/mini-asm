CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -Iinclude
TARGETS ?= x86_64

CORE_SRC := $(wildcard src/ir/*.c src/backend/*.c src/targets/*/*.c)
CORE_OBJ := $(patsubst %.c,build/%.o,$(CORE_SRC))

all: cc-backend

cc-backend: $(CORE_OBJ) build/tools/cc-backend.o
	$(CC) $(CFLAGS) $^ -o $@

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

BT_TESTS := bt_add bt_printf bt_struct

$(BT_TESTS): %: build/tools/%.o $(CORE_OBJ)
	$(CC) $(CFLAGS) $^ -o $@

bt-test: $(BT_TESTS)
	@for t in $(BT_TESTS); do ./$$t || exit 1; done

clean:
	rm -rf build cc-backend

test: all bt-test
	./tests/run.sh $(TARGETS)

.PHONY: all clean test
