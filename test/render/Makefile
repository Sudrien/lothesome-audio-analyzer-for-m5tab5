# main/analyzer.c's drawing on a PC: one picture of the screen per view.
#
#   make -C test/render
#
# Needs the Ark12 font, which is feckless-graphics-handler's. After an
# idf.py build it is in managed_components; point ARK12 elsewhere if not.
#
# SPDX-License-Identifier: MIT

CC    ?= cc
ARK12 ?= ../../managed_components/feckless_graphics_handler/fonts/ark12

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
           -Wno-unused-variable -Wno-unused-parameter \
           -Istub -I../../main -I$(ARK12)
LDFLAGS := -lm

all: run

render: render.c stub/stub.h ../../main/analyzer.c ../../main/spectrum.h
	$(CC) $(CFLAGS) -o $@ render.c $(ARK12)/ark12.c $(LDFLAGS)

run: render
	./render

clean:
	rm -f render *.ppm

.PHONY: all run clean
