CC ?= cc
TARGET ?= HDZGOGGLE
CFLAGS ?= -std=gnu11 -Wall -Wextra
CFLAGS += -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations
CFLAGS += -ffunction-sections -fdata-sections -D$(TARGET)=1
CPPFLAGS += -I../../src -I../../src/core -I../../lib/lvgl -I../../lib/log/include -I../../lib/minIni/src
LDLIBS += -pthread

ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections
endif

BINARY = test_hdmi_reacquire_$(TARGET)
CASES = retry-ui retry-hdmi other-sources transient-loss recovery present-unknown

.PHONY: all run clean

all: $(BINARY)

$(BINARY): test_hdmi_reacquire.c $(wildcard ../../src/driver/hardware-*.c) $(wildcard ../../src/driver/*.h)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LDFLAGS) $(LDLIBS) -o $@

run: $(BINARY)
	@set -e; for scenario in $(CASES); do ./$(BINARY) $$scenario; done

clean:
	rm -f test_hdmi_reacquire_HDZGOGGLE test_hdmi_reacquire_HDZGOGGLE2 test_hdmi_reacquire_HDZBOXPRO
