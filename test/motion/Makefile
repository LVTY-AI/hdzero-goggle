CC ?= cc
TARGET ?= HDZGOGGLE2
CFLAGS ?= -std=gnu11 -Wall -Wextra
CFLAGS += -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations
CFLAGS += -ffunction-sections -fdata-sections -D$(TARGET)=1
CPPFLAGS += -I../../src -I../../src/core -I../../lib/lvgl -I../../lib/log/include -I../../lib/minIni/src
LDLIBS += -pthread -lm

ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections
endif

BINARY = test_motion_wake_$(TARGET)
CASES = built-in expansion dual av-in av-in-dual hdzero menu hdmi

.PHONY: all run clean
all: $(BINARY)

$(BINARY): test_motion_wake.c ../../src/core/ht.c $(wildcard ../../src/core/*.h) $(wildcard ../../src/driver/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

run: $(BINARY)
	@set -e; for scenario in $(CASES); do ./$(BINARY) $$scenario; done

clean:
	rm -f test_motion_wake_HDZGOGGLE test_motion_wake_HDZGOGGLE2 test_motion_wake_HDZBOXPRO
