CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter

.PHONY: all run clean

all: test_avshare_reset

test_avshare_reset: test_avshare_reset.c
	$(CC) $(CFLAGS) $< -o $@

run: test_avshare_reset
	./test_avshare_reset

clean:
	rm -f test_avshare_reset
