#include "app_config.h"
#include "config/config_manager.h"
#include "network/wifi_manager.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "device/light_control.h"
#include "device/sensor_manager.h"
#include "device/ota_manager.h"
#include "device/arm_controller.h"
#include "device/self_test.h"
#include "server/local_server.h"
#include "network/udp_discovery.h"
#include "online_logger.h"
#include "diagnostics/diagnostic_logger.h"
#include "runtime/loop_policy.h"

// ==================== HTTP 烟雾测试 ====================
// 启用后固件只做 WiFi + HTTP announce，排除其他所有模块干扰


// ===================== 传感器 / 外设 实例 =====================
BH1750 lightMeter;
Adafruit_VL53L0X lox = Adafruit_VL53L0X();
WebSocketsClient webSocket;
ESP8266WebServer server(80);
WiFiUDP udp;

// ===================== 运行状态 =====================
bool bh1750Ready = false;
bool tofReady = false;
bool fsReady = false;
bool wsConnected = false;
bool selfTestDone = false;
bool selfTestFsOk = false;
bool selfTestWifiOk = false;
bool selfTestWsOk = false;
bool selfTestBh1750Ok = false;
bool selfTestTofOk = false;
bool selfTestNanoOk = false;
unsigned long selfTestCheckedAtMs = 0;
String selfTestNanoStatus = "not_run";
bool selfTestNanoHomingEnabled = false;
bool selfTestNanoHallReadEnabled = false;
bool enableBroadcast = true;
bool enableAnnounce = true;
bool backendDeviceAdded = false;
bool wsClientStarted = false;
bool provisioningMode = false;
bool smartConfigActive = false;
bool smartConfigDoneHandled = false;
unsigned long smartConfigStartMs = 0;
bool otaInProgress = false;
String firmwareChannel = FW_CHANNEL;
String otaStatus = "idle";
int otaProgress = 0;
int lastOtaProgressLog = -1;
int lastOtaProgressReport = -1;
unsigned long lastOtaProgressReportMs = 0;

unsigned long lastLightSend = 0;
unsigned long lastLightUpdate = 0;
unsigned long lastAnnounce = 0;
unsigned long lastBroadcast = 0;
unsigned long lastPing = 0;
unsigned long lastToFRead = 0;

bool pendingStateReport = false;
bool pendingStateReportWithSelfTest = false;
unsigned long lastStateReportFailedAt = 0;
unsigned long lastWsConnectedMs = 0;
bool bootOnlineReportRequested = false;
bool bootOnlineReportDone = false;
bool bootSelfTestStarted = false;
bool bootSelfTestReportDone = false;

IPAddress cachedBroadcastIP;
bool broadcastIPCached = false;

// ===================== 灯光控制参数 =====================
int brightness = 80;
int temp = 4000;
bool autoMode = true;
int recommendedBrightness = 80;
int recommendedTemp = 4000;
int luxAutoTarget = 300;
int luxAutoBrightness = 50;
char fabric[16] = "unknown";

bool effectWaveEnabled = false;
int effectBaseTemp = 3800;
int effectRange = 500;
float effectSpeed = 1.0f;
int effectBrightness = 80;
float effectPhaseOffset = 0.0f;
int effectRestoreBrightness = 80;
int effectRestoreTemp = 4000;
unsigned long effectStartMs = 0;
unsigned long lastEffectUpdateMs = 0;
const unsigned long WAVE_UPDATE_INTERVAL_MS = 80;

// ===================== Nano 云台 / 滑轨控制参数 =====================
int panDeg = 0;
int tiltDeg = 0;
int sliderMm = 0;
int angleStep = 5;
int sliderStep = 20;
int panSpeedDeg = 20;
int tiltSpeedDeg = 30;
int sliderSpeedMm = 80;

// ===================== 设备配置 =====================
DeviceConfig cfg;
String deviceId;

// ===================== setup / loop =====================
void setup() {
  Serial.begin(NANO_BAUD);
  DEBUG_SERIAL.begin(115200);
  delay(200);

  logInit();

  // Match the working 8266test startup path: prime Nano before WiFi/sensors.
  sendNano('m', "16");
  applyArmSpeed(currentArmSpeed);

  deviceId = makeDeviceId();

  DEBUG_SERIAL.println("\n========================");
  DEBUG_SERIAL.println("设备启动");
  DEBUG_SERIAL.println("ID = " + deviceId);
  DEBUG_SERIAL.println("FW = " + String(FW_VERSION));
  DEBUG_SERIAL.println("========================");

  LOG_INFO("BOOT", String("设备启动 ID=" + deviceId + " FW=" + String(FW_VERSION)).c_str());

  if (!LittleFS.begin()) {
    DEBUG_SERIAL.println("[FS] LittleFS 挂载失败");
    LOG_ERROR("BOOT", "LittleFS 挂载失败");
  } else {
    fsReady = true;
    LOG_INFO("BOOT", "LittleFS 挂载成功");
  }

  setupHardwareAndSensors();

  // Nano 的启动通常比 ESP 稍慢，这里延时重发几轮初始化，
  // 以避免首轮 m/s/S/X 在 Nano 串口尚未就绪时丢掉。
  scheduleNanoStartupSync();
  bool hasConfig = loadConfig();
  bool wifiOk = false;

  // Set log server after config is loaded
  String logHost = cfg.serverHost.length() > 0 ? cfg.serverHost : String(DEFAULT_SERVER_HOST);
  uint16_t logPort = cfg.httpPort > 0 ? cfg.httpPort : DEFAULT_HTTP_PORT;
  const char* logSecret = cfg.uploadSecret.length() > 0 ? cfg.uploadSecret.c_str() : nullptr;
  logSetServer(logHost.c_str(), logPort, logSecret);
  logSetDeviceId(deviceId.c_str());
  diagnosticInit();

  if (hasConfig) {
    DEBUG_SERIAL.println("[BOOT] Saved config found, trying to connect...");
    LOG_INFO("BOOT", "尝试连接已保存的 WiFi");
    wifiOk = connectSavedWiFi();
  } else {
    DEBUG_SERIAL.println("[BOOT] No saved config, entering parallel provisioning...");
    LOG_INFO("BOOT", "无已保存配置，进入配网模式");
  }

  if (!wifiOk) {
    LOG_WARN("BOOT", "WiFi 连接失败，进入配网模式");
    startParallelProvision();
    return;
  }

  LOG_INFO("BOOT", String("WiFi 已连接 IP=" + WiFi.localIP().toString()).c_str());

  broadcastIPCached = false;
  setupDeviceHttpServer();
  sendAnnounce();
  if (backendDeviceAdded) {
    LOG_INFO("BOOT", "开始连接 WebSocket");
    beginWebSocketClient();
  } else {
    DEBUG_SERIAL.println("[WS] skip connect until backend announce returns added=true");
    LOG_INFO("BOOT", "等待后端 announce 确认后再连接 WS");
  }
}

void loop() {
  server.handleClient();
  pollNano();
  handleNanoStartupSync();

  if (provisioningMode) {
    handleProvisioningLoop();
    return;
  }

  if (!ensureWiFiReady()) return;

  if (wsClientStarted) {
    webSocket.loop();
    handleWsHeartbeat();
  }

  if (otaInProgress) return;

  // 摇杆连续运动更新 (每帧)
  updateArmJoystickMotion();
  handleSelfTestTask();
  const bool runNonCriticalHttp = shouldRunNonCriticalHttp(effectWaveEnabled);
  if (runNonCriticalHttp) {
    handleDeviceStateReportTask();
  }
  handleLocateBreathTask();

  broadcastDevice();

  if (!isLocateBreathActive()) {
    if (effectWaveEnabled) {
      updateEffectLoop();
    } else {
      updateLightingByToF();
    }
  }

  unsigned long now = millis();

  if (runNonCriticalHttp && now - lastAnnounce > announceInterval) {
    lastAnnounce = now;
    sendAnnounce();
    if (backendDeviceAdded && !wsClientStarted) {
      beginWebSocketClient();
    }
  }

  if (runNonCriticalHttp && now - lastLightSend > lightSendInterval) {
    lastLightSend = now;
    sendLightLevelToServer();
  }

  diagnosticHandlePeriodic();
  if (runNonCriticalHttp) {
    uploadLogs();
  }
}
