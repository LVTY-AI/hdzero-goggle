# Requires Linux framebuffer headers; can run inside the build container.
CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable
CFLAGS += -ffunction-sections -fdata-sections
CPPFLAGS += -DHDZBOXPRO=1 -I../../src -I../../src/core -I../../src/driver -I../../lib/lvgl -I../../lib/log/include
ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections
endif
.PHONY: run clean
run: test_orbit
	./test_orbit
test_orbit: test_orbit.c ../../src/ui/ui_porting.c
	$(CC) $(CPPFLAGS) $(CFLAGS) test_orbit.c $(LDFLAGS) -o $@
clean:
	rm -f test_orbit
