CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra

.PHONY: all run clean

all: test_alarm_angle

test_alarm_angle: test_alarm_angle.c ../../src/core/ht_alarm.h
	$(CC) $(CFLAGS) -I../../src $< -o $@

run: test_alarm_angle
	./test_alarm_angle

clean:
	rm -f test_alarm_angle
