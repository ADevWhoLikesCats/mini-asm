PREFIX  ?= /usr/local
AR      ?= ar
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

# Static library: the backend (IR, parser, builder, opt, regalloc, backends)
LIB     := libmini-asm.a
LIB_OBJ := $(CORE_OBJ)

$(LIB): $(LIB_OBJ)
	$(AR) rcs $@ $^

lib: $(LIB)

install: $(LIB)
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/include
	install -d $(DESTDIR)$(PREFIX)/include/targets
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/*.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 include/targets/*.h $(DESTDIR)$(PREFIX)/include/targets/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/$(LIB)
	rm -f $(DESTDIR)$(PREFIX)/include/ir.h
	rm -f $(DESTDIR)$(PREFIX)/include/ir_builder.h
	rm -f $(DESTDIR)$(PREFIX)/include/ir_parse.h
	rm -f $(DESTDIR)$(PREFIX)/include/backend.h
	rm -f $(DESTDIR)$(PREFIX)/include/opt.h
	rm -rf $(DESTDIR)$(PREFIX)/include/targets

.PHONY: all clean test lib install uninstall
