CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra -Wno-unused-parameter
.DEFAULT_GOAL := all

.PHONY: all run clean

all: test_avshare_reset test_dvr_naming

test_dvr_trace: test_dvr_trace.c ../../lib/log/src/dvr_trace.c ../../lib/log/include/log/dvr_trace.h
	$(CC) $(CFLAGS) -DDVR_TRACE_PATH='"dvr-trace-fifo"' -I../../lib/log/include test_dvr_trace.c ../../lib/log/src/dvr_trace.c -pthread -o $@

.PHONY: run_dvr_trace
run_dvr_trace: test_dvr_trace
	./test_dvr_trace

test_avshare_reset: test_avshare_reset.c
	$(CC) $(CFLAGS) $< -o $@

NAMING_CPPFLAGS = -DHDZGOGGLE=1 -I../../src -I../../src/core -I../../lib/lvgl -I../../lib/log/include -I../../lib/minIni/src
NAMING_CFLAGS = -ffunction-sections -fdata-sections -Wno-unused-function
ifeq ($(shell uname -s),Darwin)
NAMING_LDFLAGS = -Wl,-dead_strip
else
NAMING_LDFLAGS = -Wl,--gc-sections
endif

test_dvr_naming: test_dvr_naming.c ../../src/core/dvr.c ../../src/driver/rtc.h ../../lib/log/src/dvr_trace.c
	$(CC) $(CFLAGS) $(NAMING_CPPFLAGS) $(NAMING_CFLAGS) $< ../../lib/log/src/dvr_trace.c $(NAMING_LDFLAGS) -pthread -o $@

run: all
	./test_avshare_reset
	./test_dvr_naming

clean:
	rm -f test_avshare_reset test_dvr_naming test_dvr_trace
