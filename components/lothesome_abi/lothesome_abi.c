/* SPDX-License-Identifier: MIT
 *
 * 0037: see lothesome_abi.h.
 */
#include "lothesome_abi.h"

#include <errno.h>

#include "esp_log.h"

static const char *TAG = "abi";

/* Static: the loaded sections are referenced from here for as long as
 * the analyzer runs, which is forever. */
static esp_elf_t s_elf;

int lothesome_abi_run(const uint8_t *image, size_t size)
{
    int ret = esp_elf_init(&s_elf);
    if (ret < 0) {
        ESP_LOGE(TAG, "esp_elf_init: %d", ret);
        return ret;
    }
    ret = esp_elf_register_symbol(g_lothesome_abi_elfsyms);
    if (ret < 0 && ret != -EEXIST) {
        ESP_LOGE(TAG, "esp_elf_register_symbol: %d", ret);
        esp_elf_deinit(&s_elf);
        return ret;
    }
    ret = esp_elf_relocate(&s_elf, image, size);
    if (ret < 0) {
        /* Most often a symbol the image wants and the table lacks:
         * esp_elf has logged its name. */
        ESP_LOGE(TAG, "esp_elf_relocate: %d", ret);
        esp_elf_unregister_symbol(g_lothesome_abi_elfsyms);
        esp_elf_deinit(&s_elf);
        return ret;
    }
    ESP_LOGI(TAG, "analyzer: %u bytes, relocated", (unsigned)size);
    return esp_elf_request(&s_elf, 0, 0, NULL);
}
