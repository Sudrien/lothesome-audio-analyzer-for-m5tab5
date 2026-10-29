/* SPDX-License-Identifier: MIT
 *
 * 0037: the host side of the analyzer's ELF. The ELF (elf/, built from
 * main/analyzer.c) leaves every library call undefined; this
 * component links those libraries and hands esp_elf a table of them,
 * one entry for each name in exports.txt.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_elf.h"

/* Every symbol in exports.txt, by name, ESP_ELFSYM_END last. */
extern esp_elf_symbol_table_t g_lothesome_abi_elfsyms[];

/* Relocate an analyzer ELF image and call its app_main(). The image
 * must stay where it is while the analyzer runs. Since the analyzer
 * never returns, neither does this, unless the image will not load:
 * then it returns esp_elf's negative errno. */
int lothesome_abi_run(const uint8_t *image, size_t size);
