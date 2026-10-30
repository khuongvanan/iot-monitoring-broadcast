#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>

static inline uint8_t *audioAlloc(size_t bytes) {
  void *p=heap_caps_malloc(bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!p)p=malloc(bytes);
  return (uint8_t*)p;
}
