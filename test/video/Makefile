CC ?= cc
CFLAGS ?= -std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function
CPPFLAGS += -D_DEFAULT_SOURCE -I../../src -I../../src/core -I../../src/driver -I../../lib/lvgl -I../../lib/log/include -I../../lib/minIni/src
ifeq ($(shell uname -s),Darwin)
LDFLAGS += -Wl,-dead_strip
else
LDFLAGS += -Wl,--gc-sections
endif
CFLAGS += -ffunction-sections -fdata-sections
SOURCES = test_camera_mode.c ../../src/core/msp_displayport.c ../../lib/log/src/log.c

.PHONY: run clean
run: test_camera_boxpro test_camera_goggle test_camera_goggle2
	./test_camera_boxpro
	./test_camera_goggle
	./test_camera_goggle2

test_camera_boxpro: $(SOURCES) ../../src/driver/hardware-boxpro.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHDZBOXPRO=1 $^ $(LDFLAGS) -pthread -o $@
test_camera_goggle: $(SOURCES) ../../src/driver/hardware-goggle.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHDZGOGGLE=1 $^ $(LDFLAGS) -pthread -o $@
test_camera_goggle2: $(SOURCES) ../../src/driver/hardware-goggle2.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -DHDZGOGGLE2=1 $^ $(LDFLAGS) -pthread -o $@

clean:
	rm -f test_camera_boxpro test_camera_goggle test_camera_goggle2
