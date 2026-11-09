/* SPDX-License-Identifier: MIT
 *
 * 0037: the .bin's app_main(). The analyzer is the ELF that elf/ built
 * from analyzer.c, embedded in this firmware, and this loads it the way
 * a launcher would load the published copy.
 *
 * u-lothesome-0001: the analyzer is built for a box, so it runs in one here
 * as on Avoidant's umode branch -- RISC-V user mode, in one block of PSRAM
 * the PMP grants it (and the framebuffer beside it, once it asks for it),
 * every call outside it answered by feckless_box_calls -- and not through
 * feckless_abi_run(), which would refuse it by the name feckless_abi_box_v1.
 * Its capture task is a thread of the box (feckless_umode_app_spawn()).
 */
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "feckless_abi.h"
#include "feckless_box_calls.h"
#include "feckless_umode_idf.h"
#include "feckless_umode_load.h"

extern const uint8_t elf_start[] asm("_binary_lothesome_analyzer_app_elf_start");
extern const uint8_t elf_end[]   asm("_binary_lothesome_analyzer_app_elf_end");

void app_main(void)
{
    /* The box: the largest PSRAM block less 3 MB for this firmware (the
     * framebuffer and the drivers' buffers come after), at most 16 MB; 64 KB
     * of it app_main's stack. The stack guard's interrupt out of every
     * core's routing first, which a window needs. */
    feckless_umode_guard_unroute();
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t box = big > (3u << 20) ? big - (3u << 20) : 0;
    if (box > (16u << 20)) box = 16u << 20;
    box &= ~(size_t)63;
    uint32_t n;
    const feckless_umode_call_t *calls = feckless_box_calls_v1(&n);
    feckless_umode_app_t *app = NULL;
    esp_err_t e = feckless_umode_load(elf_start, (size_t)(elf_end - elf_start), calls, n,
                                      (uint32_t)box, 64 * 1024, &app);
    if (e != ESP_OK) {
        ESP_LOGE("standalone", "the analyzer would not load in a box: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI("standalone", "the analyzer in a box of %u KB", (unsigned)(box / 1024));
    const feckless_umode_end_t end = feckless_umode_app_run(app);
    ESP_LOGE("standalone", "the analyzer ended: reason %d, %s at 0x%08x", (int)end.reason,
             end.reason == FECKLESS_UMODE_END_FAULT ? feckless_umode_cause_name(end.mcause) : "-",
             (unsigned)end.pc);
    feckless_umode_unload(app);
    feckless_box_calls_end();
}
