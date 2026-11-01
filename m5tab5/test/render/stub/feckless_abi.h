/* 0050: the ELF's tie to the launcher's ABI (0039), which a host has no
 * launcher for. analyzer.c has included it since 0039; without this the
 * render did not build.
 *
 * SPDX-License-Identifier: MIT */
#pragma once
static inline void feckless_abi_require(void) {}
