CC?=gcc
CFLAGS?=-O2 -Wall -Wextra -std=c11 -Iinclude
SRC=$(wildcard src/*/*.c src/*/*/*.c)
OBJ=$(SRC:.c=.o)
all: cc-backend
cc-backend: $(OBJ) tools/cc-backend.o
	$(CC) $(CFLAGS) $^ -o $@
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
clean:
	rm -f $(OBJ) tools/cc-backend.o cc-backend
.PHONY: all clean
