#include "network/http_reporter.h"
#include "device/self_test.h"

// WS 连接后首次 state-report 延迟 (ms)，0 表示不延迟
#define STATE_REPORT_FIRST_DELAY_MS 10000

// 两次 state-report 之间的最小间隔 (ms)
#define STATE_REPORT_COOLDOWN_MS 10000

String httpUrl(const String& path) {
  return "http://" + cfg.serverHost + ":" + String(cfg.httpPort) + path;
}

// ==================== 通用 JSON POST（HTTPClient 原始配置） ====================

static String lastHttpResponseBody = "";

static String summarizeHttpBody(const String& body) {
  const unsigned int maxLen = 180;
  String value = body;
  value.replace("\r", "\\r");
  value.replace("\n", "\\n");
  if (value.length() > maxLen) {
    return value.substring(0, maxLen) + "...";
  }
  return value;
}

int postJsonToServer(const String& path, const String& jsonBody) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpUrl(path));
  http.addHeader("Content-Type", "application/json");

  unsigned long startMs = millis();
  int httpCode = http.POST(jsonBody);
  unsigned long costMs = millis() - startMs;

  if (httpCode > 0) {
    String response = http.getString();
    lastHttpResponseBody = response;
    DEBUG_SERIAL.printf("[HTTP] POST %s -> %d (cost=%lu ms)\n", path.c_str(), httpCode, costMs);
    DEBUG_SERIAL.println(summarizeHttpBody(response));
  } else {
    lastHttpResponseBody = "";
    DEBUG_SERIAL.printf("[HTTP] POST %s failed: %s (cost=%lu ms)\n",
                        path.c_str(), http.errorToString(httpCode).c_str(), costMs);
  }

  http.end();
  return httpCode;
}

// ==================== State Report ====================

static unsigned long lastStateReportSentAt = 0;
static String pendingStateReportReason = "";
static String lastReportSkipReason = "";
static unsigned long lastReportSkipLogAt = 0;
static bool lastStateReportHttpSuccess = false;

static void logReportSkip(const char* reason) {
  unsigned long now = millis();
  String value = reason ? reason : "UNKNOWN";
  if (value != lastReportSkipReason || now - lastReportSkipLogAt >= 5000) {
    DEBUG_SERIAL.printf("[REPORT] skip reason=%s pending=%d includeSelfTest=%d\n",
                        value.c_str(), pendingStateReport, pendingStateReportWithSelfTest);
    lastReportSkipReason = value;
    lastReportSkipLogAt = now;
  }
}

void requestDeviceStateReport(const char* reason, bool includeSelfTest) {
  const char* safeReason = reason ? reason : "UNKNOWN";
  bool wasPending = pendingStateReport;
  bool wasSelfTestPending = pendingStateReportWithSelfTest;

  pendingStateReport = true;
  pendingStateReportWithSelfTest = pendingStateReportWithSelfTest || includeSelfTest;

  if (pendingStateReportReason.length() == 0) {
    pendingStateReportReason = safeReason;
  } else if (pendingStateReportReason.indexOf(safeReason) < 0) {
    pendingStateReportReason += "+";
    pendingStateReportReason += safeReason;
  }

  DEBUG_SERIAL.printf(
    "[REPORT] request reason=%s includeSelfTest=%d wasPending=%d wasSelfTest=%d mergedReason=%s mergedSelfTest=%d\n",
    safeReason,
    includeSelfTest,
    wasPending,
    wasSelfTestPending,
    pendingStateReportReason.c_str(),
    pendingStateReportWithSelfTest
  );
}

static bool reportReasonContains(const String& reason, const char* token) {
  return token && reason.indexOf(token) >= 0;
}

static bool isRetryableStateReportCode(int httpCode) {
  return httpCode < 0 || httpCode == 408 || httpCode == 429 || httpCode >= 500;
}

bool sendDeviceStateReport(bool includeSelfTest, const char* reason) {
  const char* safeReason = reason ? reason : "DIRECT";
  lastStateReportHttpSuccess = false;
  if (WiFi.status() != WL_CONNECTED) {
    DEBUG_SERIAL.printf("[REPORT] failed reason=WIFI_DISCONNECTED source=%s\n", safeReason);
    return false;
  }

  // backoff: if last request failed, wait before retry
  if (lastStateReportFailedAt > 0 && millis() - lastStateReportFailedAt < stateReportBackoffMs) {
    logReportSkip("BACKOFF_DIRECT");
    return false;
  }

  StaticJsonDocument<1024> doc;
  doc["chipId"] = deviceId;
  doc["deviceType"] = FW_DEVICE_TYPE;
  doc["ip"] = WiFi.localIP().toString();
  doc["luxAutoTarget"] = luxAutoTarget;
  doc["luxAutoBrightness"] = luxAutoBrightness;
  doc["firmwareVersion"] = FW_VERSION;
  doc["firmwareVersionCode"] = FW_VERSION_CODE;
  doc["firmwareChannel"] = FW_CHANNEL;
  doc["otaStatus"] = otaStatus;
  doc["otaProgress"] = otaProgress;
  if (includeSelfTest && selfTestDone) {
    appendSelfTestJson(doc.as<JsonObject>());
  }

  String json;
  serializeJson(doc, json);

  DEBUG_SERIAL.printf("[REPORT] start includeSelfTest=%d reason=%s bodyLength=%u\n",
                      includeSelfTest, safeReason, json.length());

  // ===== 诊断日志：POST 前状态 =====
  DEBUG_SERIAL.println("--- [HTTP] state-report pre-flight ---");
  DEBUG_SERIAL.printf("[HTTP] free heap = %u\n", ESP.getFreeHeap());
  DEBUG_SERIAL.printf("[HTTP] WiFi.status() = %d\n", WiFi.status());
  DEBUG_SERIAL.printf("[HTTP] wsConnected = %d\n", wsConnected);

  int httpCode = postJsonToServer("/admin/device/state-report", json);

  DEBUG_SERIAL.printf("[HTTP] state-report httpCode=%d\n", httpCode);
  DEBUG_SERIAL.printf("[HTTP] free heap after http.end() = %u\n", ESP.getFreeHeap());

  if (httpCode >= 200 && httpCode < 300) {
    DEBUG_SERIAL.printf("[REPORT] done code=%d reason=%s\n", httpCode, safeReason);
    DEBUG_SERIAL.println("[HTTP] state-report OK");
    lastStateReportHttpSuccess = true;
    lastStateReportFailedAt = 0;
    lastStateReportSentAt = millis();
    return true;
  }

  if (isRetryableStateReportCode(httpCode)) {
    DEBUG_SERIAL.printf("[REPORT] failed reason=HTTP_ERROR source=%s code=%d\n", safeReason, httpCode);
    lastStateReportFailedAt = millis();
    DEBUG_SERIAL.printf("[HTTP] state-report failed, backoff %lu ms\n", stateReportBackoffMs);
    return false;
  }

  DEBUG_SERIAL.printf("[REPORT] permanent failure code=%d reason=%s\n", httpCode, safeReason);
  DEBUG_SERIAL.println("[HTTP] state-report response summary: " + summarizeHttpBody(lastHttpResponseBody));
  lastStateReportFailedAt = 0;
  lastStateReportSentAt = millis();
  return true;
}

void handleDeviceStateReportTask() {
  if (!pendingStateReport) return;
  if (WiFi.status() != WL_CONNECTED) {
    logReportSkip("WIFI_DISCONNECTED");
    return;
  }

  // backoff: if last request failed, wait before retry
  if (lastStateReportFailedAt > 0 && millis() - lastStateReportFailedAt < stateReportBackoffMs) {
    logReportSkip("BACKOFF");
    return;
  }

  // 首次延迟：WS 连接后等 STATE_REPORT_FIRST_DELAY_MS 再发
  if (lastWsConnectedMs > 0 && millis() - lastWsConnectedMs < STATE_REPORT_FIRST_DELAY_MS) {
    logReportSkip("FIRST_WS_DELAY");
    return;
  }

  // 冷却：两次 report 之间至少间隔 STATE_REPORT_COOLDOWN_MS
  if (lastStateReportSentAt > 0 && millis() - lastStateReportSentAt < STATE_REPORT_COOLDOWN_MS) {
    logReportSkip("COOLDOWN");
    return;
  }

  bool includeSelfTest = pendingStateReportWithSelfTest;
  String reason = pendingStateReportReason.length() > 0 ? pendingStateReportReason : "PENDING";
  DEBUG_SERIAL.printf("[REPORT] dequeue includeSelfTest=%d reason=%s\n",
                      includeSelfTest, reason.c_str());

  bool ok = sendDeviceStateReport(includeSelfTest, reason.c_str());
  if (!ok) {
    DEBUG_SERIAL.printf("[REPORT] keep pending reason=%s includeSelfTest=%d\n",
                        reason.c_str(), includeSelfTest);
    pendingStateReport = true;
    pendingStateReportWithSelfTest = pendingStateReportWithSelfTest || includeSelfTest;
    pendingStateReportReason = reason;
    return;
  }

  pendingStateReport = false;
  pendingStateReportWithSelfTest = false;
  pendingStateReportReason = "";

  if (lastStateReportHttpSuccess && !bootOnlineReportDone) {
    bootOnlineReportDone = true;
    DEBUG_SERIAL.println("[REPORT] boot online report done");
  }

  if (lastStateReportHttpSuccess && reportReasonContains(reason, "SELFTEST_DONE")) {
    bootSelfTestReportDone = true;
    DEBUG_SERIAL.println("[REPORT] boot selfTest report done");
  }
}

// ==================== Announce ====================

void sendAnnounce() {
  if (!enableAnnounce || WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<256> doc;
  doc["chipId"] = deviceId;
  doc["ip"] = WiFi.localIP().toString();
  doc["deviceType"] = FW_DEVICE_TYPE;

  String json;
  serializeJson(doc, json);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpUrl("/admin/device/announce"));
  http.addHeader("Content-Type", "application/json");

  unsigned long startMs = millis();
  int httpCode = http.POST(json);
  unsigned long costMs = millis() - startMs;

  if (httpCode > 0) {
    String payload = http.getString();
    DEBUG_SERIAL.printf("[HTTP] announce -> %d (cost=%lu ms)\n", httpCode, costMs);
    DEBUG_SERIAL.println("[HTTP] announce response: " + payload);

    if (payload.indexOf("\"added\":true") >= 0 || payload.indexOf("\"added\": true") >= 0) {
      enableAnnounce = false;
      enableBroadcast = false;
      DEBUG_SERIAL.println("[HTTP] announce 设备已添加，停止 announce/broadcast");
    } else {
      DEBUG_SERIAL.println("[HTTP] announce 200 but 'added:true' not found in response");
    }
  } else {
    DEBUG_SERIAL.printf("[HTTP] announce failed: %s (cost=%lu ms)\n",
                        http.errorToString(httpCode).c_str(), costMs);
  }

  http.end();
}

// ==================== 光照上报 / 停留时长上报 ====================

void sendLightLevelToServer() {
  if (!bh1750Ready || WiFi.status() != WL_CONNECTED) return;

  float lux = lightMeter.readLightLevel();
  DEBUG_SERIAL.printf("当前光照值：%.2f lux\n", lux);

  StaticJsonDocument<192> doc;
  doc["chipId"] = deviceId;
  doc["luxValue"] = lux;

  String json;
  serializeJson(doc, json);
  postJsonToServer("/admin/lux/create", json);
}

void sendStayRecordToServer(unsigned long durationSeconds) {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<192> doc;
  doc["chipId"] = deviceId;
  doc["durationValue"] = durationSeconds * 1000UL;

  String payload;
  serializeJson(doc, payload);
  postJsonToServer("/admin/duration/create", payload);
}
