# 灯具跟踪 HTTP / Three.js 模拟器 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 构建可接收 ESP8266 同名 HTTP 控制请求，并实时绘制灯具朝向的本地调试器。

**Architecture:** 原生 Node HTTP 服务托管页面、实现 `POST /lamp/control` 和 SSE 状态广播；浏览器用 Three.js 渲染墙、灯具、光束与落点。

**Tech Stack:** Node.js 内置模块、浏览器 Fetch/EventSource、Three.js CDN。

## Global Constraints

- 接口是 `POST /lamp/control`，且支持 CORS `OPTIONS` 预检。
- `pan`、`tilt`、`brightness`、`temp` 分别限位为 -45..45、-90..90、0..100、2700..6500。
- 可视化的 `pan < 0` 向左，`tilt < 0` 朝灯具前方的墙。
- 不修改任何现有 ESP8266 固件文件。

---

### Task 1: 可测试的 HTTP 与 SSE 服务

**Files:**

- Create: `tracking-simulator/server.mjs`
- Create: `tracking-simulator/test/server.test.mjs`

**Interfaces:**

- Produces: `createSimulatorServer({ port, staticRoot }) -> { listen(), close(), getState() }`。
- Consumes: HTTP `POST /lamp/control`，SSE `GET /events`。

- [ ] **Step 1: 写入失败测试**

```js
import test from 'node:test';
import assert from 'node:assert/strict';
import { createSimulatorServer } from '../server.mjs';

test('clamps a lamp control command', async () => {
  const simulator = createSimulatorServer({ port: 0 });
  const port = await simulator.listen();
  const response = await fetch(`http://127.0.0.1:${port}/lamp/control`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ pan: -90, tilt: -100, brightness: 200, temp: 9000 }),
  });
  assert.equal(response.status, 200);
  assert.deepEqual(simulator.getState().control, { pan: -45, tilt: -90, brightness: 100, temp: 6500 });
  await simulator.close();
});
```

- [ ] **Step 2: 运行并确认失败**

Run: `node --test tracking-simulator/test/server.test.mjs`

Expected: FAIL，`../server.mjs` 尚不存在。

- [ ] **Step 3: 实现服务**

```js
export const clamp = (value, min, max) => Math.min(max, Math.max(min, value));
// applyControl 保留缺失字段，校验 Number.isFinite，创建 { control, raw, receivedAt } 状态。
// /lamp/control 对空 body、无效 JSON 返回 400，对有效 POST 返回 {"result":"OK"}，
// 对 OPTIONS 返回 204；更新后向所有 /events 客户端发送 event: state。
```

- [ ] **Step 4: 运行测试确认通过**

Run: `node --test tracking-simulator/test/server.test.mjs`

Expected: PASS。

- [ ] **Step 5: 提交服务与测试**

```bash
git add tracking-simulator/server.mjs tracking-simulator/test/server.test.mjs
git commit -m "feat: add tracking simulator HTTP service"
```

### Task 2: Three.js 方向可视化与手动请求

**Files:**

- Create: `tracking-simulator/index.html`

**Interfaces:**

- Consumes: `/events` 的 `state` 事件和 `POST /lamp/control`。
- Produces: 画面、当前状态、原始 JSON、连接状态与请求日志。

- [ ] **Step 1: 确定可检验 DOM 与坐标契约**

页面包含 `#connection-status`、`#current-pan`、`#current-tilt`、`#raw-json`、`#request-log`、`#payload-input`、`#send-payload`。墙面放在零位前向的 `-Z` 方向，灯具零位朝墙；控制云台旋转如下：

```js
gimbal.rotation.set(
  THREE.MathUtils.degToRad(control.tilt),
  THREE.MathUtils.degToRad(control.pan),
  0,
  'YXZ',
);
```

- [ ] **Step 2: 实现页面**

创建深色双栏界面。Three.js 场景必须包含网格地面、墙面、云台灯具、半透明光锥、中心射线、墙面落点；这些方向元素作为 `gimbal` 子节点。亮度改变光强，色温通过暖橙到冷白插值改变光色。EventSource 调用 `updateScene(state.control)`；手动输入框以 Fetch 发送 JSON 并记录成功或错误。

- [ ] **Step 3: 启动并做视觉验收**

Run: `node tracking-simulator/server.mjs --port 4173`

Expected: 页面显示 SSE 已连接。发送 `{"pan":-11.2,"tilt":-18.1,"brightness":80,"temp":4000}` 后，读数匹配，光束从观察者视角左转，并面向墙。

- [ ] **Step 4: 提交页面**

```bash
git add tracking-simulator/index.html
git commit -m "feat: visualize lamp tracking direction"
```

### Task 3: 边界验证与使用说明

**Files:**

- Modify: `tracking-simulator/test/server.test.mjs`
- Create: `tracking-simulator/README.md`

**Interfaces:**

- Consumes: 已完成的服务和页面。
- Produces: 覆盖预检、坏 JSON、部分字段保留和 SSE 的测试，及可复制的启动/请求说明。

- [ ] **Step 1: 补充失败断言**

```js
test('supports preflight and preserves omitted fields', async () => {
  // 断言 OPTIONS 为 204 且含 CORS；先 POST { pan: -11.2 }，
  // 再 POST { tilt: -18.1 }，最终 control 的 pan 仍为 -11.2。
});
```

- [ ] **Step 2: 完成验证与 README**

README 写明 Node 18+、`node server.mjs`、访问地址、与固件相同的 curl 请求、字段范围和“pan 负向左 / tilt 负朝墙”。服务测试断言 200、400、204、限位、缺失字段保留与 SSE 状态事件。

- [ ] **Step 3: 运行完整验证**

Run: `node --test tracking-simulator/test/server.test.mjs`

Expected: PASS。

- [ ] **Step 4: 提交验证与说明**

```bash
git add tracking-simulator/README.md tracking-simulator/test/server.test.mjs
git commit -m "test: verify tracking simulator protocol"
```
