# Host test for main/spectrum.h.
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

spectrumtest: spectrumtest.c ../main/spectrum.h ../main/monoring.h ../main/landmap.h
	$(CC) $(CFLAGS) -o $@ spectrumtest.c $(LDFLAGS)

run: spectrumtest
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 ./spectrumtest

clean:
	rm -f spectrumtest

.PHONY: all run clean
