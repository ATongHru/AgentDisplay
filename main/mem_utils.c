#include "mem_utils.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "mem";

void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

void *dram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

void mem_report(const char *tag)
{
    const char *who = tag && tag[0] ? tag : TAG;
    ESP_LOGI(who,
             "DRAM free=%u largest=%u | PSRAM free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}
