CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter
INCLUDES := -I../../src -I../../src/core -I../../lib/lvgl -I../../lib/lvgl/lvgl -I../../lib/log/include -I../../lib/minIni

.PHONY: all run clean

all: test_battery_cells

test_battery_cells: test_battery_cells.c ../../src/core/battery.c
	$(CC) $(CFLAGS) -DHDZBOXPRO $(INCLUDES) $^ -o $@

run: test_battery_cells
	./test_battery_cells

clean:
	rm -f test_battery_cells
