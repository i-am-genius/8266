#include "config/config_manager.h"
#include "diagnostics/diagnostic_logger.h"

String configPath() {
  return "/config.json";
}

String makeDeviceId() {
  String id = "lamp-";
  id += String(ESP.getChipId(), HEX);
  id.toUpperCase();
  return id;
}

bool saveConfig(const DeviceConfig& c) {
  StaticJsonDocument<256> doc;
  doc["ssid"] = c.ssid;
  doc["password"] = c.password;
  doc["serverHost"] = c.serverHost;
  doc["httpPort"] = c.httpPort;
  doc["wsPort"] = c.wsPort;

  File f = LittleFS.open(configPath(), "w");
  if (!f) {
    diagnosticLogConfig("open for write failed", true);
    return false;
  }

  if (serializeJson(doc, f) == 0) {
    f.close();
    diagnosticLogConfig("serialize failed", true);
    return false;
  }
  f.close();
  return true;
}

bool loadConfig() {
  cfg.ssid = DEFAULT_WIFI_SSID;
  cfg.password = DEFAULT_WIFI_PASSWORD;
  cfg.serverHost = DEFAULT_SERVER_HOST;
  cfg.httpPort = DEFAULT_HTTP_PORT;
  cfg.wsPort = DEFAULT_WS_PORT;

  if (!LittleFS.exists(configPath())) return cfg.ssid.length() > 0;

  File f = LittleFS.open(configPath(), "r");
  if (!f) {
    diagnosticLogConfig("open for read failed", true);
    return false;
  }

  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    diagnosticLogConfig("json parse failed", true);
    return false;
  }

  cfg.ssid = doc["ssid"] | DEFAULT_WIFI_SSID;
  cfg.password = doc["password"] | DEFAULT_WIFI_PASSWORD;
  cfg.serverHost = doc["serverHost"] | DEFAULT_SERVER_HOST;
  cfg.httpPort = doc["httpPort"] | DEFAULT_HTTP_PORT;
  cfg.wsPort = doc["wsPort"] | DEFAULT_WS_PORT;

  return cfg.ssid.length() > 0;
}

void clearConfig() {
  if (LittleFS.exists(configPath())) {
    if (!LittleFS.remove(configPath())) {
      diagnosticLogConfig("remove failed", true);
    }
  }
}

void ensureConfigDefaults(DeviceConfig& c) {
  if (c.serverHost.length() == 0) c.serverHost = DEFAULT_SERVER_HOST;
  if (c.httpPort == 0) c.httpPort = DEFAULT_HTTP_PORT;
  if (c.wsPort == 0) c.wsPort = DEFAULT_WS_PORT;
}
