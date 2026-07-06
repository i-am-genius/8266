#include "online_logger.h"
#include "app_config.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// ===================== Configuration =====================
#define LOG_RING_SIZE       32
#define LOG_UPLOAD_INTERVAL_MS  10000
#define LOG_UPLOAD_BATCH_SIZE   20
#define LOG_UPLOAD_ENDPOINT     "/admin/device/logs/batch"

// ===================== Ring Buffer =====================
static LogEntry logRing[LOG_RING_SIZE];
static int logHead = 0;  // next write position
static int logCount = 0; // number of entries in buffer
static unsigned long lastUploadMs = 0;

// ===================== Server Config =====================
static char logServerHost[64] = "";
static uint16_t logServerPort = 80;
static char logUploadSecret[64] = "YZCDDT4UC++estDhY0jGOqcWwWmEAcu9KfTqkU08a5s=";
static char logDeviceId[32] = "";

// ===================== Helper: escape JSON string =====================
static String escapeJson(const char* str) {
  String out;
  out.reserve(strlen(str) + 8);
  for (size_t i = 0; str[i] != '\0'; i++) {
    char c = str[i];
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': break;  // skip CR
      default:   out += c; break;
    }
  }
  return out;
}

static const char* levelToString(uint8_t level) {
  switch (level) {
    case LOG_LEVEL_DEBUG: return "DEBUG";
    case LOG_LEVEL_INFO:  return "INFO";
    case LOG_LEVEL_WARN:  return "WARN";
    case LOG_LEVEL_ERROR: return "ERROR";
    default:              return "UNKNOWN";
  }
}

// ===================== Init =====================
void logInit() {
  logHead = 0;
  logCount = 0;
  lastUploadMs = 0;
  DEBUG_SERIAL.println("[LOG] online logger initialized");
}

// ===================== Write (printf style) =====================
void logWrite(uint8_t level, const char* module, const char* fmt, ...) {
  // Also print to DEBUG_SERIAL
  DEBUG_SERIAL.printf("[%s][%s] ", levelToString(level), module);

  char buf[128];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  DEBUG_SERIAL.println(buf);

  // Write to ring buffer
  if (logCount < LOG_RING_SIZE) {
    // Buffer not full, write normally
    LogEntry& entry = logRing[logHead];
    entry.timestampMs = millis();
    entry.level = level;
    strncpy(entry.module, module, sizeof(entry.module) - 1);
    entry.module[sizeof(entry.module) - 1] = '\0';
    strncpy(entry.message, buf, sizeof(entry.message) - 1);
    entry.message[sizeof(entry.message) - 1] = '\0';
    logHead = (logHead + 1) % LOG_RING_SIZE;
    logCount++;
  } else if (level >= LOG_LEVEL_WARN) {
    // Buffer full, only replace DEBUG/INFO with WARN/ERROR
    // Find oldest DEBUG/INFO entry to replace
    for (int i = 0; i < LOG_RING_SIZE; i++) {
      int idx = (logHead + i) % LOG_RING_SIZE;
      if (logRing[idx].level < LOG_LEVEL_WARN) {
        LogEntry& entry = logRing[idx];
        entry.timestampMs = millis();
        entry.level = level;
        strncpy(entry.module, module, sizeof(entry.module) - 1);
        entry.module[sizeof(entry.module) - 1] = '\0';
        strncpy(entry.message, buf, sizeof(entry.message) - 1);
        entry.message[sizeof(entry.message) - 1] = '\0';
        break;
      }
    }
  }
  // If buffer full and new log is DEBUG/INFO, discard it
}

// ===================== Write (String overload) =====================
void logWrite(uint8_t level, const char* module, const String& msg) {
  logWrite(level, module, "%s", msg.c_str());
}

// ===================== Set Server =====================
void logSetServer(const char* host, uint16_t port, const char* secret) {
  strncpy(logServerHost, host, sizeof(logServerHost) - 1);
  logServerHost[sizeof(logServerHost) - 1] = '\0';
  logServerPort = port;
  if (secret != nullptr) {
    strncpy(logUploadSecret, secret, sizeof(logUploadSecret) - 1);
    logUploadSecret[sizeof(logUploadSecret) - 1] = '\0';
  }
}

// ===================== Set Device ID =====================
void logSetDeviceId(const char* deviceId) {
  strncpy(logDeviceId, deviceId, sizeof(logDeviceId) - 1);
  logDeviceId[sizeof(logDeviceId) - 1] = '\0';
}

// ===================== Upload =====================
void uploadLogs() {
  // Only depend on WiFi, not wsConnected
  if (WiFi.status() != WL_CONNECTED) return;
  if (logServerHost[0] == '\0') return;
  if (logCount == 0) return;

  unsigned long now = millis();
  if (now - lastUploadMs < LOG_UPLOAD_INTERVAL_MS) return;

  lastUploadMs = now;

  // Build NDJSON payload
  int batchCount = min((int)logCount, LOG_UPLOAD_BATCH_SIZE);

  String ndjson;
  ndjson.reserve(batchCount * 160);

  // Calculate start index (oldest entry in ring)
  int startIdx = (logHead - logCount + LOG_RING_SIZE) % LOG_RING_SIZE;

  for (int i = 0; i < batchCount; i++) {
    int idx = (startIdx + i) % LOG_RING_SIZE;
    LogEntry& e = logRing[idx];

    ndjson += "{\"uptimeMs\":";
    ndjson += String(e.timestampMs);
    ndjson += ",\"level\":\"";
    ndjson += levelToString(e.level);
    ndjson += "\",\"module\":\"";
    ndjson += escapeJson(e.module);
    ndjson += "\",\"msg\":\"";
    ndjson += escapeJson(e.message);
    ndjson += "\"}\n";
  }

  // Build URL with chipId and uploadUptimeMs parameters
  String url = "http://" + String(logServerHost) + ":" + String(logServerPort) + LOG_UPLOAD_ENDPOINT;
  if (logDeviceId[0] != '\0') {
    url += "?chipId=" + String(logDeviceId);
    url += "&uploadUptimeMs=" + String(now);
  }

  // HTTP POST
  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(2000);
  http.addHeader("Content-Type", "application/x-ndjson");
  if (logUploadSecret[0] != '\0') {
    http.addHeader("X-Upload-Secret", logUploadSecret);
  }

  int httpCode = http.POST(ndjson);
  if (httpCode >= 200 && httpCode < 300) {
    // Success: remove uploaded entries
    int remaining = logCount - batchCount;
    if (remaining <= 0) {
      logHead = 0;
      logCount = 0;
    } else {
      logCount = remaining;
    }
    DEBUG_SERIAL.printf("[LOG] uploaded %d entries, code=%d\n", batchCount, httpCode);
  } else {
    DEBUG_SERIAL.printf("[LOG] upload failed code=%d\n", httpCode);
  }

  http.end();
}
