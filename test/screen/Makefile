CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Isrc -Isrc/driver -I../../src/core

.PHONY: all run clean

all: test_screen_control

test_screen_control: test_screen_control.c ../../src/driver/screen_control.c
	$(CC) $(CFLAGS) -I../../src -I../../src/driver -pthread $^ -o $@

run: test_screen_control
	./test_screen_control

clean:
	rm -f test_screen_control
