# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## 沟通

始终使用中文沟通。

## 构建与烧录

```bash
# PlatformIO CLI (需安装 PlatformIO)
pio run              # 编译
pio run -t upload    # 编译并烧录 (串口)
pio device monitor   # 串口监视器 (115200 baud)
```

或使用 VS Code PlatformIO 扩展的图形界面。

## 项目概况

智慧服装店照明系统的 ESP8266 固件。单文件项目 (~1500 行 C++)，运行在 ESP-12E (NodeMCU) 模块上。

**硬件**: ESP8266 (ESP-12E), BH1750 光照传感器, VL53L0X ToF 测距传感器, 双色温 LED (冷白/暖白 PWM), 可选云台/滑轨电机 (通过 Arduino Nano 串口控制)

### 系统架构

| 子项目 | 路径 | 技术栈 |
|--------|------|--------|
| 后端 | `E:\smart-light-backend` | Spring Boot 4, Java 17 |
| Web 前端 | `E:\smart-light-front` | Vue 3, TypeScript, Vite |
| 小程序前端 | `E:\smart-light-mini` | uni-app, Vue 3 |
| ESP8266 固件 (本项目) | `E:\8266_OTA` | PlatformIO, Arduino, C++ |

## 引脚定义

```
LED_COLD_PIN  D2 (GPIO4)   — 冷白 LED PWM
LED_WARM_PIN  D1 (GPIO5)   — 暖白 LED PWM
BLUR          D7 (GPIO13)  — 散光/聚光控制 (数字输出)
TOF_SDA_PIN   D5 (GPIO14)  — I2C 数据 (VL53L0X + BH1750)
TOF_SCL_PIN   D6 (GPIO12)  — I2C 时钟
```

I2C 总线: `Wire.begin(D5, D6)`, 400kHz。BH1750 (0x23) 和 VL53L0X (0x29) 共享总线。
UART (Nano): `Serial1` (ESP8266 硬件串口), 57600 baud, 发送单字符命令控制电机。

## 固件入口

**单文件**: `src/main.cpp` (所有逻辑都在此文件)

- **`setup()`**: 初始化串口, 生成设备 ID (`lamp-{ESP.getChipId()}`), 挂载 LittleFS, 初始化传感器, 连接 WiFi, 连接 WebSocket, 发送上线通告
- **`loop()`**: HTTP 服务器轮询, Nano 响应轮询, WiFi 保活, WebSocket 心跳, UDP 广播 (每 5s), 光照上报 (每 30s), 灯光效果循环 / ToF 驱动更新

## 核心模块 (均在 main.cpp)

| 模块 | 关键函数 | 说明 |
|------|---------|------|
| WiFi | `connectSavedWiFi()`, `smartConfigProvision()`, `startConfigPortal()` | 三策略连接: 已保存配置 → SmartConfig → AP 热点配网 |
| 配置 | `saveConfig()`, `loadConfig()` | LittleFS `config.json` 持久化 |
| WebSocket | `beginWebSocketClient()`, `handleWsMessage()` | 连接 `ws://{host}/ws/device`, 处理 state/effect/locate/arm/ota 消息 |
| HTTP 上报 | `sendDeviceStateReport()`, `sendLightLevelToServer()`, `sendStayRecordToServer()` | 状态/光照/停留时长上报 |
| UDP 广播 | `broadcastDevice()` | 端口 4210 局域网发现 |
| LED 控制 | `applyLightSettings()` | PWM 双色温 (冷白/暖白), 色温 2700-6500K 映射 |
| Wave 效果 | `updateEffectLoop()` | 正弦波色温循环, 支持速度/相位/范围调节 |
| ToF 传感器 | `updateLightingByToF()` | 2m 内人体检测, 1s 防抖, 2s 平滑过渡, 停留时长计算 |
| BH1750 | `lightMeter.readLightLevel()` | 连续高分辨率模式, 每 30s 上报 |
| Nano 云台 | `handleArmAction()` | 方向/滑轨控制, Pan -90~90, Tilt -90~90, Slider 0-1200mm |
| OTA 升级 | `doOtaUpdate()` | 通过 WebSocket 触发, MD5 校验, 进度回调, 自动重启 |
| 本地 HTTP | ESP8266WebServer (端口 80) | `/status`, `/setLight`, `/stopBroadcast`, `/resumeBroadcast`, `/resetWifi` |

## 与后端通信协议

### WebSocket (`/ws/device`)

- **注册**: `{"type":"register","chipId":"lamp-XXX",...}`
- **心跳**: `{"type":"ping"}` (每 5s)
- **接收消息**: `state` (灯光控制), `effect` (灯效), `locate` (定位呼吸灯), `arm` (云台), `command`, `ota_update`

### HTTP 上报 (设备→服务器)

| 端点 | 频率 | 内容 |
|------|------|------|
| `POST /admin/device/announce` | 5s | 设备上线通告 |
| `POST /admin/device/state-report` | 状态变化时 | 完整设备状态 |
| `POST /admin/lux/create` | 30s | 光照值 (lux) |
| `POST /admin/duration/create` | 人离开时 | 停留时长 (ms) |

### UDP 广播
- 端口 4210, 每 5s 广播 JSON 设备信息 (局域网发现)

## 固件版本

当前版本: `1.0.2` (versionCode: `10002`)

## 相关文件

- `E:\8266_OTA\platformio.ini` — 构建配置、库依赖、链接器脚本
- `E:\8266_OTA\src\main.cpp` — 全部固件代码 (1499 行)
- `E:\light\PROJECT_FUNCTIONS.md` — 完整项目功能清单
- `E:\light\SYSTEM_ANALYSIS.md` — 系统架构分析文档
