# Host tests for main/spectrum.h and, 0036, main/menu.h.
#
#   make -C test
#
# SPDX-License-Identifier: MIT

CC ?= cc

CFLAGS  := -std=c11 -g -O1 -Wall -Wextra -Werror \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           -fno-omit-frame-pointer -I../main
LDFLAGS := -fsanitize=address,undefined -lm

all: run

menutest: menutest.c ../main/menu.h
	$(CC) $(CFLAGS) -o $@ menutest.c $(LDFLAGS)

spectrumtest: spectrumtest.c ../main/spectrum.h ../main/monoring.h ../main/landmap.h
	$(CC) $(CFLAGS) -o $@ spectrumtest.c $(LDFLAGS)

run: spectrumtest menutest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./spectrumtest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./menutest

clean:
	rm -f spectrumtest menutest

.PHONY: all run clean
