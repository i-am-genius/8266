# ESP8266 在线日志系统实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 为 ESP8266 固件添加在线日志功能，实现日志远程上传和在线查看

**Architecture:** ESP8266 通过 HTTP 批量上报 NDJSON 格式日志到后端，后端写入 JSONL 文件，前端通过查询接口展示日志面板

**Tech Stack:** ESP8266 Arduino, Spring Boot 4, Vue 3 + TypeScript

## Global Constraints

- ESP8266 内存限制：日志缓冲区最多 50 条，约 7.4KB
- 每批上传最多 20 条，避免 body 过大
- 上传间隔 10 秒，只在 WiFi 已连接时上传（不依赖 WebSocket 状态）
- 后端日志保留 7 天，每天凌晨 2 点清理
- chipId 只允许 A-Z, a-z, 0-9, -, _
- 日志上传使用 NDJSON 格式（Content-Type: application/x-ndjson）
- 使用全局 uploadSecret 鉴权，不依赖管理员 JWT
- 生产环境未配置 uploadSecret 时拒绝上传

---

## 阶段 1：后端接口和文件存储

### Task 1.1: 创建日志配置类

**Files:**
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\config\DeviceLogProperties.java`
- Modify: `E:\smart-light-backend\src\main\resources\application.yaml`

**Interfaces:**
- Produces: `DeviceLogProperties` 配置类，提供 `rootDir`, `retentionDays`, `maxBatchLines`, `maxLineLength`, `uploadSecret`, `allowUnsignedLogUpload` 属性

- [ ] **Step 1: 创建配置类**

```java
package com.genius.smartlight.config;

import lombok.Data;
import org.springframework.boot.context.properties.ConfigurationProperties;
import org.springframework.stereotype.Component;

@Data
@Component
@ConfigurationProperties(prefix = "smart-light.logs")
public class DeviceLogProperties {
    private String rootDir = "./logs";
    private int retentionDays = 7;
    private int maxBatchLines = 20;
    private int maxLineLength = 512;
    private String uploadSecret = "";  // 全局日志上传密钥
    private boolean allowUnsignedLogUpload = false;  // 是否允许无签名上传（仅本地开发）
}
```

- [ ] **Step 2: 修改 application.yaml 添加日志配置**

在 `application.yaml` 末尾添加：

```yaml
smart-light:
  logs:
    root-dir: ${DEVICE_LOG_ROOT_DIR:./logs}
    retention-days: ${DEVICE_LOG_RETENTION_DAYS:7}
    max-batch-lines: ${DEVICE_LOG_MAX_BATCH_LINES:20}
    max-line-length: ${DEVICE_LOG_MAX_LINE_LENGTH:512}
    upload-secret: ${DEVICE_LOG_UPLOAD_SECRET:}
    allow-unsigned-log-upload: ${ALLOW_UNSIGNED_LOG_UPLOAD:false}
```

- [ ] **Step 3: 验证配置类加载**

Run: `cd E:\smart-light-backend && mvn -DskipTests compile`
Expected: 编译成功，无错误

- [ ] **Step 4: Commit**

```bash
git add src/main/java/com/genius/smartlight/config/DeviceLogProperties.java src/main/resources/application.yaml
git commit -m "feat: 添加设备日志配置类"
```

---

### Task 1.2: 创建日志条目 DTO 和解析结果

**Files:**
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\dto\device\DeviceLogEntryDTO.java`
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\dto\device\DeviceLogParseResult.java`

**Interfaces:**
- Produces: `DeviceLogEntryDTO` 类，包含 `uptimeMs`, `seq`, `level`, `module`, `msg` 字段
- Produces: `DeviceLogParseResult` 类，包含 `received`, `invalid`, `entries` 字段

- [ ] **Step 1: 创建 DTO 类**

```java
package com.genius.smartlight.dto.device;

import lombok.Data;

@Data
public class DeviceLogEntryDTO {
    private Long uptimeMs;
    private Integer seq;
    private String level;
    private String module;
    private String msg;
}
```

- [ ] **Step 2: 创建解析结果类**

```java
package com.genius.smartlight.dto.device;

import lombok.Data;
import java.util.List;

@Data
public class DeviceLogParseResult {
    private int received;      // 原始非空行数
    private int invalid;       // 解析非法行数
    private List<DeviceLogEntryDTO> entries;  // 解析成功的条目
}
```

- [ ] **Step 3: 验证编译**

Run: `cd E:\smart-light-backend && mvn -DskipTests compile`
Expected: 编译成功

- [ ] **Step 4: Commit**

```bash
git add src/main/java/com/genius/smartlight/dto/device/DeviceLogEntryDTO.java src/main/java/com/genius/smartlight/dto/device/DeviceLogParseResult.java
git commit -m "feat: 添加设备日志 DTO 和解析结果类"
```

---

### Task 1.3: 创建日志服务类

**Files:**
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\service\device\DeviceLogService.java`
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\service\device\impl\DeviceLogServiceImpl.java`

**Interfaces:**
- Consumes: `DeviceLogProperties`, `DeviceLogEntryDTO`, `DeviceLogParseResult`
- Produces: `DeviceLogService` 接口，提供 `parseNdjson()`, `writeLogs()`, `queryLogs()`, `getLogDevices()`, `validateUploadSecret()` 方法

- [ ] **Step 1: 创建服务接口**

```java
package com.genius.smartlight.service.device;

import com.genius.smartlight.dto.device.DeviceLogEntryDTO;
import com.genius.smartlight.dto.device.DeviceLogParseResult;
import java.util.List;
import java.util.Map;

public interface DeviceLogService {
    /**
     * 解析 NDJSON 格式的请求体
     * @param body NDJSON 格式字符串
     * @return 解析结果，包含 received, invalid, entries
     */
    DeviceLogParseResult parseNdjson(String body);
    
    /**
     * 批量写入日志
     * @param chipId 设备 ID
     * @param uploadUptimeMs 上传时设备的 uptimeMs
     * @param entries 日志条目列表
     * @return 写入结果 {saved, invalid}
     */
    Map<String, Integer> writeLogs(String chipId, long uploadUptimeMs, List<DeviceLogEntryDTO> entries);
    
    /**
     * 查询日志
     * @param chipId 设备 ID
     * @param date 日期 (yyyy-MM-dd)，可选
     * @param level 日志级别，可选
     * @param module 模块名，可选
     * @param keyword 关键词，可选
     * @param startTime 开始时间戳，可选
     * @param endTime 结束时间戳，可选
     * @param beforeTs 分页参数，可选
     * @param limit 返回条数
     * @param order 排序 asc/desc
     * @return 日志列表和分页信息
     */
    Map<String, Object> queryLogs(String chipId, String date, String level, String module, 
                                   String keyword, Long startTime, Long endTime, 
                                   Long beforeTs, int limit, String order);
    
    /**
     * 获取有日志的设备列表
     * @return 设备列表
     */
    List<Map<String, Object>> getLogDevices();
    
    /**
     * 校验上传密钥
     * @param secret 上传密钥
     * @return 是否有效
     */
    boolean validateUploadSecret(String secret);
}
```

- [ ] **Step 2: 创建服务实现类**

```java
package com.genius.smartlight.service.device.impl;

import com.genius.smartlight.config.DeviceLogProperties;
import com.genius.smartlight.dto.device.DeviceLogEntryDTO;
import com.genius.smartlight.dto.device.DeviceLogParseResult;
import com.genius.smartlight.service.device.DeviceLogService;
import com.alibaba.fastjson2.JSON;
import com.alibaba.fastjson2.JSONObject;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.stereotype.Service;

import java.io.*;
import java.nio.file.*;
import java.time.*;
import java.time.format.DateTimeFormatter;
import java.util.*;
import java.util.concurrent.ConcurrentHashMap;
import java.util.stream.Collectors;
import java.util.stream.Stream;

@Slf4j
@Service
@RequiredArgsConstructor
public class DeviceLogServiceImpl implements DeviceLogService {

    private final DeviceLogProperties properties;
    
    // 设备级锁，防止同一设备并发写入
    private final ConcurrentHashMap<String, Object> deviceLocks = new ConcurrentHashMap<>();
    
    private static final Set<String> VALID_LEVELS = Set.of("DEBUG", "INFO", "WARN", "ERROR");
    
    @Override
    public DeviceLogParseResult parseNdjson(String body) {
        DeviceLogParseResult result = new DeviceLogParseResult();
        List<DeviceLogEntryDTO> entries = new ArrayList<>();
        
        if (body == null || body.isEmpty()) {
            result.setReceived(0);
            result.setInvalid(0);
            result.setEntries(entries);
            return result;
        }
        
        int received = 0;
        int invalid = 0;
        
        for (String line : body.split("\n")) {
            line = line.trim();
            if (line.isEmpty()) continue;
            
            received++;
            
            try {
                JSONObject json = JSON.parseObject(line);
                
                // 校验必填字段
                if (!json.containsKey("uptimeMs") || !json.containsKey("seq") ||
                    !json.containsKey("level") || !json.containsKey("module") ||
                    !json.containsKey("msg")) {
                    invalid++;
                    continue;
                }
                
                // 校验 level 合法性
                String level = json.getString("level");
                if (!VALID_LEVELS.contains(level)) {
                    invalid++;
                    continue;
                }
                
                // 校验长度
                String module = json.getString("module");
                String msg = json.getString("msg");
                if (module.length() > 12 || msg.length() > 128) {
                    invalid++;
                    continue;
                }
                
                DeviceLogEntryDTO entry = new DeviceLogEntryDTO();
                entry.setUptimeMs(json.getLong("uptimeMs"));
                entry.setSeq(json.getIntValue("seq"));
                entry.setLevel(level);
                entry.setModule(module);
                entry.setMsg(msg);
                
                entries.add(entry);
            } catch (Exception e) {
                log.debug("Failed to parse NDJSON line: {}", line, e);
                invalid++;
            }
        }
        
        result.setReceived(received);
        result.setInvalid(invalid);
        result.setEntries(entries);
        return result;
    }
    
    @Override
    public Map<String, Integer> writeLogs(String chipId, long uploadUptimeMs, List<DeviceLogEntryDTO> entries) {
        // 1. 校验 chipId
        String safeChipId = validateChipId(chipId);
        if (safeChipId == null) {
            throw new IllegalArgumentException("Invalid chipId: " + chipId);
        }
        
        int saved = 0;
        int invalid = 0;
        
        // 2. 获取设备级锁
        Object lock = deviceLocks.computeIfAbsent(safeChipId, k -> new Object());
        
        synchronized (lock) {
            long serverTimeMs = System.currentTimeMillis();
            
            for (DeviceLogEntryDTO entry : entries) {
                try {
                    // 3. 校验日志条目
                    if (!validateEntry(entry)) {
                        invalid++;
                        continue;
                    }
                    
                    // 4. 时间换算
                    long ts = serverTimeMs - (uploadUptimeMs - entry.getUptimeMs());
                    if (entry.getUptimeMs() > uploadUptimeMs) {
                        ts = serverTimeMs; // 异常情况使用服务器时间
                    }
                    
                    // 5. 根据 ts 决定文件日期
                    String dateStr = LocalDate.ofInstant(
                        Instant.ofEpochMilli(ts),
                        ZoneId.systemDefault()
                    ).format(DateTimeFormatter.ISO_LOCAL_DATE);
                    
                    // 6. 构建文件路径（使用绝对路径）
                    Path root = Paths.get(properties.getRootDir()).toAbsolutePath().normalize();
                    Path logFile = root.resolve(safeChipId).resolve(dateStr + ".jsonl").normalize();
                    
                    // 7. 安全检查：确保路径在 root 下
                    if (!logFile.startsWith(root)) {
                        throw new IllegalArgumentException("Invalid path");
                    }
                    
                    // 8. 确保目录存在
                    Files.createDirectories(logFile.getParent());
                    
                    // 9. 构建 JSONL 行
                    String jsonLine = String.format(
                        "{\"ts\":%d,\"uptimeMs\":%d,\"seq\":%d,\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}",
                        ts, entry.getUptimeMs(), entry.getSeq(),
                        entry.getLevel(), entry.getModule(),
                        escapeJson(entry.getMsg())
                    );
                    
                    // 10. 追加写入文件
                    Files.write(logFile, (jsonLine + "\n").getBytes(), 
                        StandardOpenOption.CREATE, StandardOpenOption.APPEND);
                    
                    saved++;
                } catch (Exception e) {
                    log.warn("Failed to write log entry: {}", e.getMessage());
                    invalid++;
                }
            }
        }
        
        Map<String, Integer> result = new HashMap<>();
        result.put("saved", saved);
        result.put("invalid", invalid);
        return result;
    }
    
    @Override
    public Map<String, Object> queryLogs(String chipId, String date, String level, String module,
                                          String keyword, Long startTime, Long endTime,
                                          Long beforeTs, int limit, String order) {
        // 1. 校验 chipId
        String safeChipId = validateChipId(chipId);
        if (safeChipId == null) {
            throw new IllegalArgumentException("Invalid chipId: " + chipId);
        }
        
        // 2. 确定要查询的日期文件
        List<String> datesToQuery = new ArrayList<>();
        if (date != null && !date.isEmpty()) {
            datesToQuery.add(date);
        } else {
            // 默认查今天
            datesToQuery.add(LocalDate.now().format(DateTimeFormatter.ISO_LOCAL_DATE));
        }
        
        // 3. 读取日志文件
        List<JSONObject> allLogs = new ArrayList<>();
        Path root = Paths.get(properties.getRootDir()).toAbsolutePath().normalize();
        Path logDir = root.resolve(safeChipId);
        
        if (!Files.exists(logDir)) {
            return buildQueryResult(new ArrayList<>(), 0, false);
        }
        
        for (String dateStr : datesToQuery) {
            Path logFile = logDir.resolve(dateStr + ".jsonl");
            if (!Files.exists(logFile)) continue;
            
            try (Stream<String> lines = Files.lines(logFile)) {
                lines.map(line -> {
                    try {
                        return JSON.parseObject(line);
                    } catch (Exception e) {
                        return null;
                    }
                })
                .filter(Objects::nonNull)
                .filter(json -> {
                    // 4. 应用筛选条件
                    if (level != null && !level.isEmpty()) {
                        Set<String> levels = Set.of(level.split(","));
                        if (!levels.contains(json.getString("level"))) return false;
                    }
                    if (module != null && !module.isEmpty()) {
                        Set<String> modules = Set.of(module.split(","));
                        if (!modules.contains(json.getString("module"))) return false;
                    }
                    if (keyword != null && !keyword.isEmpty()) {
                        String msg = json.getString("msg");
                        if (msg == null || !msg.contains(keyword)) return false;
                    }
                    if (startTime != null) {
                        Long ts = json.getLong("ts");
                        if (ts == null || ts < startTime) return false;
                    }
                    if (endTime != null) {
                        Long ts = json.getLong("ts");
                        if (ts == null || ts > endTime) return false;
                    }
                    if (beforeTs != null) {
                        Long ts = json.getLong("ts");
                        if (ts == null || ts >= beforeTs) return false;
                    }
                    return true;
                })
                .forEach(allLogs::add);
            } catch (IOException e) {
                log.warn("Failed to read log file: {}", logFile, e);
            }
        }
        
        // 5. 排序
        boolean ascending = "asc".equalsIgnoreCase(order);
        allLogs.sort((a, b) -> {
            Long tsA = a.getLong("ts");
            Long tsB = b.getLong("ts");
            if (tsA == null || tsB == null) return 0;
            return ascending ? Long.compare(tsA, tsB) : Long.compare(tsB, tsA);
        });
        
        // 6. 分页
        boolean hasMore = allLogs.size() > limit;
        List<JSONObject> paginatedLogs = allLogs.subList(0, Math.min(limit, allLogs.size()));
        
        // 7. 转换为返回格式
        List<Map<String, Object>> logs = paginatedLogs.stream()
            .map(json -> {
                Map<String, Object> map = new HashMap<>();
                map.put("ts", json.getLong("ts"));
                map.put("uptimeMs", json.getLong("uptimeMs"));
                map.put("seq", json.getIntValue("seq"));
                map.put("level", json.getString("level"));
                map.put("module", json.getString("module"));
                map.put("msg", json.getString("msg"));
                return map;
            })
            .collect(Collectors.toList());
        
        return buildQueryResult(logs, allLogs.size(), hasMore);
    }
    
    @Override
    public List<Map<String, Object>> getLogDevices() {
        List<Map<String, Object>> devices = new ArrayList<>();
        Path root = Paths.get(properties.getRootDir()).toAbsolutePath().normalize();
        
        if (!Files.exists(root)) return devices;
        
        try (Stream<Path> deviceDirs = Files.list(root)) {
            deviceDirs.filter(Files::isDirectory)
                .forEach(deviceDir -> {
                    String chipId = deviceDir.getFileName().toString();
                    Map<String, Object> deviceInfo = new HashMap<>();
                    deviceInfo.put("chipId", chipId);
                    
                    // 查找最新的日志文件
                    try (Stream<Path> logFiles = Files.list(deviceDir)) {
                        Optional<Long> lastLogTime = logFiles
                            .filter(p -> p.toString().endsWith(".jsonl"))
                            .map(p -> {
                                try {
                                    return Files.getLastModifiedTime(p).toMillis();
                                } catch (IOException e) {
                                    return 0L;
                                }
                            })
                            .max(Long::compareTo);
                        
                        deviceInfo.put("lastLogTime", lastLogTime.orElse(0L));
                    } catch (IOException e) {
                        deviceInfo.put("lastLogTime", 0L);
                    }
                    
                    devices.add(deviceInfo);
                });
        } catch (IOException e) {
            log.warn("Failed to list device directories: {}", root, e);
        }
        
        return devices;
    }
    
    @Override
    public boolean validateUploadSecret(String secret) {
        String configuredSecret = properties.getUploadSecret();
        
        // 未配置 secret 时
        if (configuredSecret == null || configuredSecret.isEmpty()) {
            // 如果允许无签名上传（仅本地开发），返回 true
            if (properties.isAllowUnsignedLogUpload()) {
                return true;
            }
            // 否则拒绝上传
            return false;
        }
        
        // 配置了 secret 时，必须匹配
        return configuredSecret.equals(secret);
    }
    
    private String validateChipId(String chipId) {
        if (chipId == null || chipId.isEmpty()) return null;
        if (!chipId.matches("^[A-Za-z0-9_-]+$")) return null;
        return chipId.toUpperCase();
    }
    
    private boolean validateEntry(DeviceLogEntryDTO entry) {
        if (entry.getUptimeMs() == null || entry.getSeq() == null ||
            entry.getLevel() == null || entry.getModule() == null || entry.getMsg() == null) {
            return false;
        }
        if (!VALID_LEVELS.contains(entry.getLevel())) {
            return false;
        }
        if (entry.getModule().length() > 12 || entry.getMsg().length() > 128) {
            return false;
        }
        return true;
    }
    
    private String escapeJson(String str) {
        StringBuilder sb = new StringBuilder(str.length() + 10);
        for (int i = 0; i < str.length(); i++) {
            char c = str.charAt(i);
            switch (c) {
                case '"':  sb.append("\\\""); break;
                case '\\': sb.append("\\\\"); break;
                case '\n': sb.append("\\n"); break;
                case '\r': sb.append("\\r"); break;
                case '\t': sb.append("\\t"); break;
                default:   sb.append(c);
            }
        }
        return sb.toString();
    }
    
    private Map<String, Object> buildQueryResult(List<Map<String, Object>> logs, int total, boolean hasMore) {
        Map<String, Object> result = new HashMap<>();
        result.put("total", total);
        result.put("hasMore", hasMore);
        result.put("logs", logs);
        return result;
    }
}
```

- [ ] **Step 3: 验证编译**

Run: `cd E:\smart-light-backend && mvn -DskipTests compile`
Expected: 编译成功

- [ ] **Step 4: Commit**

```bash
git add src/main/java/com/genius/smartlight/service/device/DeviceLogService.java src/main/java/com/genius/smartlight/service/device/impl/DeviceLogServiceImpl.java
git commit -m "feat: 添加设备日志服务类"
```

---

### Task 1.4: 创建日志控制器

**Files:**
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\controller\admin\device\DeviceLogController.java`

**Interfaces:**
- Consumes: `DeviceLogService`, `DeviceLogEntryDTO`, `DeviceLogParseResult`
- Produces: REST API endpoints: `POST /admin/device/logs/batch`, `GET /admin/device/logs/query`, `GET /admin/device/logs/devices`

- [ ] **Step 1: 创建控制器类**

```java
package com.genius.smartlight.controller.admin.device;

import com.genius.smartlight.common.ApiResponse;
import com.genius.smartlight.dto.device.DeviceLogParseResult;
import com.genius.smartlight.service.device.DeviceLogService;
import io.swagger.v3.oas.annotations.Operation;
import io.swagger.v3.oas.annotations.tags.Tag;
import lombok.RequiredArgsConstructor;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.*;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

@Tag(name = "设备日志", description = "设备日志上传和查询接口")
@RestController
@RequestMapping("/admin/device/logs")
@RequiredArgsConstructor
public class DeviceLogController {

    private final DeviceLogService deviceLogService;

    @Operation(summary = "批量上传日志", description = "设备通过 NDJSON 格式批量上传日志")
    @PostMapping("/batch")
    public ResponseEntity<?> batchLogs(
            @RequestParam String chipId,
            @RequestParam long uploadUptimeMs,
            @RequestHeader(value = "Authorization", required = false) String authorization,
            @RequestBody String body) {
        // 1. 校验 uploadSecret
        String secret = "";
        if (authorization != null && authorization.startsWith("Bearer ")) {
            secret = authorization.substring(7);
        }
        if (!deviceLogService.validateUploadSecret(secret)) {
            return ResponseEntity.status(HttpStatus.UNAUTHORIZED)
                .body(ApiResponse.error(401, "Invalid upload secret"));
        }
        
        // 2. 解析 NDJSON
        DeviceLogParseResult parseResult = deviceLogService.parseNdjson(body);
        
        // 3. 写入日志
        Map<String, Integer> writeResult = deviceLogService.writeLogs(chipId, uploadUptimeMs, parseResult.getEntries());
        
        // 4. 构建响应
        Map<String, Integer> data = new HashMap<>();
        data.put("received", parseResult.getReceived());
        data.put("saved", writeResult.get("saved"));
        data.put("invalid", parseResult.getInvalid() + writeResult.get("invalid"));
        
        return ResponseEntity.ok(ApiResponse.success(data));
    }

    @Operation(summary = "查询日志", description = "按条件查询设备日志")
    @GetMapping("/query")
    public ApiResponse<Map<String, Object>> queryLogs(
            @RequestParam String chipId,
            @RequestParam(required = false) String date,
            @RequestParam(required = false) String level,
            @RequestParam(required = false) String module,
            @RequestParam(required = false) String keyword,
            @RequestParam(required = false) Long startTime,
            @RequestParam(required = false) Long endTime,
            @RequestParam(required = false) Long beforeTs,
            @RequestParam(defaultValue = "100") int limit,
            @RequestParam(defaultValue = "desc") String order) {
        Map<String, Object> result = deviceLogService.queryLogs(
            chipId, date, level, module, keyword, startTime, endTime, beforeTs, limit, order);
        return ApiResponse.success(result);
    }

    @Operation(summary = "获取设备列表", description = "获取有日志的设备列表")
    @GetMapping("/devices")
    public ApiResponse<List<Map<String, Object>>> getLogDevices() {
        List<Map<String, Object>> devices = deviceLogService.getLogDevices();
        return ApiResponse.success(devices);
    }
}
```

- [ ] **Step 2: 验证编译**

Run: `cd E:\smart-light-backend && mvn -DskipTests compile`
Expected: 编译成功

- [ ] **Step 3: Commit**

```bash
git add src/main/java/com/genius/smartlight/controller/admin/device/DeviceLogController.java
git commit -m "feat: 添加设备日志控制器"
```

---

### Task 1.5: 创建日志清理任务

**Files:**
- Create: `E:\smart-light-backend\src\main\java\com\genius\smartlight\task\DeviceLogCleanupTask.java`

**Interfaces:**
- Consumes: `DeviceLogProperties`
- Produces: 定时清理过期日志文件

- [ ] **Step 1: 创建清理任务类**

```java
package com.genius.smartlight.task;

import com.genius.smartlight.config.DeviceLogProperties;
import lombok.RequiredArgsConstructor;
import lombok.extern.slf4j.Slf4j;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

import java.io.IOException;
import java.nio.file.*;
import java.time.LocalDate;
import java.time.format.DateTimeFormatter;
import java.util.stream.Stream;

@Slf4j
@Component
@RequiredArgsConstructor
public class DeviceLogCleanupTask {

    private final DeviceLogProperties properties;

    @Scheduled(cron = "0 0 2 * * ?") // 每天凌晨 2 点执行
    public void cleanupOldLogs() {
        LocalDate cutoffDate = LocalDate.now().minusDays(properties.getRetentionDays());
        
        Path root = Paths.get(properties.getRootDir()).toAbsolutePath().normalize();
        if (!Files.exists(root)) return;
        
        log.info("Starting log cleanup, cutoff date: {}", cutoffDate);
        
        // 遍历所有设备目录，使用 try-with-resources 确保目录流关闭
        try (Stream<Path> deviceDirs = Files.list(root)) {
            deviceDirs.filter(Files::isDirectory).forEach(deviceDir -> {
                // 删除过期的日志文件
                try (Stream<Path> logFiles = Files.list(deviceDir)) {
                    logFiles.filter(path -> {
                        String fileName = path.getFileName().toString();
                        if (!fileName.endsWith(".jsonl")) return false;
                        try {
                            LocalDate fileDate = LocalDate.parse(fileName.replace(".jsonl", ""), 
                                DateTimeFormatter.ISO_LOCAL_DATE);
                            return fileDate.isBefore(cutoffDate);
                        } catch (Exception e) {
                            // 文件名格式不正确，跳过
                            return false;
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
            log.warn("Failed to list device directories in: {}", root, e);
        }
        
        log.info("Log cleanup completed");
    }
}
```

- [ ] **Step 2: 验证编译**

Run: `cd E:\smart-light-backend && mvn -DskipTests compile`
Expected: 编译成功

- [ ] **Step 3: Commit**

```bash
git add src/main/java/com/genius/smartlight/task/DeviceLogCleanupTask.java
git commit -m "feat: 添加日志清理定时任务"
```

---

### Task 1.6: 修改 Spring Security 配置

**Files:**
- Modify: `E:\smart-light-backend\src\main\java\com\genius\smartlight\config\SecurityConfig.java` (或类似文件)

**Interfaces:**
- Produces: `POST /admin/device/logs/batch` 放行，不被管理员 JWT 拦截

- [ ] **Step 1: 检查现有 SecurityConfig**

Run: `find E:\smart-light-backend -name "SecurityConfig.java" -type f`
Expected: 找到安全配置文件

- [ ] **Step 2: 修改 SecurityConfig 放行日志上传接口**

```java
// 在 SecurityFilterChain 或 WebSecurityConfigurerAdapter 中添加
.requestMatchers(HttpMethod.POST, "/admin/device/logs/batch").permitAll()
```

- [ ] **Step 3: 验证编译**

Run: `cd E:\smart-light-backend && mvn -DskipTests compile`
Expected: 编译成功

- [ ] **Step 4: Commit**

```bash
git add src/main/java/com/genius/smartlight/config/SecurityConfig.java
git commit -m "feat: 放行设备日志上传接口"
```

---

## 阶段 2：curl/Postman 模拟 NDJSON 验证

### Task 2.1: 启动后端服务并测试日志上传

**Files:**
- None (使用现有文件)

**Interfaces:**
- Consumes: `POST /admin/device/logs/batch` 接口

- [ ] **Step 1: 启动后端服务**

Run: `cd E:\smart-light-backend && mvn spring-boot:run`
Expected: 服务启动成功，监听端口 3000

- [ ] **Step 2: 使用 curl 测试 NDJSON 日志上传**

```bash
curl -X POST "http://localhost:3000/admin/device/logs/batch?chipId=LAMP-37461B&uploadUptimeMs=252220000" \
  -H "Content-Type: application/x-ndjson" \
  -H "Authorization: Bearer your-upload-secret" \
  -d '{"uptimeMs":252215000,"seq":1001,"level":"INFO","module":"WIFI","msg":"Connected to WiFi"}
{"uptimeMs":252216000,"seq":1002,"level":"DEBUG","module":"SENSOR","msg":"BH1750 lux=320"}'
```

Expected 响应:
```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "received": 2,
    "saved": 2,
    "invalid": 0
  }
}
```

- [ ] **Step 3: 验证日志文件已创建**

Run: `ls -la ./logs/LAMP-37461B/`
Expected: 看到今天的日期文件，如 `2026-07-06.jsonl`

- [ ] **Step 4: 查看日志文件内容**

Run: `cat ./logs/LAMP-37461B/2026-07-06.jsonl`
Expected: 看到两行 JSON 日志，每行包含 ts, uptimeMs, seq, level, module, msg 字段

---

### Task 2.2: 测试日志查询接口

**Files:**
- None (使用现有文件)

**Interfaces:**
- Consumes: `GET /admin/device/logs/query` 接口

- [ ] **Step 1: 测试查询日志**

```bash
curl "http://localhost:3000/admin/device/logs/query?chipId=LAMP-37461B&limit=10&order=desc"
```

Expected 响应:
```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "total": 2,
    "hasMore": false,
    "logs": [
      {"ts":...,"uptimeMs":252216000,"seq":1002,"level":"DEBUG","module":"SENSOR","msg":"BH1750 lux=320"},
      {"ts":...,"uptimeMs":252215000,"seq":1001,"level":"INFO","module":"WIFI","msg":"Connected to WiFi"}
    ]
  }
}
```

- [ ] **Step 2: 测试按级别筛选**

```bash
curl "http://localhost:3000/admin/device/logs/query?chipId=LAMP-37461B&level=INFO"
```

Expected: 只返回 INFO 级别的日志

- [ ] **Step 3: 测试设备列表接口**

```bash
curl "http://localhost:3000/admin/device/logs/devices"
```

Expected 响应:
```json
{
  "code": 0,
  "msg": "success",
  "data": [
    {"chipId": "LAMP-37461B", "lastLogTime": ...}
  ]
}
```

---

## 阶段 3：前端日志面板

### Task 3.1: 创建日志 API 封装

**Files:**
- Create: `E:\smart-light-archive\src\api\log.ts`

**Interfaces:**
- Produces: `getLogs()`, `getLogDevices()` 函数

- [ ] **Step 1: 创建 API 文件**

```typescript
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
  beforeTs?: number;
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

export function getLogs(params: LogQueryParams) {
  return request.get<LogQueryResult>('/admin/device/logs/query', { params });
}

export function getLogDevices() {
  return request.get<DeviceLogInfo[]>('/admin/device/logs/devices');
}
```

- [ ] **Step 2: 验证 TypeScript 编译**

Run: `cd E:\smart-light-archive && npm run type-check`
Expected: 无类型错误

- [ ] **Step 3: Commit**

```bash
git add src/api/log.ts
git commit -m "feat: 添加设备日志 API 封装"
```

---

### Task 3.2: 创建日志面板页面

**Files:**
- Create: `E:\smart-light-archive\src\views\DeviceLogs.vue`

**Interfaces:**
- Consumes: `getLogs()`, `getLogDevices()` API
- Produces: DeviceLogs.vue 页面组件

- [ ] **Step 1: 创建页面组件**

```vue
<template>
  <div class="device-logs">
    <div class="filters">
      <select v-model="selectedDevice" @change="fetchLatestLogs">
        <option value="">选择设备</option>
        <option v-for="device in devices" :key="device.chipId" :value="device.chipId">
          {{ device.chipId }}
        </option>
      </select>
      
      <input type="date" v-model="selectedDate" @change="fetchLatestLogs" />
      
      <select v-model="selectedLevel" @change="fetchLatestLogs">
        <option value="">全部级别</option>
        <option value="DEBUG">DEBUG</option>
        <option value="INFO">INFO</option>
        <option value="WARN">WARN</option>
        <option value="ERROR">ERROR</option>
      </select>
      
      <input type="text" v-model="keyword" placeholder="搜索关键词" @input="debouncedFetch" />
      
      <button @click="fetchLatestLogs" :disabled="loading">刷新</button>
      
      <label>
        <input type="checkbox" v-model="autoRefresh" @change="toggleAutoRefresh" />
        自动刷新
      </label>
    </div>
    
    <div class="log-list" ref="logList">
      <div v-if="!selectedDevice" class="empty-state">请选择设备</div>
      <div v-else-if="logs.length === 0 && !loading" class="empty-state">暂无日志</div>
      <div v-else class="log-entries">
        <div v-for="log in logs" :key="log.seq" class="log-entry" @click="toggleDetail(log)">
          <span class="log-time">{{ formatTime(log.ts) }}</span>
          <span :class="['log-level', log.level.toLowerCase()]">{{ log.level }}</span>
          <span class="log-module">{{ log.module }}</span>
          <span class="log-msg">{{ log.msg }}</span>
        </div>
        <div v-if="expandedLog" class="log-detail">
          <pre>{{ JSON.stringify(expandedLog, null, 2) }}</pre>
        </div>
      </div>
      <div v-if="loading" class="loading">加载中...</div>
      <div v-if="hasMore && !loading" class="load-more">
        <button @click="loadMoreLogs">加载更多</button>
      </div>
    </div>
  </div>
</template>

<script setup lang="ts">
import { ref, onMounted, onBeforeUnmount } from 'vue';
import { getLogs, getLogDevices, LogEntry, DeviceLogInfo } from '@/api/log';

const devices = ref<DeviceLogInfo[]>([]);
const selectedDevice = ref('');
const selectedDate = ref(new Date().toISOString().split('T')[0]);
const selectedLevel = ref('');
const keyword = ref('');
const logs = ref<LogEntry[]>([]);
const loading = ref(false);
const hasMore = ref(true);
const autoRefresh = ref(false);
const expandedLog = ref<LogEntry | null>(null);
let refreshTimer: number | null = null;

// 防抖函数
let debounceTimer: number | null = null;
const debouncedFetch = () => {
  if (debounceTimer) clearTimeout(debounceTimer);
  debounceTimer = window.setTimeout(fetchLatestLogs, 300);
};

// 获取设备列表
async function fetchDevices() {
  try {
    const res = await getLogDevices();
    devices.value = res.data;
  } catch (e) {
    console.error('Failed to fetch devices:', e);
  }
}

// 获取最新日志
async function fetchLatestLogs() {
  if (!selectedDevice.value || loading.value) return;
  loading.value = true;
  
  try {
    const params: any = {
      chipId: selectedDevice.value,
      limit: 100,
      order: 'desc'
    };
    if (selectedDate.value) params.date = selectedDate.value;
    if (selectedLevel.value) params.level = selectedLevel.value;
    if (keyword.value) params.keyword = keyword.value;
    
    const res = await getLogs(params);
    logs.value = res.data.logs;
    hasMore.value = res.data.hasMore;
  } catch (e) {
    console.error('Failed to fetch logs:', e);
  } finally {
    loading.value = false;
  }
}

// 加载更多日志
async function loadMoreLogs() {
  if (!selectedDevice.value || loading.value || !hasMore.value || logs.value.length === 0) return;
  loading.value = true;
  
  try {
    const lastTs = logs.value[logs.value.length - 1].ts;
    const params: any = {
      chipId: selectedDevice.value,
      limit: 100,
      order: 'desc',
      beforeTs: lastTs
    };
    if (selectedDate.value) params.date = selectedDate.value;
    if (selectedLevel.value) params.level = selectedLevel.value;
    if (keyword.value) params.keyword = keyword.value;
    
    const res = await getLogs(params);
    logs.value = [...logs.value, ...res.data.logs];
    hasMore.value = res.data.hasMore;
  } catch (e) {
    console.error('Failed to load more logs:', e);
  } finally {
    loading.value = false;
  }
}

// 切换自动刷新
function toggleAutoRefresh() {
  if (autoRefresh.value) {
    refreshTimer = window.setInterval(fetchLatestLogs, 10000);
  } else {
    if (refreshTimer) {
      window.clearInterval(refreshTimer);
      refreshTimer = null;
    }
  }
}

// 切换日志详情
function toggleDetail(log: LogEntry) {
  expandedLog.value = expandedLog.value?.seq === log.seq ? null : log;
}

// 格式化时间
function formatTime(ts: number) {
  return new Date(ts).toLocaleTimeString();
}

onMounted(() => {
  fetchDevices();
});

onBeforeUnmount(() => {
  if (refreshTimer) {
    window.clearInterval(refreshTimer);
    refreshTimer = null;
  }
  if (debounceTimer) {
    clearTimeout(debounceTimer);
  }
});
</script>

<style scoped>
.device-logs {
  padding: 20px;
}

.filters {
  display: flex;
  gap: 10px;
  margin-bottom: 20px;
  flex-wrap: wrap;
}

.filters select,
.filters input[type="date"],
.filters input[type="text"] {
  padding: 8px;
  border: 1px solid #ddd;
  border-radius: 4px;
}

.filters button {
  padding: 8px 16px;
  background: #007bff;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
}

.filters button:disabled {
  background: #ccc;
}

.log-list {
  background: #1e1e1e;
  border-radius: 8px;
  padding: 16px;
  max-height: 600px;
  overflow-y: auto;
  font-family: 'Consolas', 'Monaco', monospace;
  font-size: 13px;
}

.empty-state {
  text-align: center;
  color: #888;
  padding: 40px;
}

.loading {
  text-align: center;
  color: #888;
  padding: 20px;
}

.load-more {
  text-align: center;
  padding: 10px;
}

.load-more button {
  padding: 8px 16px;
  background: #444;
  color: white;
  border: none;
  border-radius: 4px;
  cursor: pointer;
}

.log-entry {
  display: flex;
  gap: 10px;
  padding: 4px 0;
  cursor: pointer;
  border-bottom: 1px solid #333;
}

.log-entry:hover {
  background: #2d2d2d;
}

.log-time {
  color: #888;
  min-width: 80px;
}

.log-level {
  min-width: 50px;
  font-weight: bold;
}

.log-level.debug { color: #888; }
.log-level.info { color: #5b9bd5; }
.log-level.warn { color: #d4a017; }
.log-level.error { color: #e74c3c; }

.log-module {
  color: #98c379;
  min-width: 60px;
}

.log-msg {
  color: #d4d4d4;
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.log-detail {
  background: #2d2d2d;
  padding: 10px;
  margin: 5px 0;
  border-radius: 4px;
  overflow-x: auto;
}

.log-detail pre {
  margin: 0;
  color: #d4d4d4;
  font-size: 12px;
}
</style>
```

- [ ] **Step 2: 验证 TypeScript 编译**

Run: `cd E:\smart-light-archive && npm run type-check`
Expected: 无类型错误

- [ ] **Step 3: Commit**

```bash
git add src/views/DeviceLogs.vue
git commit -m "feat: 添加设备日志面板页面"
```

---

### Task 3.3: 添加路由和导航

**Files:**
- Modify: `E:\smart-light-archive\src\router\index.ts`
- Modify: `E:\smart-light-archive\src\components\layout\SidebarNav.vue`

**Interfaces:**
- Produces: `/device-logs` 路由和侧边栏菜单项

- [ ] **Step 1: 修改路由配置**

在 `src/router/index.ts` 中添加日志路由：

```typescript
// 在 routes 数组中添加
{
  path: '/device-logs',
  name: 'DeviceLogs',
  component: () => import('@/views/DeviceLogs.vue'),
  meta: { title: '设备日志' }
}
```

- [ ] **Step 2: 修改侧边栏导航**

在 `src/components/layout/SidebarNav.vue` 中添加日志菜单项：

```vue
<!-- 在合适的位置添加 -->
<router-link to="/device-logs" class="nav-item">
  <span class="nav-icon">📋</span>
  <span class="nav-text">设备日志</span>
</router-link>
```

- [ ] **Step 3: 验证前端编译**

Run: `cd E:\smart-light-archive && npm run build`
Expected: 构建成功

- [ ] **Step 4: Commit**

```bash
git add src/router/index.ts src/components/layout/SidebarNav.vue
git commit -m "feat: 添加设备日志路由和导航"
```

---

## 阶段 4：ESP8266 在线日志模块

### Task 4.0: 检查现有项目结构

**Files:**
- Read: `E:\8266_OTA\src\main.cpp`
- Read: `E:\8266_OTA\include\app_config.h`
- Read: `E:\8266_OTA\src\network\http_reporter.cpp`

**Interfaces:**
- 确认现有项目结构，检查是否有 `http_reporter.h`, `postNdjsonToServer()`, `cfg.serverHost`, `wsConnected` 等

- [ ] **Step 1: 检查现有文件结构**

Run: `ls -la E:\8266_OTA\src\network\`
Expected: 查看是否有 http_reporter.cpp

- [ ] **Step 2: 检查 main.cpp 中的变量定义**

搜索 `wsConnected`, `cfg.serverHost`, `deviceId` 等变量是否已定义

- [ ] **Step 3: 根据检查结果调整后续任务**

如果现有结构与计划不符，需要调整 Task 4.1 和 4.2 的实现方式

---

### Task 4.1: 创建日志系统头文件

**Files:**
- Create: `E:\8266_OTA\include\online_logger.h`

**Interfaces:**
- Produces: `LogEntry` 结构体, `LOG_DEBUG/INFO/WARN/ERROR` 宏, `logInit()`, `logWrite()`, `logSetServer()`, `uploadLogs()` 函数

- [ ] **Step 1: 创建头文件**

```cpp
#pragma once

#include <Arduino.h>

// 日志系统配置
#define LOG_BUFFER_CAPACITY 50
#define LOG_UPLOAD_INTERVAL_MS 10000
#define LOG_MAX_BATCH_SIZE 20
#define LOG_MSG_MAX_LEN 128
#define LOG_MODULE_MAX_LEN 12
#define LOG_ENABLED true

// 日志级别
#define LOG_LEVEL_DEBUG 0
#define LOG_LEVEL_INFO  1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_ERROR 3

// 日志条目结构
struct LogEntry {
    unsigned long uptimeMs;
    uint32_t seq;
    uint8_t level;
    char module[LOG_MODULE_MAX_LEN];
    char message[LOG_MSG_MAX_LEN];
};

// 日志宏 - 支持 const char* 和 String
#if LOG_ENABLED
#define LOG_DEBUG(module, msg) logWrite(LOG_LEVEL_DEBUG, module, msg)
#define LOG_INFO(module, msg)  logWrite(LOG_LEVEL_INFO, module, msg)
#define LOG_WARN(module, msg)  logWrite(LOG_LEVEL_WARN, module, msg)
#define LOG_ERROR(module, msg) logWrite(LOG_LEVEL_ERROR, module, msg)
#else
#define LOG_DEBUG(module, msg)
#define LOG_INFO(module, msg)
#define LOG_WARN(module, msg)
#define LOG_ERROR(module, msg)
#endif

// 函数声明
void logInit();
void logWrite(uint8_t level, const char* module, const char* msg);
void logWrite(uint8_t level, const char* module, const String& msg);  // String 重载
void logSetServer(const String& host, uint16_t port, const String& secret);
void uploadLogs();
```

- [ ] **Step 2: Commit**

```bash
git add include/online_logger.h
git commit -m "feat: 添加在线日志系统头文件"
```

---

### Task 4.2: 实现日志系统

**Files:**
- Create: `E:\8266_OTA\src\online_logger.cpp`
- Modify: `E:\8266_OTA\src\main.cpp`

**Interfaces:**
- Consumes: `online_logger.h`, 现有 HTTP 客户端
- Produces: `logWrite()`, `uploadLogs()` 实现

**重要说明：**
- uploadLogs() 只依赖 WiFi 连接状态和 serverHost 配置，不依赖 wsConnected
- 第一版只在关键位置添加日志，不替换现有 DEBUG_SERIAL.printf()

- [ ] **Step 1: 创建实现文件**

```cpp
#include "online_logger.h"
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

// 外部变量声明（需要根据实际项目结构调整）
extern String deviceId;

// 日志缓冲区
static LogEntry logBuffer[LOG_BUFFER_CAPACITY];
static uint8_t logCount = 0;
static uint32_t logSeq = 0;
static unsigned long lastLogUploadMs = 0;

// 服务器配置
static String logServerHost = "";
static uint16_t logServerPort = 80;
static String logUploadSecret = "";

// 辅助函数
static const char* levelToString(uint8_t level) {
    switch (level) {
        case LOG_LEVEL_DEBUG: return "DEBUG";
        case LOG_LEVEL_INFO:  return "INFO";
        case LOG_LEVEL_WARN:  return "WARN";
        case LOG_LEVEL_ERROR: return "ERROR";
        default: return "UNKNOWN";
    }
}

static String escapeJson(const String& str) {
    String result;
    result.reserve(str.length() + 10);
    for (size_t i = 0; i < str.length(); i++) {
        char c = str.charAt(i);
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c;
        }
    }
    return result;
}

// 初始化日志系统
void logInit() {
    logCount = 0;
    logSeq = 0;
    lastLogUploadMs = 0;
}

// 配置日志服务器
void logSetServer(const String& host, uint16_t port, const String& secret) {
    logServerHost = host;
    logServerPort = port;
    logUploadSecret = secret;
}

// 写入日志 - const char* 版本
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
    } else if (level >= LOG_LEVEL_WARN) {
        // 缓冲区满时，WARN/ERROR 尝试替换 DEBUG/INFO
        for (uint8_t i = 0; i < logCount; i++) {
            if (logBuffer[i].level < LOG_LEVEL_WARN) {
                logBuffer[i] = entry;
                break;
            }
        }
    }
    // DEBUG/INFO 在缓冲区满时丢弃
    
    // 同时输出到串口（保持原有调试能力）
    Serial1.printf("[%s][%s] %s\n", levelToString(level), module, msg);
}

// 写入日志 - String 重载版本
void logWrite(uint8_t level, const char* module, const String& msg) {
    logWrite(level, module, msg.c_str());
}

// 上传日志
void uploadLogs() {
    if (!LOG_ENABLED) return;
    
    // 只在 WiFi 已连接、服务器已配置时上传
    if (WiFi.status() != WL_CONNECTED || logServerHost.isEmpty()) return;
    if (millis() - lastLogUploadMs < LOG_UPLOAD_INTERVAL_MS) return;
    if (logCount == 0) return;
    
    unsigned long uploadUptimeMs = millis();
    lastLogUploadMs = uploadUptimeMs;
    
    uint8_t uploadCount = min(logCount, (uint8_t)LOG_MAX_BATCH_SIZE);
    
    // 构造 NDJSON body
    String body;
    body.reserve(4096);
    
    for (uint8_t i = 0; i < uploadCount; i++) {
        LogEntry& entry = logBuffer[i];
        body += "{\"uptimeMs\":" + String(entry.uptimeMs);
        body += ",\"seq\":" + String(entry.seq);
        body += ",\"level\":\"" + String(levelToString(entry.level)) + "\"";
        body += ",\"module\":\"" + String(entry.module) + "\"";
        body += ",\"msg\":\"" + escapeJson(String(entry.message)) + "\"";
        body += "}\n";
    }
    
    // 构造 URL
    String url = "http://" + logServerHost + ":" + String(logServerPort) 
                 + "/admin/device/logs/batch?chipId=" + deviceId 
                 + "&uploadUptimeMs=" + String(uploadUptimeMs);
    
    // 发送 HTTP 请求
    WiFiClient client;
    HTTPClient http;
    http.begin(client, url);
    http.addHeader("Content-Type", "application/x-ndjson");
    if (!logUploadSecret.isEmpty()) {
        http.addHeader("Authorization", "Bearer " + logUploadSecret);
    }
    
    int httpCode = http.POST(body);
    http.end();
    
    if (httpCode >= 200 && httpCode < 300) {
        // 上传成功，移除已上传的日志
        for (uint8_t i = uploadCount; i < logCount; i++) {
            logBuffer[i - uploadCount] = logBuffer[i];
        }
        logCount -= uploadCount;
        Serial1.printf("[LOG] Uploaded %d logs, remaining %d\n", uploadCount, logCount);
    } else {
        Serial1.printf("[LOG] Upload failed, HTTP %d\n", httpCode);
    }
}
```

- [ ] **Step 2: 修改 main.cpp 集成日志系统**

在 `setup()` 函数中添加：
```cpp
#include "online_logger.h"

// 在 setup() 中
logInit();
// 配置日志服务器（根据实际项目结构调整）
logSetServer(cfg.serverHost, cfg.httpPort, cfg.uploadSecret);
LOG_INFO("SYSTEM", "Device starting up");
```

在 `loop()` 函数中添加：
```cpp
uploadLogs();
```

- [ ] **Step 3: 验证编译**

Run: `cd E:\8266_OTA && pio run`
Expected: 编译成功

- [ ] **Step 4: Commit**

```bash
git add include/online_logger.h src/online_logger.cpp src/main.cpp
git commit -m "feat: 实现在线日志系统"
```

---

### Task 4.3: 添加关键日志点位

**Files:**
- Modify: `E:\8266_OTA\src\network\wifi_manager.cpp` (或相关 WiFi 文件)
- Modify: `E:\8266_OTA\src\network\ws_client.cpp`
- Modify: `E:\8266_OTA\src\main.cpp`

**Interfaces:**
- Consumes: `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` 宏
- Produces: 关键事件日志

**重要说明：** 第一版只在关键位置添加日志，不替换现有 DEBUG_SERIAL.printf()

- [ ] **Step 1: 在 WiFi 连接/断开处添加日志**

```cpp
// WiFi 连接成功时
String msg = "Connected to " + WiFi.SSID();
LOG_INFO("WIFI", msg.c_str());

// WiFi 断开时
LOG_WARN("WIFI", "Disconnected");
```

- [ ] **Step 2: 在 WebSocket 连接/断开处添加日志**

```cpp
// WebSocket 连接成功时
LOG_INFO("WS", "Connected to server");

// WebSocket 断开时
LOG_WARN("WS", "Disconnected from server");
```

- [ ] **Step 3: 在设备注册处添加日志**

```cpp
// 注册成功时
LOG_INFO("WS", "Device registered successfully");

// 注册失败时
LOG_ERROR("WS", "Device registration failed");
```

- [ ] **Step 4: 在 HTTP 日志上传处添加日志**

```cpp
// 上传成功时（已在 uploadLogs() 中实现）
// 上传失败时（已在 uploadLogs() 中实现）
```

- [ ] **Step 5: 在传感器异常处添加日志**

```cpp
// 传感器读取失败时
LOG_ERROR("SENSOR", "Failed to read BH1750");
LOG_ERROR("SENSOR", "Failed to read VL53L0X");
```

- [ ] **Step 6: 在灯光控制命令接收处添加日志**

```cpp
// 收到灯光控制命令时
String logMsg = "Received command: brightness=" + String(brightness) + " temp=" + String(temp);
LOG_INFO("LIGHT", logMsg.c_str());
```

- [ ] **Step 7: 验证编译**

Run: `cd E:\8266_OTA && pio run`
Expected: 编译成功

- [ ] **Step 8: Commit**

```bash
git add src/network/wifi_manager.cpp src/network/ws_client.cpp src/main.cpp
git commit -m "feat: 添加关键日志点位"
```

---

## 阶段 5：联调与验证

### Task 5.1: 完整流程验证

**Files:**
- None (使用现有文件)

**Interfaces:**
- Consumes: 所有已实现的接口

- [ ] **Step 1: 启动后端服务**

Run: `cd E:\smart-light-backend && mvn spring-boot:run`

- [ ] **Step 2: 编译并烧录 ESP8266**

Run: `cd E:\8266_OTA && pio run -t upload`

- [ ] **Step 3: 打开串口监视器查看日志输出**

Run: `cd E:\8266_OTA && pio device monitor`
Expected: 看到 `[INFO][SYSTEM] Device starting up` 等日志

- [ ] **Step 4: 启动前端开发服务器**

Run: `cd E:\smart-light-archive && npm run dev`

- [ ] **Step 5: 访问日志面板页面**

打开浏览器访问 `http://localhost:5173/device-logs`

- [ ] **Step 6: 验证日志显示**

1. 选择设备 LAMP-37461B
2. 点击刷新按钮
3. 应该能看到 ESP8266 上传的日志
4. 开启自动刷新，观察日志实时更新

- [ ] **Step 7: 验证日志筛选功能**

1. 测试按级别筛选
2. 测试按模块筛选
3. 测试关键词搜索

- [ ] **Step 8: 验证日志详情展开**

点击某条日志，查看展开的详细信息

---

## 回滚方案

### 后端回滚

如果后端出现问题：
```bash
cd E:\smart-light-backend
git stash  # 暂存所有修改
# 或
git checkout -- .  # 丢弃所有修改
```

### 前端回滚

如果前端出现问题：
```bash
cd E:\smart-light-archive
git stash  # 暂存所有修改
# 或
git checkout -- .  # 丢弃所有修改
```

### ESP8266 回滚

如果 ESP8266 出现问题：
```bash
cd E:\8266_OTA
git stash  # 暂存所有修改
# 或
git checkout -- .  # 丢弃所有修改
```

---

## 验证命令汇总

### 后端验证

```bash
cd E:\smart-light-backend
mvn clean compile  # 编译检查
mvn test           # 运行测试
```

### 前端验证

```bash
cd E:\smart-light-archive
npm run type-check  # TypeScript 类型检查
npm run build       # 构建检查
```

### ESP8266 验证

```bash
cd E:\8266_OTA
pio run             # 编译检查
pio run -t upload   # 烧录
pio device monitor  # 串口监视器
```
