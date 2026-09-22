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

clean:
	rm -rf build cc-backend

test: all
	./tests/run.sh $(TARGETS)

.PHONY: all clean test
