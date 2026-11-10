CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Wextra
INCLUDES := -I../../src

.PHONY: all run clean

all: test_wav_info

test_wav_info: test_wav_info.c ../../src/util/wav_info.c
	$(CC) $(CFLAGS) $(INCLUDES) $^ -o $@

run: test_wav_info
	./test_wav_info

clean:
	rm -f test_wav_info
