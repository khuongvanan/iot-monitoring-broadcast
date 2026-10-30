#pragma once
#include <Arduino.h>

constexpr int MODEM_TX = 4, MODEM_RX = 5;
constexpr uint32_t MODEM_READY_TIMEOUT_MS = 30000;
constexpr uint32_t MODEM_AT_RETRY_MS = 500;

constexpr int SHT_SDA = 8, SHT_SCL = 9, FIRE_PIN = 7;
constexpr uint8_t SHT_ADDRESS = 0x44;

constexpr int FIRE_ACTIVE_LEVEL = LOW;
constexpr int AUDIO_MCLK = 15, AUDIO_BCLK = 16, AUDIO_LRCK = 17, AUDIO_DOUT = 18;
constexpr char APN[] = "v-internet";
#include "secrets.h"
constexpr uint32_t TELEMETRY_MS = 60000;
constexpr uint32_t CONFIG_POLL_MS = 60000;

constexpr uint32_t RPC_LONG_POLL_MS = 1000;
constexpr uint32_t RPC_GAP_MS = 200;
constexpr uint32_t DEFAULT_ANNOUNCE_SECONDS = 300;
constexpr uint32_t FIRE_CONFIRM_MS = 100, FIRE_CLEAR_MS = 5000;
constexpr int DEFAULT_VOLUME_PERMILLE = 1000;
constexpr int MAX_VOLUME_PERMILLE = 1000;
constexpr int SIREN_AMPLITUDE = 2000;
constexpr size_t MAX_MP3_BYTES = 96 * 1024;
constexpr size_t MAX_TEXT_BYTES = 128 * 1024 + 1;
constexpr unsigned MAX_CACHE_FILES = 12;
constexpr char FIRE_PHRASE[] = "Cảnh báo! Phát hiện lửa. Hãy rời khỏi khu vực nguy hiểm.";

constexpr bool FORMAT_LITTLEFS_ON_FAILURE = false;

constexpr size_t MAX_TEXT_INTERNAL_BYTES = 16 * 1024;
constexpr size_t MAX_FILE_PSRAM_BYTES = 2 * 1024 * 1024;
constexpr size_t MAX_FILE_INTERNAL_BYTES = 64 * 1024;
constexpr size_t UPLOAD_CHUNK_BYTES = 1536;
