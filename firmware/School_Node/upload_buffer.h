#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

class UploadBuffer {
public:
  uint8_t *data = nullptr;
  size_t size = 0, used = 0;
  uint32_t epoch = 0, touched = 0, expectedCRC = 0, runningCRC = 0xffffffffU;
  char id[65] = {};
  bool mp3 = false;
  ~UploadBuffer() {
    clear();
  }
  void clear() {
    free(data);
    data = nullptr;
    size = used = 0;
    id[0] = 0;
  }
  bool matches(const char *sid) const {
    return data && sid && strcmp(id, sid) == 0;
  }
  bool begin(uint8_t *allocated, const char *sid, size_t bytes, uint32_t crc, bool audio,
             uint32_t e, uint32_t now) {
    if (!allocated || !sid || !sid[0] || strlen(sid) > 64 || !bytes) {
      free(allocated);
      return false;
    }
    clear();
    data = allocated;
    strcpy(id, sid);
    size = bytes;
    used = 0;
    mp3 = audio;
    epoch = e;
    touched = now;
    expectedCRC = crc;
    runningCRC = 0xffffffffU;
    data[0] = 0;
    return true;
  }
  static uint32_t updateCRC(uint32_t crc, const uint8_t *p, size_t n) {
    while (n--) {
      crc ^= *p++;
      for (unsigned b = 0; b < 8; b++)
        crc = (crc >> 1) ^ (0xedb88320U & uint32_t(-int32_t(crc & 1)));
    }
    return crc;
  }
  bool append(const char *sid, size_t offset, const uint8_t *p, size_t n, uint32_t e,
              uint32_t now) {
    if (!matches(sid) || epoch != e || !p || !n || offset > used || offset > size ||
        n > size - offset)
      return false;
    // A retry may repeat acknowledged bytes, but must not change their contents.
    if (offset < used) {
      if (n > used - offset || memcmp(data + offset, p, n) != 0)
        return false;
    } else {
      memcpy(data + used, p, n);
      used += n;
      runningCRC = updateCRC(runningCRC, p, n);
      data[used] = 0;
    }
    touched = now;
    return true;
  }
  // Accept only a full payload with the expected CRC; CRC is not authentication.
  bool complete() const {
    return data && used == size && (runningCRC ^ 0xffffffffU) == expectedCRC;
  }
  uint8_t *release() {
    uint8_t *p = data;
    data = nullptr;
    clear();
    return p;
  }
};

static inline bool mp3Offset(const uint8_t *p, size_t n, size_t &offset) {
  offset = 0;
  if (n >= 3 && memcmp(p, "ID3", 3) == 0) {
    if (n < 10 || (p[3] != 2 && p[3] != 3 && p[3] != 4))
      return false;
    for (unsigned i = 6; i < 10; i++)
      if (p[i] & 0x80)
        return false;
    size_t tag = (size_t(p[6]) << 21) | (size_t(p[7]) << 14) | (size_t(p[8]) << 7) | p[9];
    size_t footer = (p[3] == 4 && (p[5] & 0x10)) ? 10 : 0;
    if (tag > n - 10 || footer > n - 10 - tag)
      return false;
    offset = 10 + tag + footer;
  }
  return offset < n;
}
