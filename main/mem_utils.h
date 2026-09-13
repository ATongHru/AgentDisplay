#pragma once

#include <stddef.h>

void *psram_malloc(size_t size);
void *dram_malloc(size_t size);
void mem_report(const char *tag);
