.PHONY: all test clean

CC     ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -O2 -I ../sender/main
SRC    := ../sender/main/decode.c ../sender/main/isotp.c

all: test

test: test_logic
	./test_logic

test_logic: test_logic.c $(SRC)
	$(CC) $(CFLAGS) $^ -o $@ -lm

clean:
	rm -f test_logic
