# ESP8266 在线日志系统设计文档

## 概述

为智慧服装店照明系统的 ESP8266 固件添加在线日志功能，替代串口调试，方便远程查看设备运行日志。

### 背景

当前 ESP8266 固件使用 `debuglog` 和 `nanolog` 进行调试，需要连接串口才能查看日志。这种方式在现场调试时非常不便，需要物理接触设备。

### 目标

1. 实现日志的远程上传和在线查看
2. 保持原有串口调试能力
3. 支持按设备、级别、模块、关键词筛选
4. 支持自动刷新，方便实时调试

### 技术方案

采用 **HTTP 批量上报 + JSONL 文件存储 + 前端日志面板** 方案。

---

## 1. 整体架构

```
┌─────────────┐      HTTP POST (每10s)      ┌─────────────┐
│   ESP8266   │ ──────────────────────────→ │   后端      │
│   固件      │    /admin/device/logs/batch │   (Spring Boot)
│             │                             │             │
│  日志缓冲区 │                             │  日志服务    │
│  (环形队列) │                             │  写入文件    │
└─────────────┘                             └──────┬──────┘
                                                   │
                                                   │ 读取文件
                                                   ▼
                                            ┌─────────────┐
                                            │   前端      │
                                            │ (Vue 3)     │
                                            │             │
                                            │  日志面板    │
                                            │  筛选/搜索  │
                                            └─────────────┘
```

**核心流程**：

1. ESP8266 在内存中维护固定容量的环形日志缓冲区，收集运行日志
2. 每 10 秒将缓冲区中待上报日志批量 POST 到后端
3. 后端根据 chipId 和日期，将日志追加写入对应 JSON Lines 文件
4. 前端调用后端日志查询接口，后端读取文件并返回过滤后的日志数据
5. 前端日志面板负责展示、搜索、筛选和自动刷新

---

## 2. 日志数据格式

### 2.1 ESP8266 端日志条目格式

每条日志在 ESP8266 内存中的结构：

```cpp
struct LogEntry {
    unsigned long uptimeMs;        // millis()，设备启动后的毫秒数
    uint32_t seq;                  // 日志序号，用于排序/去重
    uint8_t level;                 // 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR
    char module[12];               // 模块标识
    char message[128];             // 日志内容，超长截断
};
```

**内存占用**：约 148 字节/条，50 条 ≈ 7.4KB

### 2.2 HTTP 批量上报格式

**请求**：`POST /admin/device/logs/batch?chipId=LAMP-37461B&uploadUptimeMs=252220000`

**请求头**：
```
Content-Type: application/x-ndjson
X-Upload-Secret: {upload-secret}
```

**请求体**：每行一个 JSON 对象（NDJSON 格式）

```json
{"uptimeMs":252215000,"seq":1001,"level":"INFO","module":"WIFI","msg":"Connected to somebody的iPhone"}
{"uptimeMs":252216000,"seq":1002,"level":"DEBUG","module":"SENSOR","msg":"BH1750 lux=320"}
```

**字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| uptimeMs | long | 设备启动后的毫秒数（**不是真实时间戳**） |
| seq | int | 日志序号，递增 |
| level | String | DEBUG / INFO / WARN / ERROR |
| module | String | 模块标识，12 字符以内 |
| msg | String | 日志内容，128 字符以内 |

**重要说明**：ESP8266 没有可靠 RTC，**不发送真实时间戳**。`uptimeMs` 是设备启动后的毫秒数，由后端在接收时用服务器时间回推真实时间戳。

### 2.3 JSONL 文件格式

后端存储时，每条日志追加一行 JSON：

```json
{"ts":1720252215000,"uptimeMs":252215000,"seq":1001,"level":"INFO","module":"WIFI","msg":"Connected to somebody的iPhone"}
{"ts":1720252216000,"uptimeMs":252216000,"seq":1002,"level":"DEBUG","module":"SENSOR","msg":"BH1750 lux=320"}
```

**字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| ts | long | **后端生成**的 Unix 毫秒时间戳，用于前端展示和筛选 |
| uptimeMs | long | ESP8266 启动后的毫秒数，用于排查设备运行状态 |
| seq | int | 设备端日志序号，用于排序/去重 |
| level | String | DEBUG / INFO / WARN / ERROR |
| module | String | WIFI / WS / SENSOR / OTA / LIGHT / SYSTEM |
| msg | String | 日志内容 |

**文件路径**：`/logs/{chipId}/{yyyy-MM-dd}.jsonl`

**时间戳生成规则**：

后端在写入 JSONL 文件时，使用以下公式回推每条日志的真实时间戳：

```java
ts = serverNow - (uploadUptimeMs - entry.uptimeMs)
```

其中：
- `serverNow`：服务器当前时间（System.currentTimeMillis()）
- `uploadUptimeMs`：ESP8266 上传时的 uptimeMs（URL 参数）
- `entry.uptimeMs`：每条日志记录时的 uptimeMs

**异常保护**：
- `uptimeMs` 为空 → 使用 `serverNow`
- `uploadUptimeMs <= 0` → 使用 `serverNow`
- `entry.uptimeMs > uploadUptimeMs` → 使用 `serverNow`
- `delta > 24 小时` → 使用 `serverNow`

**优势**：同一批上传的多条日志能保留相对时间顺序

---

## 3. ESP8266 端日志缓冲与上报机制

### 3.1 环形缓冲区设计

```cpp
#define LOG_BUFFER_CAPACITY 50
#define LOG_MSG_MAX_LEN 128
#define LOG_MODULE_MAX_LEN 12

struct LogEntry {
    unsigned long uptimeMs;
    uint32_t seq;
    uint8_t level;
    char module[LOG_MODULE_MAX_LEN];
    char message[LOG_MSG_MAX_LEN];
};

// 固定数组队列（非环形队列），简单稳定
LogEntry logBuffer[LOG_BUFFER_CAPACITY];
uint8_t logCount = 0;  // 当前有效日志数量
uint32_t logSeq = 0;   // 日志递增序号
```

### 3.2 写入策略

缓冲区未满：直接写入

缓冲区已满：
- 新日志是 DEBUG/INFO：直接丢弃新日志
- 新日志是 WARN/ERROR：尝试替换一条 DEBUG/INFO
- 如果缓冲区里全是 WARN/ERROR：丢弃新日志

### 3.3 上报流程

```cpp
unsigned long lastLogUploadMs = 0;
const unsigned long LOG_UPLOAD_INTERVAL_MS = 10000; // 10秒

void uploadLogs() {
    // 只在 WiFi 已连接、后端地址已配置时上传
    if (!wsConnected || cfg.serverHost.isEmpty()) return;
    if (millis() - lastLogUploadMs < LOG_UPLOAD_INTERVAL_MS) return;
    if (logCount == 0) return;
    
    // 固定本次上传的 uptimeMs，body 和 URL 都基于这个值
    unsigned long uploadUptimeMs = millis();
    lastLogUploadMs = uploadUptimeMs;
    
    // 每批最多上传 20 条，避免 body 过大
    uint8_t uploadCount = min(logCount, (uint8_t)20);
    
    // 构造 NDJSON body
    String body;
    body.reserve(4096);
    
    for (uint8_t i = 0; i < uploadCount; i++) {
        LogEntry& entry = logBuffer[i];
        body += "{\"uptimeMs\":" + String(entry.uptimeMs);
        body += ",\"seq\":" + String(entry.seq);
        body += ",\"level\":\"" + levelToString(entry.level) + "\"";
        body += ",\"module\":\"" + String(entry.module) + "\"";
        body += ",\"msg\":\"" + escapeJson(String(entry.message)) + "\"";
        body += "}\n";
    }
    
    // POST 到后端，使用固定的 uploadUptimeMs
    String url = "/admin/device/logs/batch?chipId=" + deviceId 
                 + "&uploadUptimeMs=" + String(uploadUptimeMs);
    
    int httpCode = postNdjsonToServer(url, body);
    
    // 只有 HTTP 返回 2xx 成功后，才清空已上传的日志
    if (httpCode >= 200 && httpCode < 300) {
        // 移除已上传的日志
        for (uint8_t i = uploadCount; i < logCount; i++) {
            logBuffer[i - uploadCount] = logBuffer[i];
        }
        logCount -= uploadCount;
        
        DEBUG_SERIAL.printf("[LOG] Uploaded %d logs, remaining %d\n", uploadCount, logCount);
    } else {
        DEBUG_SERIAL.printf("[LOG] Upload failed, HTTP %d, keeping logs\n", httpCode);
    }
}
```

### 3.4 日志宏定义

```cpp
#define LOG_DEBUG(module, msg) logWrite(0, module, msg)
#define LOG_INFO(module, msg)  logWrite(1, module, msg)
#define LOG_WARN(module, msg)  logWrite(2, module, msg)
#define LOG_ERROR(module, msg) logWrite(3, module, msg)

void logWrite(uint8_t level, const char* module, const char* msg) {
    LogEntry entry;
    entry.uptimeMs = millis();
    entry.seq = ++logSeq;
    entry.level = level;
    strncpy(entry.module, module, LOG_MODULE_MAX_LEN - 1);
    entry.module[LOG_MODULE_MAX_LEN - 1] = '\0';
    strncpy(entry.message, msg, LOG_MSG_MAX_LEN - 1);
    entry.message[LOG_MSG_MAX_LEN - 1] = '\0';
    
    if (logCount < LOG_BUFFER_CAPACITY) {
        logBuffer[logCount++] = entry;
    } else {
        // 按优先级替换
        replaceLowestPriority(entry);
    }
    
    // 同时输出到 Serial1（保持原有串口调试能力）
    DEBUG_SERIAL.printf("[%s][%s] %s\n", levelStr(level), module, msg);
}
```

### 3.5 与现有日志系统的兼容

- **保持串口输出**：所有日志同时输出到 Serial1，不影响现有调试方式
- **渐进式替换**：现有代码中的 `DEBUG_SERIAL.printf()` 可以逐步替换为 `LOG_xxx()` 宏
- **条件编译**：可以通过宏开关禁用日志上传功能

### 3.6 初始日志点位

第一版只在关键位置添加日志：

- WiFi 连接/断开
- WebSocket 连接/断开
- 设备注册成功/失败
- 传感器异常
- OTA 开始/成功/失败
- 灯光控制命令接收
- HTTP 日志上传成功/失败

---

## 4. 后端日志接收、文件写入和查询接口

### 4.1 日志接收接口

**接口**：`POST /admin/device/logs/batch`

**请求参数**：

| 参数 | 位置 | 类型 | 说明 |
|------|------|------|------|
| chipId | query | String | 设备 ID，如 "LAMP-37461B" |
| uploadUptimeMs | query | long | 上传时设备的 uptimeMs |

**请求头**：
```
Content-Type: application/x-ndjson
```

**请求体**：每行一个 JSON 对象

**响应**：
```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "received": 20,
    "saved": 18,
    "invalid": 2
  }
}
```

### 4.2 文件写入逻辑

```java
public void writeLogs(String chipId, long uploadUptimeMs, List<LogEntry> entries) {
    // 1. 校验 chipId（只允许 A-Z, a-z, 0-9, -, _）
    String safeChipId = validateChipId(chipId);
    if (safeChipId == null) {
        throw new ServiceException(400, "Invalid chipId");
    }
    
    // 2. 计算服务器真实时间
    long serverTimeMs = System.currentTimeMillis();
    
    // 3. 遍历日志条目
    for (LogEntry entry : entries) {
        // 时间换算
        long ts = serverTimeMs - (uploadUptimeMs - entry.getUptimeMs());
        
        // 边界保护
        if (entry.getUptimeMs() > uploadUptimeMs) {
            ts = serverTimeMs; // 异常情况使用服务器时间
        }
        
        // 根据换算后的 ts 决定文件日期
        String dateStr = LocalDate.ofInstant(
            Instant.ofEpochMilli(ts), 
            ZoneId.systemDefault()
        ).format(DateTimeFormatter.ISO_LOCAL_DATE);
        
        // 构建文件路径并 normalize
        Path path = Paths.get(logRoot, safeChipId, dateStr + ".jsonl").normalize();
        
        // 安全检查：确保路径仍在 logRoot 下，防止路径穿越
        if (!path.startsWith(Paths.get(logRoot).normalize())) {
            throw new ServiceException(400, "Invalid path");
        }
        
        // 确保目录存在
        Files.createDirectories(path.getParent());
        
        // 追加写入文件（带设备级锁）
        writeToFile(path, entry, ts);
    }
}

private String validateChipId(String chipId) {
    if (chipId == null || chipId.isEmpty()) return null;
    // 只允许 A-Z, a-z, 0-9, -, _
    if (!chipId.matches("^[A-Za-z0-9_-]+$")) return null;
    return chipId.toUpperCase(); // 统一转大写
}
```

### 4.3 NDJSON 解析和校验

后端接收到 body 后，逐行解析并校验：

```java
public List<LogEntry> parseNdjson(String body) {
    List<LogEntry> entries = new ArrayList<>();
    
    for (String line : body.split("\n")) {
        line = line.trim();
        if (line.isEmpty()) continue;
        
        try {
            JSONObject json = JSON.parseObject(line);
            
            // 校验必填字段
            if (!json.containsKey("uptimeMs") || !json.containsKey("seq") ||
                !json.containsKey("level") || !json.containsKey("module") ||
                !json.containsKey("msg")) {
                invalidCount++;
                continue;
            }
            
            // 校验 level 合法性
            String level = json.getString("level");
            if (!VALID_LEVELS.contains(level)) {
                invalidCount++;
                continue;
            }
            
            // 校验长度
            String module = json.getString("module");
            String msg = json.getString("msg");
            if (module.length() > 12 || msg.length() > 128) {
                invalidCount++;
                continue;
            }
            
            entries.add(new LogEntry(json));
        } catch (Exception e) {
            invalidCount++;
        }
    }
    
    return entries;
}
```

### 4.4 日志查询接口

**接口**：`GET /admin/device/logs/query`

**请求参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| chipId | String | 是 | 设备 ID |
| date | String | 否 | 日期，格式 yyyy-MM-dd，默认今天 |
| level | String | 否 | 日志级别筛选，多个用逗号分隔 |
| module | String | 否 | 模块筛选，多个用逗号分隔 |
| keyword | String | 否 | 关键词搜索 |
| startTime | Long | 否 | 开始时间戳（毫秒） |
| endTime | Long | 否 | 结束时间戳（毫秒） |
| limit | Integer | 否 | 返回条数，默认 100，最大 500 |
| order | String | 否 | 排序，asc/desc，默认 desc |
| beforeTs | Long | 否 | 用于分页，返回 ts < beforeTs 的日志（配合 order=desc 使用） |

**分页说明**：

- 第一次查询：不传 `beforeTs`，返回最新的 `limit` 条日志
- 加载更多：传入当前列表最后一条日志的 `ts`，返回更早的日志
- 示例：`GET /admin/device/logs/query?chipId=xxx&limit=100&order=desc&beforeTs=1720252215000`

**响应**：
```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "total": 1250,
    "hasMore": true,
    "logs": [
      {
        "ts": 1720252215000,
        "uptimeMs": 252215000,
        "seq": 1001,
        "level": "INFO",
        "module": "WIFI",
        "msg": "Connected to somebody的iPhone"
      }
    ]
  }
}
```

**时间范围查询逻辑**：

- 有 date：只查指定日期文件
- 没有 date，但有 startTime/endTime：根据时间范围读取涉及到的多个日期文件
- 都没有：默认查今天

### 4.5 设备列表接口

**接口**：`GET /admin/device/logs/devices`

**响应**：
```json
{
  "code": 0,
  "msg": "success",
  "data": [
    {
      "chipId": "LAMP-37461B",
      "deviceType": "lamp",
      "lastLogTime": 1720252216000
    }
  ]
}
```

### 4.6 后端配置

```yaml
smart-light:
  logs:
    root-dir: ./logs
    retention-days: 7
    max-batch-lines: 20
    max-line-length: 512
```

### 4.7 日志清理任务

```java
@Scheduled(cron = "0 0 2 * * ?") // 每天凌晨 2 点执行
public void cleanupOldLogs() {
    LocalDate cutoffDate = LocalDate.now().minusDays(retentionDays);
    
    Path logsDir = Paths.get(logRoot);
    if (!Files.exists(logsDir)) return;
    
    // 遍历所有设备目录，使用 try-with-resources 确保目录流关闭
    try (Stream<Path> deviceDirs = Files.list(logsDir)) {
        deviceDirs.filter(Files::isDirectory).forEach(deviceDir -> {
            // 删除过期的日志文件
            try (Stream<Path> logFiles = Files.list(deviceDir)) {
                logFiles.filter(path -> {
                    String fileName = path.getFileName().toString();
                    if (!fileName.endsWith(".jsonl")) return false;
                    try {
                        LocalDate fileDate = LocalDate.parse(fileName.replace(".jsonl", ""));
                        return fileDate.isBefore(cutoffDate);
                    } catch (DateTimeParseException e) {
                        return false; // 文件名格式不正确，跳过
                    }
                }).forEach(path -> {
                    try {
                        Files.delete(path);
                        log.info("Deleted old log file: {}", path);
                    } catch (IOException e) {
                        log.warn("Failed to delete log file: {}", path, e);
                    }
                });
            } catch (IOException e) {
                log.warn("Failed to list log files in: {}", deviceDir, e);
            }
        });
    } catch (IOException e) {
        log.warn("Failed to list device directories in: {}", logsDir, e);
    }
}
```

### 4.8 安全策略

接口 `/admin/device/logs/batch` 需要特殊处理，第一版采用 **deviceSecret 鉴权**。

**后端安全实现**：

由于 `/admin/**` 可能已有 JWT 拦截器，需要对 `/admin/device/logs/batch` 单独处理：

**方案 A：Spring Security 单独放行 + DeviceSecretFilter**
```java
// SecurityConfig.java
.requestMatchers("/admin/device/logs/batch").permitAll()  // 放行日志上传接口
```

然后创建 `DeviceSecretFilter`，对该接口进行 deviceSecret 校验：
```java
@Component
public class DeviceSecretFilter extends OncePerRequestFilter {
    @Override
    protected void doFilterInternal(HttpServletRequest request, ...) {
        if (request.getRequestURI().equals("/admin/device/logs/batch")) {
            String authHeader = request.getHeader("Authorization");
            if (authHeader == null || !authHeader.startsWith("Bearer ")) {
                response.setStatus(401);
                return;
            }
            String secret = authHeader.substring(7);
            // 校验 deviceSecret 是否有效
            if (!deviceLogService.validateDeviceSecret(secret)) {
                response.setStatus(401);
                return;
            }
        }
        filterChain.doFilter(request, response);
    }
}
```

**方案 B：在 DeviceLogController 中手动校验**
```java
@PostMapping("/batch")
public CommonResult<LogBatchResult> batch(
        @RequestParam String chipId,
        @RequestParam long uploadUptimeMs,
        @RequestHeader("Authorization") String authHeader,
        @RequestBody String body) {
    // 校验 Authorization header
    if (authHeader == null || !authHeader.startsWith("Bearer ")) {
        throw new ServiceException(401, "Unauthorized");
    }
    String secret = authHeader.substring(7);
    if (!deviceLogService.validateDeviceSecret(chipId, secret)) {
        throw new ServiceException(401, "Invalid device secret");
    }
    // 处理日志上传
    ...
}
```

**推荐方案 A**，更符合 Spring Security 的设计模式。

**deviceSecret 来源**：

1. **设备注册时生成**：设备首次绑定/注册时，后端生成 `deviceSecret`
2. **存储方式**：后端保存 `deviceSecret` 的哈希值（如 BCrypt），不保存明文
3. **下发给设备**：设备注册成功后，通过 WebSocket 或 HTTP 响应将 `deviceSecret` 下发给 ESP8266
4. **本地保存**：ESP8266 将 `deviceSecret` 保存到 LittleFS 的 `config.json` 中
5. **重新生成**：设备重置或重新绑定时，可重新生成 `deviceSecret`

**deviceSecret 格式建议**：
```
长度：32 字符
字符集：A-Z, a-z, 0-9
示例：aB3dE5fG7hI9jK1lM3nO5pQ7rS9tU1v
```

**请求头示例**：
```
Content-Type: application/x-ndjson
Authorization: Bearer aB3dE5fG7hI9jK1lM3nO5pQ7rS9tU1v
```

---

## 5. 前端日志面板设计

### 5.1 页面布局

```
┌─────────────────────────────────────────────────────────────┐
│  设备日志面板                                                │
├─────────────────────────────────────────────────────────────┤
│  [设备选择▼] [日期📅] [级别▼] [模块▼] [关键词搜索    ] [刷新🔄] │
├─────────────────────────────────────────────────────────────┤
│  日志列表                                                    │
│  ┌─────────────────────────────────────────────────────────┐│
│  │ 10:30:15 [INFO ] WIFI   Connected to somebody的iPhone  ││
│  │ 10:30:16 [DEBUG] SENSOR BH1750 lux=320                 ││
│  │ 10:30:17 [WARN ] WS     WebSocket reconnecting...      ││
│  │ 10:30:18 [ERROR] OTA    Update failed: timeout          ││
│  │ ...                                                     ││
│  └─────────────────────────────────────────────────────────┘│
│                                                             │
│  [加载更多]                                                  │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 筛选控件

| 控件 | 类型 | 说明 |
|------|------|------|
| 设备选择 | 下拉框 | 从 `/admin/device/logs/devices` 获取设备列表 |
| 日期 | 日期选择器 | 默认今天，可选历史日期 |
| 级别 | 多选框 | DEBUG / INFO / WARN / ERROR，默认全选 |
| 模块 | 多选框 | WIFI / WS / SENSOR / OTA / LIGHT / SYSTEM，默认全选 |
| 关键词 | 输入框 | 模糊搜索日志内容，带防抖（300ms） |
| 刷新 | 按钮 + 自动刷新 | 手动刷新 + 可开启自动刷新（每 10 秒） |

### 5.3 日志展示格式

每行日志格式：
```
{时间} [{级别}] {模块} {消息}
```

**级别颜色**（低饱和标签）：

| 级别 | 颜色 |
|------|------|
| DEBUG | 灰色 |
| INFO | 蓝灰色 |
| WARN | 琥珀/黄色 |
| ERROR | 红色 |

**模块颜色**：弱化处理，不使用强颜色，保持企业级简约风格。

### 5.4 自动刷新功能

```typescript
const autoRefresh = ref(false);
const refreshInterval = ref(10000); // 10秒
const loading = ref(false);
const hasMore = ref(true);
let refreshTimer: number | null = null;

function toggleAutoRefresh() {
  if (autoRefresh.value) {
    refreshTimer = window.setInterval(fetchLatestLogs, refreshInterval.value);
  } else {
    if (refreshTimer) {
      window.clearInterval(refreshTimer);
      refreshTimer = null;
    }
  }
}

// 获取最新日志（自动刷新时使用）
async function fetchLatestLogs() {
  if (loading.value) return; // 避免请求重叠
  loading.value = true;
  
  try {
    const params = {
      chipId: selectedDevice.value,
      date: selectedDate.value,
      level: selectedLevels.value.join(','),
      module: selectedModules.value.join(','),
      keyword: keyword.value,
      limit: 100,
      order: 'desc'
    };
    
    const response = await api.get('/admin/device/logs/query', { params });
    logs.value = response.data.logs;
    hasMore.value = response.data.hasMore;
  } finally {
    loading.value = false;
  }
}

// 加载更多历史日志
async function loadMoreLogs() {
  if (loading.value || !hasMore.value || logs.value.length === 0) return;
  loading.value = true;
  
  try {
    const lastTs = logs.value[logs.value.length - 1].ts;
    const params = {
      chipId: selectedDevice.value,
      date: selectedDate.value,
      level: selectedLevels.value.join(','),
      module: selectedModules.value.join(','),
      keyword: keyword.value,
      limit: 100,
      order: 'desc',
      beforeTs: lastTs  // 传入最后一条日志的 ts
    };
    
    const response = await api.get('/admin/device/logs/query', { params });
    logs.value = [...logs.value, ...response.data.logs];
    hasMore.value = response.data.hasMore;
  } finally {
    loading.value = false;
  }
}

// 组件卸载时清除定时器
onBeforeUnmount(() => {
  if (refreshTimer) {
    window.clearInterval(refreshTimer);
    refreshTimer = null;
  }
});
```

### 5.5 日志详情

点击某条日志可以展开详情，显示完整信息：

```
设备 ID: LAMP-37461B
服务器时间: 2026-07-06 10:30:15
设备 uptime: 252215000ms (4分12秒)
序号: 1001
级别: INFO
模块: WIFI
消息: Connected to somebody的iPhone
原始 JSON: {"ts":1720252215000,"uptimeMs":252215000,"seq":1001,"level":"INFO","module":"WIFI","msg":"Connected to somebody的iPhone"}
```

### 5.6 API 调用封装

```typescript
// src/api/log.ts
import request from '@/utils/request';

export interface LogEntry {
  ts: number;
  uptimeMs: number;
  seq: number;
  level: string;
  module: string;
  msg: string;
}

export interface LogQueryParams {
  chipId: string;
  date?: string;
  level?: string;
  module?: string;
  keyword?: string;
  startTime?: number;
  endTime?: number;
  limit?: number;
  order?: 'asc' | 'desc';
  beforeTs?: number;  // 用于分页，返回 ts < beforeTs 的日志
}

export interface LogQueryResult {
  total: number;
  hasMore: boolean;
  logs: LogEntry[];
}

export interface DeviceLogInfo {
  chipId: string;
  deviceType?: string;
  lastLogTime?: number;
}

// 查询日志
export function getLogs(params: LogQueryParams) {
  return request.get<LogQueryResult>('/admin/device/logs/query', { params });
}

// 获取设备列表
export function getLogDevices() {
  return request.get<DeviceLogInfo[]>('/admin/device/logs/devices');
}
```

### 5.7 状态处理

前端页面需要处理以下状态：

- 没有选择设备
- 当前日期无日志
- 请求失败
- 后端日志文件不存在
- 自动刷新中

---

## 6. 实施步骤和文件改动清单

### 6.1 实施顺序

```
阶段 1：确定接口协议和数据格式
  ↓
阶段 2：后端日志接收、写入、查询接口
  ↓
阶段 3：前端日志面板
  ↓
阶段 4：ESP8266 固件接入
  ↓
阶段 5：联调测试
```

**建议后端先做**，可以先用 Postman/curl 模拟 ESP8266 上传 NDJSON，确认后端写文件和查询都正常后，再让 ESP8266 真机接入。

### 6.2 文件改动清单

#### 阶段 2：后端接口实现

| 文件 | 改动内容 |
|------|----------|
| 新增 `controller/admin/device/DeviceLogController.java` | 日志接收、查询、设备列表接口 |
| 新增 `service/device/DeviceLogService.java` | 解析 NDJSON、写文件、查询过滤 |
| 新增 `config/DeviceLogProperties.java` | 读取日志配置 |
| 新增 `dto/device/DeviceLogEntryDTO.java` | 日志条目 DTO |
| 新增 `dto/device/DeviceLogQueryDTO.java` | 查询参数 DTO |
| 新增 `task/DeviceLogCleanupTask.java` | 定时清理过期日志 |
| 修改 `application.yml` | 添加日志配置 |

#### 阶段 3：前端日志面板

| 文件 | 改动内容 |
|------|----------|
| 新增 `src/api/log.ts` | 日志 API 封装 |
| 新增 `src/views/DeviceLogs.vue` | 日志面板页面 |
| 修改 `src/router/index.ts` | 添加日志路由 |
| 修改 `src/components/layout/SidebarNav.vue` | 添加日志菜单项 |

#### 阶段 4：ESP8266 固件接入

| 文件 | 改动内容 |
|------|----------|
| 新增 `include/online_logger.h` | 日志结构、宏、函数声明 |
| 新增 `src/online_logger.cpp` | 环形缓冲、写入、NDJSON 构造、上传 |
| 修改 `src/main.cpp` | 初始化日志系统，在 `loop()` 调用 `uploadLogs()` |
| 修改各模块文件 | 逐步将 `DEBUG_SERIAL.printf()` 替换为 `LOG_xxx()` 宏 |

### 6.3 阶段 2 详细步骤

**步骤 2.1：创建日志配置**
- 在 `application.yml` 中添加日志配置项
- 创建 `DeviceLogProperties` 配置类

**步骤 2.2：实现日志接收接口**
- 创建 `DeviceLogController`
- 实现 `POST /admin/device/logs/batch` 接口
- 实现 NDJSON 解析和校验

**步骤 2.3：实现文件写入服务**
- 创建 `DeviceLogService`
- 实现 chipId 校验
- 实现时间换算
- 实现文件追加写入
- 实现并发锁

**步骤 2.4：实现日志查询接口**
- 实现 `GET /admin/device/logs/query` 接口
- 实现按日期、级别、模块、关键词筛选
- 实现分页和排序

**步骤 2.5：实现设备列表接口**
- 实现 `GET /admin/device/logs/devices` 接口
- 返回 chipId、deviceType、lastLogTime

**步骤 2.6：实现日志清理任务**
- 创建定时任务
- 按配置保留天数清理过期日志

### 6.4 阶段 3 详细步骤

**步骤 3.1：创建 API 封装**
- 创建 `src/api/log.ts`
- 定义接口类型
- 封装请求函数

**步骤 3.2：创建日志面板页面**
- 创建 `src/views/DeviceLogs.vue`
- 实现筛选控件
- 实现日志列表展示
- 实现自动刷新
- 实现日志详情展开

**步骤 3.3：添加路由和导航**
- 在路由配置中添加日志路由
- 在导航菜单中添加日志入口

**步骤 3.4：样式优化**
- 日志级别颜色标签
- 滚动容器
- 空状态、加载状态、错误状态

### 6.5 阶段 4 详细步骤

**步骤 4.1：创建日志系统头文件**
- 定义 `LogEntry` 结构体
- 定义日志级别枚举
- 定义日志宏（LOG_DEBUG、LOG_INFO、LOG_WARN、LOG_ERROR）
- 定义配置宏

**步骤 4.2：实现环形缓冲区**
- 实现 `logWrite()` 函数
- 实现优先级替换逻辑
- 实现缓冲区清空逻辑

**步骤 4.3：实现日志上传**
- 实现 `postNdjsonToServer()` 函数
- 实现 `uploadLogs()` 函数
- 处理上传成功/失败逻辑

**步骤 4.4：集成到主循环**
- 在 `setup()` 中初始化日志系统
- 在 `loop()` 中调用 `uploadLogs()`
- 确保只在 WiFi 已连接时上传

**步骤 4.5：替换现有日志**
- 逐步将各模块的 `DEBUG_SERIAL.printf()` 替换为 `LOG_xxx()` 宏
- 保持串口输出功能

### 6.6 阶段 5：联调测试

**测试场景**：

1. 用 curl 模拟 NDJSON 上传，确认后端能写入文件
2. 上传非法 chipId，确认后端拒绝
3. 上传非法 JSON 行，确认后端跳过并返回 invalid 数量
4. 上传跨天日志，确认写入正确日期文件
5. 连续并发上传，确认文件不乱序、不损坏
6. 前端筛选 level/module/keyword/order 是否和后端一致
7. ESP8266 断网后日志是否保留，恢复网络后是否继续上传
8. 上传成功后是否只删除已成功上传的日志
9. 缓冲区满时优先级策略是否生效
10. 自动刷新是否正常工作

**验证命令**：

后端：
```bash
mvn test
mvn -DskipTests compile
```

前端：
```bash
npm run type-check
npm run build
```

ESP8266：
```bash
platformio run
platformio run --target upload
```

---

## 7. 配置项说明

### 7.1 ESP8266 端配置

```cpp
// 日志系统配置
#define LOG_BUFFER_CAPACITY 50      // 缓冲区容量
#define LOG_UPLOAD_INTERVAL_MS 10000 // 上传间隔（毫秒）
#define LOG_MAX_BATCH_SIZE 20       // 每批最大上传条数
#define LOG_MSG_MAX_LEN 128         // 消息最大长度
#define LOG_MODULE_MAX_LEN 12       // 模块名最大长度
#define LOG_ENABLED true            // 是否启用在线日志
```

### 7.2 后端配置

```yaml
smart-light:
  logs:
    root-dir: ./logs              # 日志根目录
    retention-days: 7             # 保留天数
    max-batch-lines: 20           # 每批最大行数
    max-line-length: 512          # 单行最大长度
```

---

## 8. 注意事项

### 8.1 ESP8266 端注意事项

1. **内存管理**：避免长时间 String 拼接，使用 `reserve()` 预分配内存
2. **上传失败处理**：只有 HTTP 返回 2xx 成功后才清空已上传日志
3. **递归防护**：日志模块内部禁止调用 `LOG_xxx()`，避免上传失败时反复写日志
4. **字符串安全**：`strncpy` 后要手动补 `\0`
5. **上传条件**：只在 WiFi 已连接、后端地址已配置时上传

### 8.2 后端注意事项

1. **chipId 校验**：不能直接拼接文件路径，防止路径穿越
2. **并发控制**：同一设备的并发上传需要加锁
3. **时间换算**：边界保护，异常情况使用服务器时间
4. **文件日期**：根据换算后的 ts 决定，跨天批次可写入多个文件
5. **NDJSON 校验**：逐行解析，非法行跳过并统计

### 8.3 前端注意事项

1. **请求防抖**：关键词搜索加 300ms 防抖
2. **请求重叠**：自动刷新时避免重复请求
3. **定时器清理**：组件卸载时清除定时器
4. **状态处理**：loading、empty、error 状态都要处理
5. **性能优化**：日志列表使用固定高度滚动容器

---

## 9. 未来扩展

1. **日志告警**：ERROR 级别日志触发告警通知
2. **日志分析**：统计各模块日志频率，分析设备健康状态
3. **实时推送**：WebSocket 推送新日志，实现真正的实时查看
4. **日志导出**：支持导出为 CSV/JSON 格式
5. **多设备对比**：同时查看多个设备的日志
