#include <Arduino.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <atomic>
#include <math.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include "upload_buffer.h"
#include "config.h"
#include "audio_resampler.h"
#include "modem_http.h"
#include "fire_alert.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#include "minimp3.h"

static ModemHTTP net;
static I2SClass audio;
static Preferences prefs;
static SemaphoreHandle_t fsLock;
static bool fsOK = false;
static QueueHandle_t textQueue, audioQueue, downloadQueue, resultQueue;
static std::atomic<bool> modemOnline{false};
static std::atomic<uint32_t> pendingDownload{0};
static std::atomic<bool> fireAlarm{false}, audioOK{false};
static std::atomic<uint32_t> generation{1}, announceSeconds{DEFAULT_ANNOUNCE_SECONDS};
static std::atomic<int> speechVolume{DEFAULT_VOLUME_PERMILLE}, lastTTSHTTP{0};
static std::atomic<uint32_t> ttsDownloads{0}, ttsFailures{0}, speechPlayed{0};
static portMUX_TYPE sensorMux = portMUX_INITIALIZER_UNLOCKED;
struct SensorState {
  float temperature = NAN, humidity = NAN;
  bool shtOK = false, fireRaw = false;
  uint32_t lastGood = 0, fireEvents = 0, lastFire = 0;
};
static SensorState sensors;
struct TextJob {
  char *text;
  uint32_t epoch;
};
struct AudioJob {
  uint8_t *data;
  size_t length;
  uint32_t epoch;
  bool media = false;
};
static std::atomic<bool> mediaActive{false}, uploadBusy{false};
static std::atomic<unsigned> playbackState{0};
static UploadBuffer transfer;
static String committedID;
static uint32_t committedEpoch = 0;
static void clearUpload() {
  transfer.clear();
  uploadBusy.store(false);
}

static size_t uploadLimit(bool mp3) {
  // Use a contiguous block and leave RAM available for audio, JSON and modem tasks.
  size_t ps = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  size_t cap = mp3 ? MAX_FILE_PSRAM_BYTES : MAX_TEXT_BYTES - 1;
  if (ps > 128 * 1024)
    return min(cap, ps - 128 * 1024 - 1);
  size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t freeRAM = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (block <= 8192 || freeRAM <= 160 * 1024)
    return 0;
  cap = mp3 ? MAX_FILE_INTERNAL_BYTES : MAX_TEXT_INTERNAL_BYTES;
  return min(cap, min(block - 8192, freeRAM - 160 * 1024));
}
static uint8_t *uploadAlloc(size_t bytes) {
  size_t ps = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ps > 128 * 1024)
    return (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return (uint8_t *)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
struct DownloadJob {
  char text[600];
  uint32_t epoch, id;
};
struct DownloadResult {
  uint8_t *data;
  size_t length;
  uint32_t id;
};
static String firePath;
static int audioRate = 0;
static mp3dec_t decoder;
static int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];

static SensorState snapshot() {
  portENTER_CRITICAL(&sensorMux);
  SensorState s = sensors;
  portEXIT_CRITICAL(&sensorMux);
  return s;
}
static bool elapsed(uint32_t now, uint32_t then, uint32_t interval) {
  return now - then >= interval;
}
static String jsonString(JsonDocument &doc) {
  String s;
  serializeJson(doc, s);
  return s;
}
static String encodeURL(const String &s) {
  const char h[] = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); ++i) {
    uint8_t c = s[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.')
      out += (char)c;
    else {
      out += '%';
      out += h[c >> 4];
      out += h[c & 15];
    }
  }
  return out;
}
static String cachePath(const String &text) {
  uint8_t hash[32];
  if (mbedtls_sha256((const uint8_t *)text.c_str(), text.length(), hash, 0) != 0)
    return "";
  char hex[65];
  for (int i = 0; i < 32; i++)
    snprintf(hex + i * 2, 3, "%02x", hash[i]);
  return String("/tts/") + hex + ".mp3";
}
static uint8_t *loadCache(const String &path, size_t &length) {
  length = 0;
  if (!fsOK || path.isEmpty())
    return nullptr;
  xSemaphoreTake(fsLock, portMAX_DELAY);
  File f = LittleFS.open(path, "r");
  uint8_t *b = nullptr;
  if (f && f.size() > 0 && f.size() <= MAX_MP3_BYTES) {
    size_t n = f.size();
    b = audioAlloc(n);
    if (b) {
      if (f.read(b, n) == n)
        length = n;
      else {
        free(b);
        b = nullptr;
      }
    }
  }
  f.close();
  xSemaphoreGive(fsLock);
  return b;
}
static void saveCache(const String &path, const uint8_t *data, size_t length) {
  if (!fsOK || path.isEmpty())
    return;
  xSemaphoreTake(fsLock, portMAX_DELAY);

  unsigned count = 0;
  String victim;
  File dir = LittleFS.open("/tts");
  if (dir) {
    File f = dir.openNextFile();
    while (f) {
      String p = f.path();
      if (!f.isDirectory() && p.endsWith(".mp3")) {
        ++count;
        if (victim.isEmpty() && p != firePath && p != path)
          victim = p;
      }
      f.close();
      f = dir.openNextFile();
    }
    dir.close();
  }
  if (count >= MAX_CACHE_FILES && !victim.isEmpty())
    LittleFS.remove(victim);
  String tmp = path + ".tmp";
  File f = LittleFS.open(tmp, "w");
  bool ok = f && f.write(data, length) == length;
  f.close();
  if (ok) {
    LittleFS.remove(path);
    ok = LittleFS.rename(tmp, path);
  }
  if (!ok) {
    LittleFS.remove(tmp);
    Serial.println("[CACHE] Write failed; RAM playback remains available.");
  }
  xSemaphoreGive(fsLock);
}
static void deleteCache(const String &path) {
  if (!fsOK)
    return;
  xSemaphoreTake(fsLock, portMAX_DELAY);
  LittleFS.remove(path);
  xSemaphoreGive(fsLock);
}

static bool validMP3(const uint8_t *b, size_t n) {
  size_t offset = 0;
  if (!mp3Offset(b, n, offset))
    return false;
  b += offset;
  n -= offset;
  mp3dec_t d;
  mp3dec_init(&d);
  mp3dec_frame_info_t info = {};
  int16_t out[MINIMP3_MAX_SAMPLES_PER_FRAME];
  int k = mp3dec_decode_frame(&d, b, (int)n, out, &info);
  return k > 0 && info.frame_bytes > 0 && (info.channels == 1 || info.channels == 2) &&
         info.hz >= 8000 && info.hz <= 48000;
}
static bool queueText(const String &s) {
  String text = s;
  text.trim();
  if (fireAlarm.load() || mediaActive.load() || uploadBusy.load() || text.isEmpty() ||
      text.length() >= MAX_TEXT_BYTES)
    return false;
  TextJob j{(char *)audioAlloc(text.length() + 1), generation.load()};
  if (!j.text)
    return false;
  memcpy(j.text, text.c_str(), text.length() + 1);
  if (xQueueSend(textQueue, &j, 0) == pdTRUE)
    return true;
  free(j.text);
  return false;
}
static String environmentText() {
  SensorState s = snapshot();
  if (!s.shtOK || millis() - s.lastGood > 10000)
    return "";
  String t = String(s.temperature, 1);
  t.replace(".", " phẩy ");
  String h = String(s.humidity, 0);
  return "Nhiệt độ hiện tại là " + t + " độ C. Độ ẩm là " + h + " phần trăm.";
}
static uint8_t crc8(const uint8_t *p, size_t n) {
  uint8_t c = 0xff;
  while (n--) {
    c ^= *p++;
    for (int b = 0; b < 8; b++)
      c = (c & 0x80) ? (uint8_t)((c << 1) ^ 0x31) : (uint8_t)(c << 1);
  }
  return c;
}
static void sensorTask(void *) {
  Wire.begin(SHT_SDA, SHT_SCL, 100000);
  Wire.setTimeOut(20);
  pinMode(FIRE_PIN, INPUT_PULLUP);
  bool activeTracking = false, clearTracking = false, pending = false;
  uint32_t activeAt = 0, clearAt = 0, lastMeasure = millis() - 2000, measureAt = 0;
  TickType_t tick = xTaskGetTickCount();
  for (;;) {
    uint32_t now = millis();
    bool raw = digitalRead(FIRE_PIN) == FIRE_ACTIVE_LEVEL;
    portENTER_CRITICAL(&sensorMux);
    sensors.fireRaw = raw;
    portEXIT_CRITICAL(&sensorMux);
    // Require a stable input; use a longer clear delay to avoid alarm chatter.
    if (raw) {
      clearTracking = false;
      if (!activeTracking) {
        activeTracking = true;
        activeAt = now;
      }
      if (!fireAlarm.load() && elapsed(now, activeAt, FIRE_CONFIRM_MS)) {
        generation.fetch_add(1);
        fireAlarm.store(true);
        portENTER_CRITICAL(&sensorMux);
        sensors.fireEvents++;
        sensors.lastFire = now;
        portEXIT_CRITICAL(&sensorMux);
        Serial.println("[FIRE] ON - local audio priority");
      }
    } else {
      activeTracking = false;
      if (!clearTracking) {
        clearTracking = true;
        clearAt = now;
      }
      if (fireAlarm.load() && elapsed(now, clearAt, FIRE_CLEAR_MS)) {
        fireAlarm.store(false);
        Serial.println("[FIRE] Clear after stable inactive input");
      }
    }
    if (!pending && elapsed(now, lastMeasure, 2000)) {
      lastMeasure = now;

      Wire.beginTransmission(SHT_ADDRESS);
      Wire.write(0x24);
      Wire.write(0x00);
      if (Wire.endTransmission() == 0) {
        pending = true;
        measureAt = now;
      } else {
        portENTER_CRITICAL(&sensorMux);
        sensors.shtOK = false;
        portEXIT_CRITICAL(&sensorMux);
      }
    }
    if (pending && elapsed(now, measureAt, 20)) {
      pending = false;
      uint8_t b[6];
      bool ok = Wire.requestFrom(SHT_ADDRESS, (uint8_t)6) == 6;
      if (ok)
        for (int i = 0; i < 6; i++)
          b[i] = Wire.read();
      while (Wire.available())
        Wire.read();
      ok = ok && crc8(b, 2) == b[2] && crc8(b + 3, 2) == b[5];
      float t = NAN, h = NAN;
      if (ok) {
        t = -45.0f + 175.0f * ((uint16_t(b[0]) << 8) | b[1]) / 65535.0f;
        h = 100.0f * ((uint16_t(b[3]) << 8) | b[4]) / 65535.0f;
      }
      portENTER_CRITICAL(&sensorMux);
      sensors.shtOK = ok;
      if (ok) {
        sensors.temperature = t;
        sensors.humidity = h;
        sensors.lastGood = now;
      }
      portEXIT_CRITICAL(&sensorMux);
    }
    vTaskDelayUntil(&tick, pdMS_TO_TICKS(10));
  }
}

static bool beginAudio(bool reset = false) {
  if (reset || (!audioOK.load() && audioRate)) {
    audio.end();
    audioRate = 0;
    audioOK.store(false);
  }
  if (audioOK.load())
    return true;
  audio.setPins(AUDIO_BCLK, AUDIO_LRCK, AUDIO_DOUT, -1, AUDIO_MCLK);
  bool ok = audio.begin(I2S_MODE_STD, AudioResampler48k::outputRate, I2S_DATA_BIT_WIDTH_16BIT,
                        I2S_SLOT_MODE_STEREO);
  audioOK.store(ok);
  if (ok) {
    audioRate = AudioResampler48k::outputRate;
    Serial.println("[AUDIO] I2S 48000 Hz / 16-bit / stereo; MCLK enabled");
  } else
    Serial.println("[AUDIO] I2S initialization failed");
  return ok;
}
static bool interrupted(uint32_t epoch, bool alarmVoice) {
  return alarmVoice ? !fireAlarm.load() : (fireAlarm.load() || generation.load() != epoch);
}
static bool writePCM(const int16_t *p, size_t frames, uint32_t epoch, bool alarmVoice,
                     bool check = true) {
  while (frames) {
    if (check && interrupted(epoch, alarmVoice))
      return false;
    size_t n = min((size_t)128, frames), written = 0;

    esp_err_t err = i2s_channel_write(audio.txChan(), p, n * 4, &written, 100);
    if (err != ESP_OK || written != n * 4) {
      audioOK.store(false);
      return false;
    }
    p += n * 2;
    frames -= n;
  }
  return true;
}
static bool playMP3(const uint8_t *data, size_t length, uint32_t epoch, bool alarmVoice,
                    bool media = false) {
  if (!beginAudio())
    return false;
  mp3dec_init(&decoder);
  AudioResampler48k resampler;
  size_t pos = 0, used = 0;
  if (!mp3Offset(data, length, pos))
    return false;
  int decodedFrames = 0, sourceRate = 0;
  int16_t block[256], lastLeft = 0, lastRight = 0;

  constexpr int rampFrames = AudioResampler48k::outputRate / 200;
  int32_t gain = 0, startGain = 0, targetGain = 0;
  int rampPosition = rampFrames;
  auto flush = [&]() -> bool {
    if (!used)
      return true;
    bool ok = writePCM(block, used, epoch, alarmVoice);
    used = 0;
    return ok;
  };
  auto emitRaw = [&](int16_t l, int16_t r) -> bool {
    block[used * 2] = l;
    block[used * 2 + 1] = r;
    ++used;
    return used < 128 || flush();
  };
  auto emit = [&](int16_t l, int16_t r) -> bool {
    if (rampPosition < rampFrames) {
      ++rampPosition;
      gain = startGain + (int64_t(targetGain - startGain) * rampPosition) / rampFrames;
    }
    lastLeft = int16_t((int64_t(l) * gain) / (1000 * 65536LL));
    lastRight = int16_t((int64_t(r) * gain) / (1000 * 65536LL));
    return emitRaw(lastLeft, lastRight);
  };
  while (pos < length) {
    if (interrupted(epoch, alarmVoice))
      return false;
    mp3dec_frame_info_t info = {};
    int n = mp3dec_decode_frame(&decoder, data + pos, (int)(length - pos), pcm, &info);
    if (info.frame_bytes <= 0)
      break;
    pos += info.frame_bytes;
    if (!n)
      continue;
    if ((info.channels != 1 && info.channels != 2) || info.hz < 8000 || info.hz > 48000)
      return false;
    if (!sourceRate) {
      sourceRate = info.hz;
      if (!resampler.begin(sourceRate))
        return false;
      Serial.printf("[AUDIO] MP3 %d Hz / %d ch -> 48000 Hz / dual mono\n", info.hz, info.channels);
    } else if (info.hz != sourceRate) {
      Serial.println("[AUDIO] Rejected MP3 sample-rate change inside one file");
      return false;
    }
    int volume = alarmVoice ? max(30, speechVolume.load()) : speechVolume.load();
    volume = max(0, min(MAX_VOLUME_PERMILLE, volume));
    int32_t requestedGain = volume * 65536;
    if (requestedGain != targetGain) {
      startGain = gain;
      targetGain = requestedGain;
      rampPosition = 0;
    }

    if (!resampler.push(pcm, n, info.channels, true, emit))
      return false;
    ++decodedFrames;
    delay(1);
  }
  if (!decodedFrames || !resampler.finish(emit))
    return false;

  const int16_t tailLeft = lastLeft, tailRight = lastRight;
  for (int i = 1; i <= rampFrames; ++i) {
    if (!emitRaw(int32_t(tailLeft) * (rampFrames - i) / rampFrames,
                 int32_t(tailRight) * (rampFrames - i) / rampFrames))
      return false;
  }
  if (!flush())
    return false;
  int16_t zero[256] = {};

  for (int left = media ? 0 : audioRate / 2; left > 0;) {
    int n = min(128, left);
    if (!writePCM(zero, n, epoch, alarmVoice))
      return false;
    left -= n;
  }
  return true;
}
static void audioTask(void *) {
  bool wasAlarm = false;
  uint32_t nextVoice = 0, audioEpoch = generation.load();
  float phase = 0;
  beginAudio();
  for (;;) {
    bool alarm = fireAlarm.load();
    if (alarm != wasAlarm || audioEpoch != generation.load()) {
      audioEpoch = generation.load();
      beginAudio(true);
      wasAlarm = alarm;
      nextVoice = millis() + 2000;
      AudioJob stale;
      unsigned queued = uxQueueMessagesWaiting(audioQueue);
      while (queued-- && xQueueReceive(audioQueue, &stale, 0) == pdTRUE) {
        if (!alarm && stale.epoch == audioEpoch) {
          if (xQueueSend(audioQueue, &stale, 0) != pdTRUE)
            free(stale.data);
        } else
          free(stale.data);
      }
    }
    if (!audioOK.load()) {
      delay(200);
      beginAudio(true);
      continue;
    }
    if (alarm) {
      if ((int32_t)(millis() - nextVoice) >= 0) {
        playMP3(FIRE_MP3, FIRE_MP3_LENGTH, 0, true);
        nextVoice = millis() + 5000;
      }
      if (!fireAlarm.load())
        continue;
      int16_t buf[256];
      float hz = ((millis() / 400) % 2) ? 1200.0f : 800.0f;
      for (int i = 0; i < 128; i++) {
        int16_t v = lroundf(sinf(phase) * SIREN_AMPLITUDE);
        buf[2 * i] = buf[2 * i + 1] = v;
        phase += 2.0f * PI * hz / float(AudioResampler48k::outputRate);
        if (phase >= 2 * PI)
          phase -= 2 * PI;
      }
      writePCM(buf, 128, 0, true);
      delay(1);
    } else {
      AudioJob j;
      if (xQueueReceive(audioQueue, &j, 0) == pdTRUE) {
        if (j.epoch == generation.load()) {
          if (j.media)
            playbackState.store(2);
          bool ok = playMP3(j.data, j.length, j.epoch, false, j.media);
          if (ok)
            speechPlayed.fetch_add(1);
          if (j.media && j.epoch == generation.load()) {
            playbackState.store(ok ? 3 : 4);
            mediaActive.store(false);
          }
          Serial.println(ok ? "[AUDIO] Speech done" : "[AUDIO] Speech stopped/error");
          if (!ok)
            beginAudio(true);
        }
        free(j.data);
      } else {
        int16_t zero[256] = {};
        writePCM(zero, 128, 0, false, false);
        delay(1);
      }
    }
  }
}

static String baseURL() {
  return String(TB_HOST) + "/api/v1/" + TB_TOKEN;
}
static bool tbConfigured() {
  return strlen(TB_TOKEN) > 0;
}
static int postJSON(const String &endpoint, const String &json) {
  size_t n = 0;
  return net.request(baseURL() + endpoint, &json, nullptr, n);
}
static bool getJSON(const String &endpoint, JsonDocument &doc) {
  size_t n = 0;
  uint8_t *b = nullptr;
  int code = net.request(baseURL() + endpoint, nullptr, &b, n, 8192);
  bool ok = code == 200 && b && deserializeJson(doc, b, n) == DeserializationError::Ok;
  free(b);
  return ok;
}
static void setSettings(JsonVariantConst cfg) {
  if (cfg["volume_permille"].is<int>()) {
    int v = cfg["volume_permille"].as<int>();
    if (v >= 0 && v <= MAX_VOLUME_PERMILLE && v != speechVolume.load()) {
      speechVolume.store(v);
    }
  }
  if (cfg["announce_interval_s"].is<uint32_t>()) {
    uint32_t v = cfg["announce_interval_s"].as<uint32_t>();
    if ((v == 0 || (v >= 30 && v <= 86400)) && v != announceSeconds.load()) {
      announceSeconds.store(v);
      prefs.putUInt("announce", v);
    }
  }
}
static bool publishTelemetry(uint32_t okCount, uint32_t failCount) {
  SensorState s = snapshot();
  JsonDocument d;
  if (s.shtOK && millis() - s.lastGood < 10000) {
    d["temperature"] = s.temperature;
    d["humidity"] = s.humidity;
  }
  d["sht_ok"] = s.shtOK && millis() - s.lastGood < 10000;
  d["fire"] = fireAlarm.load();
  d["fire_raw"] = s.fireRaw;
  d["fire_event_count"] = s.fireEvents;
  d["last_fire_uptime_s"] = s.lastFire / 1000;
  d["uptime_s"] = millis() / 1000;
  d["audio_ok"] = audioOK.load();
  d["volume_percent"] = speechVolume.load() / 10;
  d["media_active"] = mediaActive.load();
  d["playback_state"] = playbackState.load();
  d["volume_permille"] = speechVolume.load();
  d["announce_interval_s"] = announceSeconds.load();
  d["send_ok"] = okCount;
  d["send_fail"] = failCount;
  d["tts_last_http"] = lastTTSHTTP.load();
  d["tts_downloads"] = ttsDownloads.load();
  d["tts_failures"] = ttsFailures.load();
  d["speech_played"] = speechPlayed.load();
  String r = net.query("AT+CSQ");
  int v = -1, ber = -1, p = r.indexOf("+CSQ:");
  if (p >= 0 && sscanf(r.c_str() + p, "+CSQ: %d,%d", &v, &ber) == 2 && v >= 0 && v <= 31)
    d["csq"] = v;

  d["cpsi_raw"] = net.query("AT+CPSI?");
  d["cbc_raw"] = net.query("AT+CBC");
  String j = jsonString(d);
  int code = postJSON("/telemetry", j);
  Serial.printf("[TB] Telemetry HTTP %d; T=%.1f H=%.1f fire=%d\n", code, s.temperature, s.humidity,
                (int)fireAlarm.load());
  return code >= 200 && code < 300;
}
static void attributes() {
  JsonDocument d;
  d["sensor_mode"] = "real";
  d["firmware"] = "school-node-text-mp3-v2";
  d["modem_info"] = net.query("ATI");
  d["imei_raw"] = net.query("AT+CGSN");
  d["iccid_raw"] = net.query("AT+CICCID");
  d["operator_raw"] = net.query("AT+COPS?");
  d["ip_raw"] = net.query("AT+CGPADDR=1");
  d["flame_active_level"] = FIRE_ACTIVE_LEVEL == LOW ? "LOW" : "HIGH";
  Serial.printf("[TB] Attributes HTTP %d\n", postJSON("/attributes", jsonString(d)));
}
static void pollRPC() {
  JsonDocument doc;
  if (!getJSON("/rpc?timeout=" + String(RPC_LONG_POLL_MS), doc))
    return;
  if (!doc["id"].is<int>() || !doc["method"].is<const char *>())
    return;
  int id = doc["id"].as<int>();
  String method = doc["method"].as<String>();
  JsonVariantConst p = doc["params"];
  JsonDocument reply;
  if (method == "speak") {
    String t;
    if (p.is<const char *>())
      t = p.as<String>();
    else if (p["text"].is<const char *>())
      t = p["text"].as<String>();
    bool ok = queueText(t);
    reply["accepted"] = ok;
    reply["status"] = ok ? "queued" : "rejected_fire_busy_or_invalid_text";
  } else if (method == "announce") {
    bool ok = queueText(environmentText());
    reply["accepted"] = ok;
  } else if (method == "stop") {
    generation.fetch_add(1);
    mediaActive.store(false);
    playbackState.store(0);
    clearUpload();
    reply["stopped"] = true;
    reply["fire_alarm_active"] = fireAlarm.load();
  } else if (method == "setConfig") {
    bool volumeOK = p["volume_permille"].isNull() ||
                    (p["volume_permille"].is<int>() && p["volume_permille"].as<int>() >= 0 &&
                     p["volume_permille"].as<int>() <= MAX_VOLUME_PERMILLE);
    uint32_t a = p["announce_interval_s"] | 0U;
    bool periodOK = p["announce_interval_s"].isNull() || (p["announce_interval_s"].is<uint32_t>() &&
                                                          (a == 0 || (a >= 30 && a <= 86400)));
    bool ok = p.is<JsonObjectConst>() && volumeOK && periodOK;
    if (ok)
      setSettings(p);
    reply["accepted"] = ok;
    reply["volume_permille"] = speechVolume.load();
  } else if (method == "getVolume") {
    reply.set(speechVolume.load() / 10);
  } else if (method == "setVolume") {
    bool ok = p.is<int>() && p.as<int>() >= 0 && p.as<int>() <= 100;
    if (ok) {
      speechVolume.store(p.as<int>() * 10);
      reply.set(p.as<int>());
    } else
      reply["error"] = "volume_must_be_integer_0_to_100";
  } else if (method == "beginUpload") {
    String sid = p["id"] | "", kind = p["kind"] | "";
    size_t bytes = p["bytes"] | 0U;
    uint32_t crc = p["crc32"] | 0U;
    bool isMP3 = kind == "mp3";
    bool ok = !fireAlarm.load() && (kind == "text" || isMP3) && sid.length() > 0 &&
              sid.length() <= 64 && bytes > 0;
    if (ok && transfer.matches(sid.c_str())) {
      ok = transfer.size == bytes && transfer.expectedCRC == crc && transfer.mp3 == isMP3 &&
           transfer.epoch == generation.load();
      if (ok)
        transfer.touched = millis();
    } else if (ok) {

      ok = !transfer.data && !mediaActive.load() && bytes <= uploadLimit(isMP3);
      if (ok) {
        uint8_t *data = uploadAlloc(bytes + 1);
        if (data) {

          generation.fetch_add(1);
          playbackState.store(0);
          uploadBusy.store(true);
          ok = transfer.begin(data, sid.c_str(), bytes, crc, isMP3, generation.load(), millis());
        } else
          ok = false;
      }
    }
    reply["accepted"] = ok;
    reply["offset"] = ok ? transfer.used : 0;
    reply["status"] = ok ? "receiving" : "busy_fire_or_not_enough_memory";
  } else if (method == "uploadChunk") {
    String sid = p["id"] | "";
    const char *encoded = p["data"] | "";
    size_t offset = p["offset"] | SIZE_MAX, decoded = 0;
    uint8_t raw[UPLOAD_CHUNK_BYTES];
    size_t encodedSize = strlen(encoded);
    bool ok =
        !fireAlarm.load() && encodedSize > 0 && encodedSize <= ((UPLOAD_CHUNK_BYTES + 2) / 3) * 4;
    if (ok)
      ok = mbedtls_base64_decode(raw, sizeof(raw), &decoded, (const unsigned char *)encoded,
                                 encodedSize) == 0;
    if (ok)
      ok = transfer.append(sid.c_str(), offset, raw, decoded, generation.load(), millis());
    reply["accepted"] = ok;
    reply["offset"] = transfer.used;
    reply["status"] = ok ? "receiving" : "invalid_session_offset_or_data";
  } else if (method == "cancelUpload") {
    String sid = p["id"] | "";
    bool ok = !transfer.data || transfer.matches(sid.c_str());
    if (ok)
      clearUpload();
    reply["accepted"] = ok;
  } else if (method == "commitUpload") {
    String sid = p["id"] | "";
    bool ok = !sid.isEmpty() && sid == committedID && committedEpoch == generation.load();
    const char *status = "incomplete_crc_or_busy";
    if (!ok && transfer.matches(sid.c_str()) && transfer.epoch == generation.load() &&
        transfer.complete() && !fireAlarm.load()) {
      if (transfer.mp3) {
        if (validMP3(transfer.data, transfer.size)) {
          AudioJob job{transfer.data, transfer.size, transfer.epoch, true};
          mediaActive.store(true);
          playbackState.store(1);
          ok = xQueueSend(audioQueue, &job, 0) == pdTRUE;
          if (!ok) {
            mediaActive.store(false);
            playbackState.store(0);
            status = "audio_queue_busy_retry_commit";
          }
        } else {
          status = "invalid_or_unsupported_mp3";
          clearUpload();
        }
      } else {
        if (memchr(transfer.data, 0, transfer.size) == nullptr) {
          TextJob job{(char *)transfer.data, transfer.epoch};
          ok = xQueueSend(textQueue, &job, 0) == pdTRUE;
          if (!ok)
            status = "text_queue_busy_retry_commit";
        } else {
          status = "text_contains_nul";
          clearUpload();
        }
      }
      if (ok) {
        transfer.release();
        uploadBusy.store(false);
        committedID = sid;
        committedEpoch = transfer.epoch;
      }
    }
    reply["accepted"] = ok;
    reply["status"] = ok ? "queued" : status;
  } else if (method == "getStatus") {
    reply["media_active"] = mediaActive.load();
    reply["playback_state"] = playbackState.load();
    reply["text_max_bytes"] = uploadLimit(false);
    reply["mp3_max_bytes"] = uploadLimit(true);
    reply["upload_chunk_bytes"] = UPLOAD_CHUNK_BYTES;
    reply["upload_active"] = transfer.data != nullptr;
    reply["upload_received"] = transfer.used;
    reply["upload_total"] = transfer.size;
    reply["tts_last_http"] = lastTTSHTTP.load();
    reply["psram_bytes"] = ESP.getPsramSize();
    reply["firmware"] = "school-node-text-mp3-v2";
    SensorState s = snapshot();
    reply["fire"] = fireAlarm.load();
    reply["sht_ok"] = s.shtOK;
    reply["audio_ok"] = audioOK.load();
    reply["volume_permille"] = speechVolume.load();
    reply["announce_interval_s"] = announceSeconds.load();
    if (s.shtOK) {
      reply["temperature"] = s.temperature;
      reply["humidity"] = s.humidity;
    }
  } else {
    reply["error"] = "unknown_method";
  }
  int code = postJSON("/rpc/" + String(id), jsonString(reply));
  Serial.printf("[RPC] %s reply HTTP %d\n", method.c_str(), code);
}
static uint32_t lastTtsFail = 0;
static bool ttsHasFailed = false;
static uint8_t *speechBytes(const String &text, size_t &len) {
  String path = cachePath(text);
  uint8_t *b = loadCache(path, len);
  if (b) {
    if (validMP3(b, len))
      return b;
    free(b);
    deleteCache(path);
    b = nullptr;
  }
  if (ttsHasFailed && !elapsed(millis(), lastTtsFail, 60000))
    return nullptr;
  String url =
      "https://translate.google.com/translate_tts?ie=UTF-8&client=tw-ob&tl=vi&q=" + encodeURL(text);
  int code = net.request(url, nullptr, &b, len, MAX_MP3_BYTES, true);
  lastTTSHTTP.store(code);
  Serial.printf("[TTS] HTTP %d, bytes=%u\n", code, (unsigned)len);
  if (code != 200 || !b || !validMP3(b, len)) {
    ttsFailures.fetch_add(1);
    free(b);
    len = 0;
    ttsHasFailed = true;
    lastTtsFail = millis();
    return nullptr;
  }
  ttsDownloads.fetch_add(1);
  ttsHasFailed = false;
  saveCache(path, b, len);
  return b;
}

static String nextChunk(const char *text, size_t length, size_t &pos) {
  while (pos < length &&
         (text[pos] == ' ' || text[pos] == '\n' || text[pos] == '\r' || text[pos] == '\t'))
    ++pos;
  size_t begin = pos, lastBreak = pos;
  unsigned chars = 0;
  while (pos < length && chars < 140) {
    uint8_t c = text[pos];
    size_t width = c < 0x80 ? 1 : (c < 0xe0 ? 2 : (c < 0xf0 ? 3 : 4));
    if (pos + width > length) {
      pos = length;
      break;
    }
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
      lastBreak = pos;
    pos += width;
    ++chars;
  }
  if (pos < length && lastBreak > begin)
    pos = lastBreak;
  String result;
  result.reserve(pos - begin);
  for (size_t i = begin; i < pos; ++i)
    result += text[i];
  result.trim();
  return result;
}

static uint8_t *obtainSpeech(const String &text, size_t &len, uint32_t epoch) {
  String path = cachePath(text);
  uint8_t *b = loadCache(path, len);
  if (b) {
    if (validMP3(b, len))
      return b;
    free(b);
    deleteCache(path);
  }
  len = 0;
  if (!modemOnline.load() || fireAlarm.load() || text.length() >= 600)
    return nullptr;
  static uint32_t sequence = 0;
  DownloadJob request = {};
  text.toCharArray(request.text, sizeof(request.text));
  request.epoch = epoch;
  request.id = ++sequence;
  DownloadResult old;
  while (xQueueReceive(resultQueue, &old, 0) == pdTRUE)
    free(old.data);
  pendingDownload.store(request.id);
  if (xQueueSend(downloadQueue, &request, 0) != pdTRUE) {
    pendingDownload.store(0);
    return nullptr;
  }
  uint32_t t = millis();
  while (millis() - t < 180000 && !fireAlarm.load() && generation.load() == epoch) {
    DownloadResult r;
    if (xQueueReceive(resultQueue, &r, pdMS_TO_TICKS(100)) == pdTRUE) {
      if (r.id == request.id) {
        pendingDownload.store(0);
        len = r.length;
        return r.data;
      }
      free(r.data);
    }
  }
  pendingDownload.store(0);
  return nullptr;
}
static bool processSpeech(const TextJob &job) {
  const char *text = job.text;
  size_t length = strlen(text), pos = 0;
  while (pos < length) {
    if (fireAlarm.load() || job.epoch != generation.load())
      return false;
    String part = nextChunk(text, length, pos);
    if (part.isEmpty())
      continue;
    size_t len = 0;
    uint8_t *b = obtainSpeech(part, len, job.epoch);
    if (!b)
      return false;
    if (fireAlarm.load() || job.epoch != generation.load()) {
      free(b);
      return false;
    }
    AudioJob a{b, len, job.epoch};
    bool queued = false;
    uint32_t t = millis();
    while (!fireAlarm.load() && job.epoch == generation.load()) {
      if (xQueueSend(audioQueue, &a, pdMS_TO_TICKS(100)) == pdTRUE) {
        queued = true;
        break;
      }
    }
    if (!queued) {
      free(b);
      return false;
    }
  }
  return true;
}
static void speechTask(void *) {
  bool startupQueued = false;
  uint32_t lastAnnounce = 0, lastAttempt = millis() - 60000;
  for (;;) {
    uint32_t interval = announceSeconds.load() * 1000U;
    bool due = !startupQueued || (interval && elapsed(millis(), lastAnnounce, interval));
    if (!fireAlarm.load() && !mediaActive.load() && !uploadBusy.load() && due &&
        elapsed(millis(), lastAttempt, 60000)) {
      String text = environmentText();
      if (!text.isEmpty()) {
        lastAttempt = millis();
        TextJob job{const_cast<char *>(text.c_str()), generation.load()};
        if (processSpeech(job)) {
          startupQueued = true;
          lastAnnounce = millis();
          lastAttempt = millis() - 60000;
        }
      }
    }
    TextJob job;
    if (xQueueReceive(textQueue, &job, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (job.epoch == generation.load() && !fireAlarm.load() && !mediaActive.load())
        if (!processSpeech(job))
          Serial.println("[TTS] Speech unavailable/interrupted; check network/cache/log.");
      free(job.text);
    }
    delay(10);
  }
}
static void networkTask(void *) {
  net.begin();
  bool online = false;
  uint32_t lastConnect = millis() - 10000;
  uint32_t lastTele = millis() - TELEMETRY_MS, lastCfg = millis() - CONFIG_POLL_MS, lastRPC = 0,
           lastReg = 0;
  uint32_t good = 0, bad = 0;
  uint32_t lastEvents = 0;
  bool lastFire = false;
  bool tb = tbConfigured();
  if (!tb)
    Serial.println("[TB] TOKEN not configured. Sensors/alarm still run.");
  for (;;) {
    if (transfer.data &&
        (transfer.epoch != generation.load() || elapsed(millis(), transfer.touched, 180000)))
      clearUpload();
    if (fireAlarm.load())
      mediaActive.store(false);
    if (!online || net.needsReset) {
      modemOnline.store(false);
      if (elapsed(millis(), lastConnect, 10000)) {
        online = net.initialize();
        lastConnect = millis();
        Serial.println(online ? "[MODEM] Registered" : "[MODEM] Retry in 10 seconds");
        if (online) {
          modemOnline.store(true);
          lastReg = millis();
          if (tb)
            attributes();
        }
      }
      if (!online || net.needsReset) {
        delay(100);
        continue;
      }
    }
    if (elapsed(millis(), lastReg, 30000)) {
      lastReg = millis();
      online = net.registered();
      modemOnline.store(online);
      if (!online)
        continue;
    }
    SensorState s = snapshot();
    bool fire = fireAlarm.load();
    uint32_t now = millis();
    bool changed = fire != lastFire || s.fireEvents != lastEvents;
    if (tb && (elapsed(now, lastTele, TELEMETRY_MS) || (changed && elapsed(now, lastTele, 5000)))) {
      lastTele = now;
      if (publishTelemetry(good, bad)) {
        good++;
        lastFire = fire;
        lastEvents = s.fireEvents;
      } else
        bad++;
    }
    if (net.needsReset)
      continue;
    if (tb && elapsed(millis(), lastCfg, CONFIG_POLL_MS)) {
      lastCfg = millis();
      JsonDocument d;
      if (getJSON("/attributes?sharedKeys=announce_interval_s", d)) {
        JsonDocument cfg;
        cfg["announce_interval_s"] = d["shared"]["announce_interval_s"];
        setSettings(cfg.as<JsonVariantConst>());
      }
    }
    if (net.needsReset)
      continue;
    DownloadJob request;
    if (xQueueReceive(downloadQueue, &request, 0) == pdTRUE) {
      if (request.id == pendingDownload.load() && request.epoch == generation.load() &&
          !fireAlarm.load()) {
        size_t len = 0;
        uint8_t *data = speechBytes(request.text, len);
        if (request.id == pendingDownload.load() && request.epoch == generation.load() &&
            !fireAlarm.load()) {
          DownloadResult r{data, len, request.id};
          if (xQueueSend(resultQueue, &r, 0) != pdTRUE)
            free(data);
        } else
          free(data);
      }
    }
    if (net.needsReset)
      continue;
    if (tb && elapsed(millis(), lastRPC, RPC_GAP_MS)) {
      pollRPC();
      lastRPC = millis();
    }
    if (transfer.data &&
        (transfer.epoch != generation.load() || elapsed(millis(), transfer.touched, 180000)))
      clearUpload();
    if (fireAlarm.load())
      mediaActive.store(false);
    delay(20);
  }
}
void nodeSetup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("School Node text-mp3 v2 fixed-48k - real sensors / 4G / TTS / local flame alarm");
  fsLock = xSemaphoreCreateMutex();
  textQueue = xQueueCreate(2, sizeof(TextJob));
  audioQueue = xQueueCreate(2, sizeof(AudioJob));
  downloadQueue = xQueueCreate(1, sizeof(DownloadJob));
  resultQueue = xQueueCreate(1, sizeof(DownloadResult));
  if (!fsLock || !textQueue || !audioQueue || !downloadQueue || !resultQueue) {
    Serial.println("[FATAL] Queue/mutex allocation failed");
    return;
  }
  prefs.begin("school-node", false);
  uint32_t a = prefs.getUInt("announce", DEFAULT_ANNOUNCE_SECONDS);
  speechVolume.store(DEFAULT_VOLUME_PERMILLE);
  announceSeconds.store((a == 0 || (a >= 30 && a <= 86400)) ? a : DEFAULT_ANNOUNCE_SECONDS);
  fsOK = LittleFS.begin(FORMAT_LITTLEFS_ON_FAILURE);
  if (fsOK) {
    LittleFS.mkdir("/tts");
    Serial.printf("[CACHE] FS %u/%u bytes\n", (unsigned)LittleFS.usedBytes(),
                  (unsigned)LittleFS.totalBytes());
  } else
    Serial.println(
        "[CACHE] LittleFS not mounted; no auto-format. RAM TTS + local siren remain available.");
  firePath = cachePath(FIRE_PHRASE);
  if (xTaskCreatePinnedToCore(sensorTask, "sensors", 4096, nullptr, 4, nullptr, 1) != pdPASS) {
    Serial.println("[FATAL] Sensor task failed");
    return;
  }
  if (xTaskCreatePinnedToCore(audioTask, "audio", 32768, nullptr, 3, nullptr, 1) != pdPASS) {
    Serial.println("[FATAL] Audio task failed");
    return;
  }
  if (xTaskCreatePinnedToCore(speechTask, "speech", 32768, nullptr, 1, nullptr, 0) != pdPASS) {
    Serial.println("[ERROR] Speech task failed; local siren continues");
    return;
  }
  if (xTaskCreatePinnedToCore(networkTask, "network", 32768, nullptr, 1, nullptr, 0) != pdPASS)
    Serial.println("[ERROR] Network task failed; local sensors/alarm continue");
}
void nodeLoop() {
  if (!textQueue || !audioQueue || !downloadQueue || !resultQueue) {
    delay(1000);
    return;
  }
  static String input;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      input.trim();
      if (input == "stop") {
        generation.fetch_add(1);
        mediaActive.store(false);
        playbackState.store(0);
        Serial.println("[LOCAL] Stop speech (fire alarm unchanged)");
      } else if (input == "announce")
        Serial.println(queueText(environmentText()) ? "[LOCAL] Queued"
                                                    : "[LOCAL] Rejected/no sensor data");
      else if (input.startsWith("say "))
        Serial.println(queueText(input.substring(4)) ? "[LOCAL] Queued" : "[LOCAL] Rejected");
      else if (input.length())
        Serial.println("Commands: say <text> | announce | stop");
      input = "";
    } else if (c != '\r') {
      if (input.length() < 4096)
        input += c;
      else
        input = "";
    }
  }

  static uint32_t last = 0;
  if (elapsed(millis(), last, 5000)) {
    last = millis();
    SensorState s = snapshot();
    Serial.printf("[LOCAL] SHT=%s T=%.1f H=%.1f raw=%d alarm=%d audio=%d heap=%u\n",
                  s.shtOK ? "OK" : "FAIL", s.temperature, s.humidity, s.fireRaw,
                  (int)fireAlarm.load(), (int)audioOK.load(), (unsigned)ESP.getFreeHeap());
  }
  delay(20);
}
