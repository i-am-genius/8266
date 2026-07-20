# ESP8266 设备诊断日志增强设计

## 1. 目标

在不修改后端日志协议和运维页面的前提下，补齐 ESP8266 固件关键诊断日志，使线上日志能够回答以下问题：

- 设备为什么启动或重启。
- Wi-Fi、WebSocket、HTTP、传感器和 Nano 分别在哪一步失败或恢复。
- 灯光亮度、色温和自动模式是否真的发生变化，以及变化来自哪里。
- ARM 在高频控制期间执行了多少命令，最终停在什么位置。
- 日志是否因上传失败或缓冲区满而丢失。

本次继续使用现有 `module + msg + uptimeMs` NDJSON 格式，不修改 Spring Boot 后端和 Vue 运维页面。

## 2. 设计原则

1. `online_logger` 只负责日志缓冲和传输，不包含业务限流规则。
2. 新增 `diagnostic_logger` 作为语义事件层，集中处理去重、聚合、失败计数和周期调度。
3. 业务模块只报告真实事件，不各自实现计时器或重复抑制。
4. 保持 `LogEntry::message` 的 128 字节数组，不增加 32 条环形缓冲的静态内存占用。
5. 正常高频数据不上传；失败、恢复、状态边沿和聚合摘要才上传。
6. 不记录密码、上传密钥、完整 OTA 签名 URL、原始控制 JSON、逐条 Nano TX/RX 或逐包 UDP 坐标。

## 3. 架构

### 3.1 `online_logger`

保留现有环形缓冲、日志优先级和批量上传机制，增加以下能力：

- `bool uploadLogsBeforeRestart()`：忽略 10 秒调度间隔，立即上传最新一批日志，确保刚写入的 `REBOOT` 包含在批次中；仍受现有 HTTP 超时约束。
- 缓冲区丢弃计数：分别累计 DEBUG、INFO、WARN、ERROR 丢弃数量。
- 上传结果统计：累计连续失败数、总失败数、最后 HTTP 错误码。
- 上传恢复信号：失败后首次成功时向诊断层报告恢复摘要。
- 上传实现内部禁止调用 `LOG_*`，避免上传失败产生递归日志。

### 3.2 `diagnostic_logger`

新增 `include/diagnostics/diagnostic_logger.h` 和 `src/diagnostics/diagnostic_logger.cpp`，负责：

- 启动 reset reason 和堆内存诊断。
- 每 5 分钟健康摘要。
- 灯光控制状态变化去重。
- ARM 固定 60 秒窗口聚合。
- Wi-Fi、WebSocket、HTTP、传感器、Nano、配置和 tracking 事件格式化。
- 计划重启原因记录和立即上传。

限流与聚合的纯状态逻辑放在不依赖 Arduino 的 `diagnostic_policy` 中，以便使用 native 单元测试。

### 3.3 业务模块

现有模块在状态边沿调用诊断接口：

- `main.cpp`：启动诊断、周期任务。
- `wifi_manager.cpp`：断线、重连、配网和重启原因。
- `ws_client.cpp`：连接、断线、协议错误和控制变化。
- `http_reporter.cpp`：HTTP 结果和恢复。
- `sensor_manager.cpp`：初始化、连续无效读取和恢复。
- `arm_controller.cpp`：Nano 故障和 ARM 控制聚合输入。
- `config_manager.cpp`：配置加载、保存、解析、删除结果。
- `tracking_receiver.cpp`：tracking 生命周期和 UDP 解析异常。
- `local_server.cpp`：本地控制变化和本地重置原因。
- `ota_manager.cpp`：OTA 计划重启和立即上传。

## 4. 事件定义

### 4.1 启动与健康

启动后写入一条 `BOOT`：

```text
reset=Hardware Watchdog info=... heap=31200 max=24800 frag=11
```

字段来自 ESP8266 reset API、`ESP.getFreeHeap()`、`ESP.getMaxFreeBlockSize()` 和 `ESP.getHeapFragmentation()`。reset info 需要截断并移除换行，确保消息不超过 127 个字符。

每 5 分钟写入一条 `HEALTH`：

```text
up=300000 heap=29800 max=22100 frag=14 rssi=-57 wsRe=2 httpFail=1 drop=0
```

健康日志还包含当前有效亮度、色温和自动模式；字段使用短名称控制总长度。

### 4.2 灯光控制

`CONTROL` 覆盖 WebSocket `state/control`、本地 `/setLight` 和 `/lamp/control`。

只有最终有效状态发生变化时才记录：

```text
src=ws bri=80>60 temp=4000>5200 auto=1>0
```

规则：

- 同一个控制请求内多个字段变化只产生一条日志。
- 重复下发相同状态不产生日志。
- 自动调光内部每 2 秒计算得到的亮度不产生 `CONTROL` 日志。
- effect 和 locate 使用各自生命周期日志，不把动画逐帧变化记为控制日志。

### 4.3 ARM 控制聚合

首个 ARM 命令开启一个 60 秒滚动窗口，窗口期间的新命令只聚合、不延长截止时间。每个窗口累计：

- 命令总数。
- 来源集合：WebSocket、tracking、本地 HTTP。
- 最后动作类型。
- 最终 `pan`、`tilt`、`slider`。

窗口结束后输出一条 `ARM`：

```text
win=60s n=143 src=ws|track last=joystick pan=12 tilt=-8 slider=0
```

无命令窗口不输出。摇杆包、tracking 坐标和单次 Nano 指令不单独上传。窗口调度使用无符号 `millis()` 差值，支持约 49 天回绕。

### 4.4 网络和协议

- `WIFI`：运行期断线状态码、RSSI、重试次数、重连耗时、进入 SmartConfig 的原因。
- `WS`：连接、断线原因、连接持续时间、累计重连数、JSON 解析失败、未知消息类型和无效 OTA 参数。
- WebSocket ping/pong 不上传。
- 收到不属于本设备的广播消息不上传，避免多设备环境噪声。

### 4.5 HTTP

按 endpoint 统计连续失败：

- 第 1 次失败记录 WARN。
- 第 10、20、30 次等连续失败记录 WARN。
- 失败后的首次成功记录 INFO，包含失败次数和恢复耗时。
- 普通 2xx 成功不记录。

示例：

```text
ep=state-report code=-11 cost=2501ms fail=1
ep=state-report recovered fail=7 cost=84ms
```

日志上传 endpoint 自身不通过 HTTP 事件接口记录，防止递归。

### 4.6 传感器和 Nano

- BH1750、VL53L0X 初始化失败立即记录 `SENSOR` WARN，成功保持现有自检摘要即可。
- 连续 20 次无效 ToF 读取记录一次 WARN；恢复到有效值时记录一次 INFO。
- 连续 3 次无效 BH1750 读取记录一次 WARN；恢复时记录一次 INFO。
- Nano 启动同步超时、homing 失败、hall 读取失败、串口超长行和驱动恢复分别记录 `NANO`。
- Nano 正常 TX/RX 不上传。

### 4.7 配置和 tracking

- `CONFIG`：文件打开失败、JSON 解析失败、序列化失败、删除失败记录 WARN/ERROR；保存、删除成功仅在会引起重启时记录 INFO。
- `TRACK`：prepare、stop、timeout 记录生命周期日志；UDP JSON 解析失败记录首次和每连续 10 次，恢复时记录摘要。
- tracking 坐标和每秒状态消息不进入在线日志。

### 4.8 计划重启

所有固件主动重启统一走诊断函数：

```text
REBOOT reason=wifi_config_saved
REBOOT reason=wifi_config_cleared
REBOOT reason=smartconfig_start_failed
REBOOT reason=smartconfig_connect_failed
REBOOT reason=ota_success
```

流程为：写入 `REBOOT` -> 调用 `uploadLogsBeforeRestart()` -> 短暂 `yield()` -> `ESP.restart()`。

普通周期上传仍从最旧日志开始；重启专用上传选择最新 20 条并按产生时间排序，保证最新 `REBOOT` 在单个批次中。成功后无需保留未上传的旧日志，因为设备会立即重启；失败时不提前修改环形缓冲状态。

立即上传失败时仍继续重启，不能因为日志服务不可用阻塞设备。意外掉电、WDT 和异常崩溃依赖下一次启动的 reset reason 诊断。

## 5. 错误处理与资源约束

- 所有日志消息通过固定长度缓冲格式化，超长内容安全截断并保证 `\0` 结尾。
- `diagnostic_logger` 不持有动态日志列表，只保存计数器、时间戳和最后状态。
- HTTP 统计使用固定数量的已知 endpoint 槽位，不使用动态容器。
- 日志上传失败只更新计数器；恢复摘要在上传调用返回后进入下一批日志。
- 环形缓冲满时延续现有优先级替换策略，并准确累计被丢弃日志的级别。
- 计划重启 flush 最多尝试一个最新日志批次，避免 32 条队列导致多次 HTTP 阻塞，同时保证 `REBOOT` 被包含。

## 6. 测试策略

新增 PlatformIO native 测试环境，只编译纯 C++ `diagnostic_policy` 和缓冲策略测试目标。

测试必须先失败再实现，覆盖：

1. 相同灯光状态不产生事件。
2. 亮度、色温或自动模式任一变化只产生一条合并事件。
3. ARM 60 秒窗口聚合命令数、来源、最后动作和最终位置。
4. ARM 无命令窗口不输出，时间回绕仍能正确到期。
5. HTTP 第 1 次和每第 10 次连续失败需要记录，恢复只记录一次。
6. HEALTH 每 5 分钟到期一次，时间回绕正确。
7. 缓冲区满时丢弃计数与优先级替换一致。
8. `uploadLogsBeforeRestart()` 忽略周期门槛、选择包含 `REBOOT` 的最新批次，失败时不修改缓冲内容。

固件集成验证：

```powershell
pio test -e native
pio run -e esp12e
```

编译通过后检查固件尺寸和 RAM 使用量，确认新增静态状态不会显著挤压 ESP8266 可用堆。

## 7. 非目标

- 不修改后端日志 DTO、存储格式、权限或查询 API。
- 不修改运维页面显示字段和筛选功能。
- 不持久化崩溃前完整 RAM 日志到 LittleFS。
- 不上传高频传感器原始值、WebSocket 心跳、逐条 ARM 指令或 UDP 坐标。
- 不在本次范围内实现远程日志级别配置。

## 8. 验收标准

- 每次启动都能在服务器日志中看到 reset reason 和启动资源指标。
- 所有主动 `ESP.restart()` 前都有明确 `REBOOT reason`，网络可用时能立即上传。
- 灯光状态不变不产生日志，真实变化产生一条包含来源和前后值的日志。
- ARM 高频控制期间每分钟最多产生一条聚合日志。
- 网络、HTTP、传感器、Nano、配置和 tracking 的关键失败与恢复均可在线查看。
- 日志上传失败和缓冲丢弃可以通过恢复摘要或 HEALTH 指标观察。
- native 测试和 ESP8266 固件编译全部通过。
