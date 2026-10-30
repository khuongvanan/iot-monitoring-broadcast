#pragma once
#include <Arduino.h>
#include "config.h"
#include "tts_ca.h"
#include "audio_memory.h"

class ModemHTTP {
public:
  HardwareSerial serial{1};
  bool needsReset = false;
  bool tlsReady = false;
  int lastStatus = 0;
  String partial;

  bool traceTTS = false;
  String lastCommand;
  static String commandLabel(const String &cmd) {
    if (cmd.startsWith("AT+HTTPPARA=")) {
      int comma = cmd.indexOf(',');
      return comma >= 0 ? cmd.substring(0, comma) : String("AT+HTTPPARA");
    }
    int equal = cmd.indexOf('=');
    return equal >= 0 ? cmd.substring(0, equal) : cmd;
  }
  void traceResponse(const String &s) {
    if (!traceTTS)
      return;

    if (s == "OK" || error(s) || s.startsWith("+HTTPACTION:"))
      Serial.println("[AT RX] " + s);
  }

  void begin() {
    serial.setRxBufferSize(8192);
    serial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  }
  bool line(String &out, uint32_t timeout) {
    uint32_t t = millis();
    do {
      while (serial.available()) {
        char c = serial.read();
        if (c == '\n') {
          out = partial;
          partial = "";
          out.trim();
          if (out.length())
            return true;
        } else if (c != '\r') {
          if (partial.length() >= 4096) {
            needsReset = true;
            return false;
          }
          partial += c;
        }
      }
      delay(1);
    } while (millis() - t < timeout);
    return false;
  }
  static bool error(const String &s) {
    return s == "ERROR" || s.startsWith("+CME ERROR:") || s.startsWith("+CMS ERROR:");
  }
  void send(const String &cmd) {
    lastCommand = commandLabel(cmd);
    if (traceTTS)
      Serial.println("[AT TX] " + lastCommand);
    serial.print(cmd);
    serial.print("\r\n");
  }
  bool terminal(uint32_t timeout, String *capture = nullptr, const char *expected = "OK") {
    uint32_t t = millis();
    String s;
    while (millis() - t < timeout) {
      if (!line(s, 100))
        continue;
      traceResponse(s);
      if (s == expected)
        return true;
      if (error(s)) {
        Serial.println("[AT FAIL] " + lastCommand + " -> " + s);
        return false;
      }
      if (capture && !s.startsWith("AT")) {
        *capture += s;
        *capture += '\n';
      }
    }
    Serial.println("[AT TIMEOUT] " + lastCommand + " expected=" + String(expected));
    needsReset = true;
    return false;
  }
  bool command(const String &cmd, String *capture = nullptr, uint32_t timeout = 3000) {
    send(cmd);
    return terminal(timeout, capture);
  }
  String query(const String &cmd, uint32_t timeout = 3000) {
    String r;
    if (!command(cmd, &r, timeout))
      return "";
    r.trim();
    return r;
  }
  bool registered() {
    String r = query("AT+CEREG?");
    int n = -1, status = -1;
    int p = r.indexOf("+CEREG:");
    return p >= 0 && sscanf(r.c_str() + p, "+CEREG: %d,%d", &n, &status) == 2 &&
           (status == 1 || status == 5);
  }
  bool initialize() {
    tlsReady = false;
    if (needsReset) {
      Serial.println("[MODEM] Recovery after failed/incomplete AT transaction");

      send("AT+CFUN=1,1");
      delay(8000);
      while (serial.available())
        serial.read();
      partial = "";
      needsReset = false;
    }

    Serial.println("[MODEM] Waiting for AT (no GPIO power/reset control)...");
    bool ready = false;
    uint32_t start = millis();
    while (millis() - start < MODEM_READY_TIMEOUT_MS) {
      if (command("AT", nullptr, 1000)) {
        ready = true;
        break;
      }
      delay(MODEM_AT_RETRY_MS);
    }
    if (!ready) {

      needsReset = false;
      Serial.println("[MODEM] No AT response; check supply/UART/DTR. Will retry.");
      return false;
    }
    needsReset = false;
    if (!command("ATE0") || !command("AT+CMEE=2"))
      return false;
    Serial.println("[MODEM] " + query("ATI"));
    if (query("AT+CPIN?").indexOf("READY") < 0)
      return false;
    command("AT+CTZU=1");
    if (!command("AT+CGDCONT=1,\"IP\",\"" + String(APN) + "\""))
      return false;
    uint32_t t = millis();
    while (millis() - t < 90000) {
      if (registered()) {
        needsReset = false;
        return true;
      }
      delay(2000);
    }
    return false;
  }
  bool configureTTS() {
    Serial.println("[TTS TLS] Configure authenticated TLS context 1");

    String clock = query("AT+CCLK?");
    Serial.println("[MODEM CLOCK] " + clock);
    int yy = 0, mo = 0, dd = 0, h = 0, mi = 0, sec = 0;
    int p = clock.indexOf('"');
    if (p < 0 ||
        sscanf(clock.c_str() + p + 1, "%d/%d/%d,%d:%d:%d", &yy, &mo, &dd, &h, &mi, &sec) != 6 ||
        yy < 26 || mo < 1 || mo > 12 || dd < 1 || dd > 31) {
      Serial.println("[TTS] Modem clock invalid; waiting for network time.");
      return false;
    }

    String certs = query("AT+CCERTLIST");
    if (certs.indexOf("gts_202610.pem") < 0) {
      send("AT+CCERTDOWN=\"gts_202610.pem\"," + String(strlen(TTS_CA)));
      bool prompt = false;
      uint32_t t = millis();
      String r;
      while (millis() - t < 5000) {
        if (!serial.available()) {
          delay(1);
          continue;
        }
        char c = serial.read();
        if (c == '>') {
          prompt = true;
          partial = "";
          break;
        }
        r += c;
        if (r.indexOf("ERROR") >= 0)
          break;
      }
      if (!prompt) {
        Serial.println("[TTS TLS] Certificate upload prompt missing");
        needsReset = true;
        return false;
      }
      size_t len = strlen(TTS_CA);
      for (size_t off = 0; off < len;) {
        size_t n = min((size_t)1024, len - off);
        if (serial.write((const uint8_t *)TTS_CA + off, n) != n)
          return false;
        off += n;
        delay(10);
      }
      if (!terminal(10000))
        return false;
    }
    tlsReady = command("AT+CSSLCFG=\"sslversion\",1,3") && command("AT+CSSLCFG=\"authmode\",1,1") &&
               command("AT+CSSLCFG=\"ignorelocaltime\",1,0") &&
               command("AT+CSSLCFG=\"enableSNI\",1,1") &&
               command("AT+CSSLCFG=\"cacert\",1,\"gts_202610.pem\"");
    Serial.println(tlsReady ? "[TTS TLS] Configuration accepted"
                            : "[TTS TLS] Configuration failed; see AT FAIL above");
    return tlsReady;
  }
  bool exact(uint8_t *dst, size_t n, uint32_t timeout) {
    size_t done = 0;
    uint32_t t = millis();
    while (done < n && millis() - t < timeout) {
      int avail = serial.available();
      if (avail) {
        size_t k = min((size_t)avail, n - done);
        size_t got = serial.readBytes(dst + done, k);
        done += got;
      } else
        delay(1);
    }
    if (done != n)
      needsReset = true;
    return done == n;
  }
  bool readChunk(size_t offset, uint8_t *dst, size_t wanted) {
    send("AT+HTTPREAD=" + String(offset) + "," + String(wanted));
    String s;
    uint32_t t = millis();
    size_t got = 0;
    bool header = false;
    while (millis() - t < 10000) {
      if (!line(s, 100))
        continue;
      if (error(s))
        return false;
      if (s.startsWith("+HTTPREAD:")) {
        unsigned n = 0;
        if (sscanf(s.c_str(), "+HTTPREAD: %u", &n) != 1 || n != wanted) {
          needsReset = true;
          return false;
        }
        got = n;
        header = true;
        break;
      }
    }
    if (!header || !exact(dst, got, 10000)) {
      needsReset = true;
      return false;
    }

    t = millis();
    while (millis() - t < 5000) {
      if (!line(s, 100))
        continue;
      if (s == "+HTTPREAD: 0")
        return true;
      if (error(s))
        return false;
    }
    needsReset = true;
    return false;
  }

  int readUnknownBody(uint8_t **body, size_t &len, size_t cap) {
    if (!cap || cap > 153599)
      return -18;
    uint8_t *data = audioAlloc(cap + 1);
    if (!data)
      return -19;
    send("AT+HTTPREAD=0," + String(cap + 1));
    uint32_t started = millis();
    size_t used = 0;
    String s;
    while (millis() - started < 120000) {
      if (!line(s, 100))
        continue;
      traceResponse(s);
      if (error(s)) {
        Serial.println("[TTS CHUNK] Read rejected: " + s);
        free(data);
        needsReset = true;
        return -20;
      }
      if (!s.startsWith("+HTTPREAD:"))
        continue;
      unsigned n = 0;
      char extra = 0;
      if (sscanf(s.c_str(), "+HTTPREAD: %u %c", &n, &extra) != 1) {
        Serial.println("[TTS CHUNK] Invalid length header");
        free(data);
        needsReset = true;
        return -20;
      }
      if (n == 0) {
        data[used] = 0;
        *body = data;
        len = used;
        Serial.printf("[TTS CHUNK] Complete: %u bytes\n", (unsigned)used);
        return 0;
      }
      if (n > cap - used) {
        Serial.println("[TTS CHUNK] Body exceeds configured size limit");
        free(data);
        needsReset = true;
        return -18;
      }
      if (!exact(data + used, n, 40000)) {
        Serial.println("[TTS CHUNK] Binary payload timeout");
        free(data);
        needsReset = true;
        return -20;
      }
      used += n;
      Serial.printf("[TTS CHUNK] Received %u bytes; total=%u\n", n, (unsigned)used);
    }
    Serial.println("[TTS CHUNK] Missing end marker; discarding partial audio");
    free(data);
    needsReset = true;
    return -20;
  }

  int request(const String &url, const String *post, uint8_t **body, size_t &len,
              size_t maxBytes = 4096, bool googleTLS = false) {

    struct TraceGuard {
      bool &flag;
      bool saved;
      TraceGuard(bool &f, bool enabled) : flag(f), saved(f) {
        flag = enabled;
      }
      ~TraceGuard() {
        flag = saved;
      }
    } traceGuard(traceTTS, googleTLS);
    if (googleTLS)
      Serial.println("[TTS BEGIN] HTTPS audio request (URL/text hidden)");
    len = 0;
    if (body)
      *body = nullptr;
    lastStatus = 0;
    if (needsReset)
      return -10;
    if (url.indexOf('"') >= 0 || url.indexOf('\r') >= 0 || url.indexOf('\n') >= 0)
      return -11;
    // The TTS trust context must not be reused for an unrelated server.
    if (url.startsWith("https://") && !googleTLS) {
      Serial.println("[HTTP] HTTPS host needs its own CA/context; see README.");
      return -12;
    }
    if (googleTLS && !tlsReady && !configureTTS())
      return -13;

    command("AT+HTTPTERM", nullptr, 120000);
    if (needsReset)
      return -14;
    if (!command("AT+HTTPINIT", nullptr, 120000))
      return -15;
    int result = -16;
    do {
      if (!command("AT+HTTPPARA=\"URL\",\"" + url + "\""))
        break;
      if (!command("AT+HTTPPARA=\"READMODE\",1"))
        break;
      if (!command("AT+HTTPPARA=\"CONNECTTO\",30"))
        break;
      if (!command("AT+HTTPPARA=\"RECVTO\",40"))
        break;
      if (googleTLS && !command("AT+HTTPPARA=\"SSLCFG\",1"))
        break;
      if (post) {
        if (!command("AT+HTTPPARA=\"CONTENT\",\"application/json\""))
          break;
        send("AT+HTTPDATA=" + String(post->length()) + ",10000");
        if (!terminal(5000, nullptr, "DOWNLOAD"))
          break;
        serial.print(*post);
        if (!terminal(10000))
          break;
      }
      int method = post ? 1 : 0;
      send("AT+HTTPACTION=" + String(method));
      String s;
      uint32_t t = millis();
      int m = -1, code = 0, bytes = -1;
      bool completed = false, actionError = false, chunked = false;
      uint32_t nextProgress = 10000;

      while (millis() - t < 90000) {
        if (googleTLS && millis() - t >= nextProgress) {
          Serial.printf("[TTS WAIT] HTTPACTION elapsed=%lu ms\n", (unsigned long)(millis() - t));
          nextProgress += 10000;
        }
        if (!line(s, 100))
          continue;
        traceResponse(s);
        if (error(s)) {
          actionError = true;
          Serial.println("[HTTPACTION ERROR] " + s);
          break;
        }
        char lengthField[24] = {0}, extra = 0;
        if (sscanf(s.c_str(), "+HTTPACTION: %d,%d,%23s %c", &m, &code, lengthField, &extra) == 3 &&
            m == method) {
          if (strcmp(lengthField, "chunk") == 0) {
            chunked = true;
            completed = true;
            break;
          }
          char *end = nullptr;
          long parsed = strtol(lengthField, &end, 10);
          if (end != lengthField && *end == '\0' && parsed >= 0 && parsed <= 2147483647L) {
            bytes = (int)parsed;
            completed = true;
            break;
          }
        }
      }
      if (!completed) {
        Serial.printf("[HTTPACTION FAIL] reason=%s elapsed=%lu ms method=%d\n",
                      actionError ? "MODEM_ERROR" : "NO_MATCHING_URC_TIMEOUT",
                      (unsigned long)(millis() - t), method);
        if (partial.length())
          Serial.printf("[HTTPACTION FAIL] incomplete UART line: %u bytes (content hidden)\n",
                        (unsigned)partial.length());
        needsReset = true;
        result = -17;
        break;
      }
      if (googleTLS)
        Serial.printf("[TTS ACTION] status=%d length=%d\n", code, bytes);
      lastStatus = code;
      result = code;
      if (code < 200 || code >= 300 || !body)
        break;
      if (chunked) {
        Serial.println("[TTS CHUNK] Unknown-length response detected");
        int readResult = readUnknownBody(body, len, maxBytes);
        if (readResult != 0)
          result = readResult;
        break;
      }
      if (bytes == 0)
        break;
      if (bytes < 0 || (size_t)bytes > maxBytes) {
        result = -18;
        break;
      }
      uint8_t *data = audioAlloc((size_t)bytes + 1);
      if (!data) {
        result = -19;
        break;
      }
      bool ok = true;
      for (size_t off = 0; off < (size_t)bytes;) {
        size_t n = min((size_t)1024, (size_t)bytes - off);
        if (!readChunk(off, data + off, n)) {
          ok = false;
          break;
        }
        off += n;
      }
      if (!ok) {
        free(data);
        result = -20;
        break;
      }
      data[bytes] = 0;
      *body = data;
      len = bytes;
    } while (false);
    if (!needsReset)
      command("AT+HTTPTERM", nullptr, 120000);
    if (googleTLS)
      Serial.printf("[TTS END] result=%d bytes=%u recovery=%d\n", result, (unsigned)len,
                    (int)needsReset);
    return result;
  }
};
