CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter
INCLUDES := -I../../src -I../../src/core -I../../lib/lvgl -I../../lib/lvgl/lvgl -I../../lib/log/include -I../../lib/minIni

.PHONY: all run clean

all: test_sleep_beep

test_sleep_beep: test_sleep_beep.c ../../src/core/sleep_mode.c
	$(CC) $(CFLAGS) -DHDZBOXPRO $(INCLUDES) $^ -o $@

run: test_sleep_beep
	./test_sleep_beep

clean:
	rm -f test_sleep_beep
