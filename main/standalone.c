/* SPDX-License-Identifier: MIT
 *
 * 0037: the .bin's app_main(). The analyzer is the ELF that elf/ built
 * from analyzer.c, embedded in this firmware, and this loads it the way
 * a launcher would load the published copy: through lothesome_abi.
 */
#include <stdint.h>

#include "esp_log.h"
#include "lothesome_abi.h"

extern const uint8_t elf_start[] asm("_binary_lothesome_analyzer_app_elf_start");
extern const uint8_t elf_end[]   asm("_binary_lothesome_analyzer_app_elf_end");

void app_main(void)
{
    int ret = lothesome_abi_run(elf_start, (size_t)(elf_end - elf_start));
    ESP_LOGE("standalone", "the analyzer did not start: %d", ret);
}
