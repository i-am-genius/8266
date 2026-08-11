#pragma once

#include <Arduino.h>
#include <cstdarg>
#include "diagnostics/diagnostic_policy.h"

// ===================== Log Levels =====================
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

// ===================== Log Macros =====================
#define LOG_DEBUG(module, msg) logWrite(LOG_LEVEL_DEBUG, module, msg)
#define LOG_INFO(module, msg)  logWrite(LOG_LEVEL_INFO, module, msg)
#define LOG_WARN(module, msg)  logWrite(LOG_LEVEL_WARN, module, msg)
#define LOG_ERROR(module, msg) logWrite(LOG_LEVEL_ERROR, module, msg)

// ===================== Log Entry =====================
struct LogEntry {
  unsigned long timestampMs;  // millis() at creation
  uint8_t level;              // LOG_LEVEL_*
  char module[12];            // short module name, e.g. "WIFI", "WS", "OTA"
  char message[128];          // log message
};

// ===================== API =====================
void logInit();
void logWrite(uint8_t level, const char* module, const char* fmt, ...);
void logWrite(uint8_t level, const char* module, const String& msg);
void logSetServer(const char* host, uint16_t port, const char* secret = nullptr);
void logSetDeviceId(const char* deviceId);
void uploadLogs();
bool uploadLogsBeforeRestart();

using LogTransportStats = LogTransportPolicy;
LogTransportStats getLogTransportStats();
bool consumeLogUploadRecovery(uint32_t& failureCount, int& lastCode);
