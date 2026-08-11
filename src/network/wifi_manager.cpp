#include "network/wifi_manager.h"
#include "config/config_manager.h"
#include "online_logger.h"
#include "diagnostics/diagnostic_logger.h"

// ---- HTML 配网页面 ----

String getPortalHtml() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP8266 配网</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body{font-family:Arial;padding:20px;max-width:560px;margin:auto;}
    input{width:100%;padding:10px;margin:8px 0;box-sizing:border-box;}
    button{padding:12px 18px;margin-top:10px;}
    .box{border:1px solid #ddd;border-radius:12px;padding:16px;}
    .tip{color:#666;font-size:14px;}
  </style>
</head>
<body>
  <div class="box">
    <h2>灯节点配网</h2>
    <p>设备ID：__DEVICE_ID__</p>
    <p class="tip">设备优先通过 SmartConfig 配网；若连接失败则自动切换到此 AP 配网页面。</p>

    <form action="/saveWifi" method="POST">
      <label>Wi-Fi 名称</label>
      <input name="ssid" placeholder="请输入 Wi-Fi 名称">

      <label>Wi-Fi 密码</label>
      <input name="password" type="password" placeholder="请输入 Wi-Fi 密码">

      <label>服务器 Host/IP</label>
      <input name="serverHost" value="__SERVER_HOST__">

      <label>HTTP 端口</label>
      <input name="httpPort" value="__HTTP_PORT__">

      <label>WebSocket 端口</label>
      <input name="wsPort" value="__WS_PORT__">

      <button type="submit">保存并重启</button>
    </form>

    <form action="/resetWifi" method="POST">
      <button type="submit">清除配置并重启</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

  html.replace("__DEVICE_ID__", deviceId);
  html.replace("__SERVER_HOST__", cfg.serverHost.length() ? cfg.serverHost : String(DEFAULT_SERVER_HOST));
  html.replace("__HTTP_PORT__", String(cfg.httpPort ? cfg.httpPort : DEFAULT_HTTP_PORT));
  html.replace("__WS_PORT__", String(cfg.wsPort ? cfg.wsPort : DEFAULT_WS_PORT));
  return html;
}

// ---- WiFi 状态可读转换 ----

const char* wifiStatusToString(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:      return "WL_IDLE_STATUS (0)";
    case WL_NO_SSID_AVAIL:    return "WL_NO_SSID_AVAIL (1)";
    case WL_SCAN_COMPLETED:   return "WL_SCAN_COMPLETED (2)";
    case WL_CONNECTED:        return "WL_CONNECTED (3)";
    case WL_CONNECT_FAILED:   return "WL_CONNECT_FAILED (4)";
    case WL_CONNECTION_LOST:  return "WL_CONNECTION_LOST (5)";
    case WL_DISCONNECTED:     return "WL_DISCONNECTED (6)";
    default:                  return "UNKNOWN";
  }
}

// ---- WiFi 连接 ----

bool connectWiFi(const String& ssid, const String& password, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  DEBUG_SERIAL.println("\n[WiFi] 正在连接: " + ssid);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    DEBUG_SERIAL.print(".");
    yield();
  }
  DEBUG_SERIAL.println();

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_SERIAL.println("[WiFi] 连接成功: " + WiFi.localIP().toString());
    String msg = "连接成功 SSID=" + ssid + " IP=" + WiFi.localIP().toString();
    diagnosticLogWifi(msg.c_str());
    return true;
  }

  DEBUG_SERIAL.println("[WiFi] 连接失败");
  String msg = "连接失败 SSID=" + ssid;
  diagnosticLogWifi(msg.c_str(), true);
  return false;
}

bool connectSavedWiFi() {
  if (cfg.ssid.length() == 0) return false;

  if (connectWiFi(cfg.ssid, cfg.password, wifiConnectTimeout)) {
    return true;
  }

  if (cfg.ssid == DEFAULT_WIFI_SSID && cfg.password == DEFAULT_WIFI_PASSWORD) {
    return false;
  }

  DEBUG_SERIAL.println("[WiFi] Saved network failed, trying default network...");
  if (!connectWiFi(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASSWORD, wifiConnectTimeout)) {
    return false;
  }

  cfg.ssid = DEFAULT_WIFI_SSID;
  cfg.password = DEFAULT_WIFI_PASSWORD;
  return true;
}

// ---- 串行配网 (SmartConfig 优先, AP 后备) ----

static void ensureProvisionRoutes() {
  static bool registered = false;
  if (registered) return;
  registered = true;

  server.on("/", HTTP_GET, []() {
    server.send(200, "text/html; charset=utf-8", getPortalHtml());
  });

  server.on("/saveWifi", HTTP_POST, []() {
    DeviceConfig newCfg;
    newCfg.ssid = server.arg("ssid");
    newCfg.password = server.arg("password");
    newCfg.serverHost = server.arg("serverHost");
    newCfg.httpPort = (uint16_t)server.arg("httpPort").toInt();
    newCfg.wsPort = (uint16_t)server.arg("wsPort").toInt();

    if (newCfg.ssid.length() == 0) {
      server.send(400, "text/plain; charset=utf-8", "Wi-Fi 名称不能为空");
      return;
    }

    ensureConfigDefaults(newCfg);

    if (!saveConfig(newCfg)) {
      server.send(500, "text/plain; charset=utf-8", "保存失败");
      return;
    }

    DEBUG_SERIAL.println("[PROV] AP config saved, restarting...");

    if (smartConfigActive) {
      WiFi.stopSmartConfig();
      smartConfigActive = false;
    }

    server.send(200, "text/html; charset=utf-8", "<h3>保存成功，设备即将重启...</h3>");
    delay(1200);
    diagnosticRestart("wifi_config_saved");
  });

  server.on("/resetWifi", HTTP_POST, []() {
    clearConfig();
    DEBUG_SERIAL.println("[PROV] Config cleared via AP, restarting...");
    server.send(200, "text/html; charset=utf-8", "<h3>已清除配置，设备即将重启...</h3>");
    delay(1200);
    diagnosticRestart("wifi_config_cleared");
  });
}

void startAPPortal() {
  // 先停掉正在运行的 SmartConfig
  if (smartConfigActive) {
    WiFi.stopSmartConfig();
    smartConfigActive = false;
  }

  WiFi.disconnect(true);
  delay(500);
  WiFi.mode(WIFI_AP);
  delay(300);

  IPAddress apIP(192, 168, 4, 1);
  IPAddress gateway(192, 168, 4, 1);
  IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gateway, subnet);

  String apName = "LightConfig_" + deviceId;
  bool apOk = WiFi.softAP(apName.c_str(), AP_DEFAULT_PASSWORD, 1, false, 4);

  DEBUG_SERIAL.println("[PROV] ====== AP portal started ======");
  DEBUG_SERIAL.println("[PROV] AP SSID: " + apName);
  DEBUG_SERIAL.println("[PROV] AP password: " + String(AP_DEFAULT_PASSWORD));

  if (apOk) {
    DEBUG_SERIAL.println("[PROV] AP portal ready -> http://" + WiFi.softAPIP().toString());
  } else {
    DEBUG_SERIAL.println("[PROV] AP start failed!");
  }

  ensureProvisionRoutes();
  server.begin();
}

void startParallelProvision() {
  provisioningMode = true;

  // ---- SmartConfig 启动前：状态检查 + 清理 ----
  DEBUG_SERIAL.println("\n[PROV] === SmartConfig 启动前状态检查 ===");
  DEBUG_SERIAL.println("[PROV] MAC: " + WiFi.macAddress());
  DEBUG_SERIAL.println("[PROV] 当前模式 (getMode): " + String(WiFi.getMode()));
  DEBUG_SERIAL.println("[PROV] 当前状态 (status): " + String(wifiStatusToString(WiFi.status())));

  // 确保纯 STA 模式，先断开所有连接（ESP8266 只能 STA 模式启动 SmartConfig）
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(500);

  DEBUG_SERIAL.println("[PROV] 清理后模式: " + String(WiFi.getMode()));
  DEBUG_SERIAL.println("[PROV] 清理后状态: " + String(wifiStatusToString(WiFi.status())));
  DEBUG_SERIAL.println("[PROV] =======================================");

  bool scOk = WiFi.beginSmartConfig();
  smartConfigActive = scOk;
  smartConfigDoneHandled = false;
  smartConfigStartMs = millis();

  if (scOk) {
    DEBUG_SERIAL.println("[PROV] SmartConfig 已启动 (AirKiss, 无限等待)");
    DEBUG_SERIAL.println("[PROV] 等待手机发送 Wi-Fi 凭据...");
    // SmartConfig 等待期间不启动 AP 模式，避免干扰 STA 连接
    ensureProvisionRoutes();
    diagnosticLogWifi("SmartConfig started");
  } else {
    DEBUG_SERIAL.println("[PROV] SmartConfig 启动失败，3秒后重试...");
    delay(3000);
    diagnosticRestart("smartconfig_start_failed");
  }
}

void handleProvisioningLoop() {
  if (smartConfigActive) {
    if (!smartConfigDoneHandled) {
      if (WiFi.smartConfigDone()) {
        smartConfigDoneHandled = true;

        String rcvdSSID = WiFi.SSID();
        String rcvdPsk = WiFi.psk();
        int pwdLen = rcvdPsk.length();

        DEBUG_SERIAL.println("");
        DEBUG_SERIAL.println("[PROV] ========== SmartConfig 收到凭据 ==========");
        DEBUG_SERIAL.println("[PROV] SSID: " + rcvdSSID);
        DEBUG_SERIAL.println("[PROV] 密码长度: " + String(pwdLen) + " 字符");
        DEBUG_SERIAL.println("[PROV] 当前 WiFi 模式: " + String(WiFi.getMode()));
        DEBUG_SERIAL.println("[PROV] 当前 WiFi 状态: " + String(wifiStatusToString(WiFi.status())));
        DEBUG_SERIAL.println("[PROV] MAC: " + WiFi.macAddress());
        DEBUG_SERIAL.println("[PROV] 开始连接等待 (最长 45 秒)...");
        DEBUG_SERIAL.println("[PROV] ===========================================");

        // SmartConfig 结束后确保 STA 模式
        WiFi.mode(WIFI_STA);

        const unsigned long SC_CONNECT_TIMEOUT = 45000;
        unsigned long connectStart = millis();
        wl_status_t lastStatus = WiFi.status();
        unsigned long lastStatusLog = 0;

        while (WiFi.status() != WL_CONNECTED && millis() - connectStart < SC_CONNECT_TIMEOUT) {
          wl_status_t currentStatus = WiFi.status();
          unsigned long elapsedSec = (millis() - connectStart) / 1000;

          // 每秒打印一次状态
          if (millis() - lastStatusLog >= 1000) {
            DEBUG_SERIAL.println("[PROV] [" + String(elapsedSec) + "s] 状态: " +
                                 String(wifiStatusToString(currentStatus)));
            lastStatusLog = millis();
          }

          // 检测状态变化并记录
          if (currentStatus != lastStatus) {
            DEBUG_SERIAL.println("[PROV] >>> 状态变化: " + String(wifiStatusToString(lastStatus)) +
                                 " -> " + String(wifiStatusToString(currentStatus)) +
                                 " (耗时 " + String(elapsedSec) + "s)");
            lastStatus = currentStatus;
          }

          delay(100);
          yield();
        }

        unsigned long totalElapsed = (millis() - connectStart) / 1000;

        if (WiFi.status() == WL_CONNECTED) {
          DEBUG_SERIAL.println("[PROV] SmartConfig 连接成功! IP: " + WiFi.localIP().toString() +
                               " (耗时 " + String(totalElapsed) + "s)");

          DeviceConfig newCfg;
          newCfg.ssid = rcvdSSID;
          newCfg.password = rcvdPsk;
          newCfg.serverHost = cfg.serverHost;
          newCfg.httpPort = cfg.httpPort;
          newCfg.wsPort = cfg.wsPort;
          ensureConfigDefaults(newCfg);
          cfg = newCfg;
          saveConfig(cfg);

          DEBUG_SERIAL.println("[PROV] 配置已保存，即将重启...");
          WiFi.stopSmartConfig();
          smartConfigActive = false;
          delay(500);
          diagnosticRestart("wifi_config_saved");
        } else {
          // ---- 连接失败，详细诊断 ----
          wl_status_t finalStatus = WiFi.status();
          DEBUG_SERIAL.println("");
          DEBUG_SERIAL.println("[PROV] ========== SmartConfig 连接失败 ==========");
          DEBUG_SERIAL.println("[PROV] 最终状态: " + String(wifiStatusToString(finalStatus)));
          DEBUG_SERIAL.println("[PROV] 总耗时: " + String(totalElapsed) + "s");
          DEBUG_SERIAL.println("[PROV] SSID: " + rcvdSSID);
          DEBUG_SERIAL.println("[PROV] 密码长度: " + String(pwdLen));
          DEBUG_SERIAL.println("[PROV] WiFi 模式: " + String(WiFi.getMode()));
          DEBUG_SERIAL.println("[PROV] MAC: " + WiFi.macAddress());

          // 根据最终状态给出诊断建议
          switch (finalStatus) {
            case WL_NO_SSID_AVAIL:
              DEBUG_SERIAL.println("[PROV] 诊断: SSID 不可用");
              DEBUG_SERIAL.println("[PROV]   - 热点可能不是 2.4GHz（ESP8266 不支持 5GHz）");
              DEBUG_SERIAL.println("[PROV]   - SSID 可能不存在或已关闭");
              DEBUG_SERIAL.println("[PROV]   - 手机热点可能不可见/未开启");
              DEBUG_SERIAL.println("[PROV]   - 隐藏 SSID 可能不被 SmartConfig 支持");
              break;
            case WL_CONNECT_FAILED:
              DEBUG_SERIAL.println("[PROV] 诊断: 连接失败(认证/关联失败)");
              DEBUG_SERIAL.println("[PROV]   - 密码可能错误");
              DEBUG_SERIAL.println("[PROV]   - 加密方式不兼容（ESP8266 支持 WPA/WPA2，不支持 WPA3）");
              DEBUG_SERIAL.println("[PROV]   - 热点可能使用 WPA3 或 WPA2/WPA3 混合加密");
              DEBUG_SERIAL.println("[PROV]   - 热点可能设置了 MAC 地址过滤");
              break;
            case WL_DISCONNECTED:
              DEBUG_SERIAL.println("[PROV] 诊断: 已断开连接");
              DEBUG_SERIAL.println("[PROV]   - 可能认证失败（密码错误）");
              DEBUG_SERIAL.println("[PROV]   - 热点可能拒绝连接");
              DEBUG_SERIAL.println("[PROV]   - 信号可能太弱");
              DEBUG_SERIAL.println("[PROV]   - 热点可能限制了连接设备数");
              break;
            case WL_IDLE_STATUS:
              DEBUG_SERIAL.println("[PROV] 诊断: 空闲状态(连接未开始或已超时)");
              DEBUG_SERIAL.println("[PROV]   - 热点兼容性问题或连接等待不足");
              DEBUG_SERIAL.println("[PROV]   - 热点可能繁忙或响应慢");
              break;
            default:
              DEBUG_SERIAL.println("[PROV] 诊断: 未知失败原因");
              DEBUG_SERIAL.println("[PROV]   - 请检查热点设置和兼容性");
              break;
          }
          DEBUG_SERIAL.println("[PROV] =============================================");
          DEBUG_SERIAL.println("");

          DEBUG_SERIAL.println("[PROV] 3秒后重启 SmartConfig...");
          delay(3000);
          diagnosticRestart("smartconfig_connect_failed");
        }
        return;
      }

      // SmartConfig 等待中，定期日志
      static unsigned long lastScLog = 0;
      if (millis() - lastScLog > 15000) {
        lastScLog = millis();
        unsigned long waitedSec = (millis() - smartConfigStartMs) / 1000;
        DEBUG_SERIAL.println("[PROV] SmartConfig 等待中... (已等待 " + String(waitedSec) + "s)");
      }

    }
  }
}

// ---- WiFi 保活 ----

bool ensureWiFiReady() {
  if (WiFi.status() == WL_CONNECTED) return true;

  broadcastIPCached = false;

  unsigned long reconnectStartedAt = millis();
  char disconnectMessage[80];
  snprintf(
    disconnectMessage,
    sizeof(disconnectMessage),
    "disconnected status=%d rssi=%d",
    (int)WiFi.status(),
    WiFi.RSSI()
  );
  diagnosticLogWifi(disconnectMessage, true);

  DEBUG_SERIAL.println("[WiFi] Disconnected, trying saved network...");
  if (connectSavedWiFi()) {
    broadcastIPCached = false;
    char recoveryMessage[64];
    snprintf(
      recoveryMessage,
      sizeof(recoveryMessage),
      "recovered after=%lums",
      millis() - reconnectStartedAt
    );
    diagnosticLogWifi(recoveryMessage);
    return true;
  }

  DEBUG_SERIAL.println("[WiFi] Saved network failed, starting parallel provisioning...");
  diagnosticLogWifi("saved network failed; provisioning", true);
  startParallelProvision();
  return false;
}
