# main/analyzer.c's drawing on a PC: one picture of the screen per view.
#
#   make -C test/render
#
# Needs the Ark12 font, which is feckless-graphics-handler's. After an
# idf.py build it is in managed_components; point ARK12 elsewhere if not.
# And cmake, for the licences page's list (0036): cmake/libversions.cmake
# run on its own, into libversions/ here.
#
# SPDX-License-Identifier: MIT

CC    ?= cc
ARK12 ?= ../../managed_components/feckless_graphics_handler/fonts/ark12

CFLAGS  := -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-function \
           -Wno-unused-variable -Wno-unused-parameter \
           -Istub -I../../main -I$(ARK12) -Ilibversions
LDFLAGS := -lm

all: run

libversions/libversions.h: ../../cmake/libversions.cmake ../../dependencies.lock
	cmake -P ../../cmake/libversions.cmake

render: render.c stub/stub.h ../../main/analyzer.c ../../main/spectrum.h ../../main/menu.h \
        libversions/libversions.h
	$(CC) $(CFLAGS) -o $@ render.c $(ARK12)/ark12.c $(LDFLAGS)

run: render
	./render

clean:
	rm -f render *.ppm
	rm -rf libversions

.PHONY: all run clean
