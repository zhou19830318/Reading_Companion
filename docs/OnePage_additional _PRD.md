# OnePage「生活助手」页面追加需求与实现说明

## 1. 需求背景

OnePage 当前已经具备：

- ESP32-C6 + Wi‑Fi 连接能力；
- 视频中展示的 AI 助手入口和语音交互流程；
- 语音录入、自动转写和本地记录能力；
- 实体按键驱动的线性页面交互；
- Micro SD 卡本地存储；
- 现有 HTTP 网络访问和 Web 配置页面；
- 基于 JSON 的设置、阅读状态和缓存机制。

本需求新增一个独立的 **「生活助手」页面**。页面不直接接入天气服务或待办服务，而是统一从用户部署的 **OpenClaw 服务器**获取数据。

页面打开时执行一次同步：

1. 先读取本地缓存并立即显示；
2. 后台连接 OpenClaw 服务器；
3. 获取待办事务和天气信息；
4. 校验成功后写入本地缓存；
5. 刷新页面并显示最新数据。

如果网络或服务器不可用，仍然显示最近一次成功缓存，并明确显示缓存时间。

---

## 2. 产品目标与非目标

### 2.1 目标

- 用户打开页面即可看到今天的天气和待办事项；
- 页面加载不因网络请求长时间卡死；
- 有网时自动同步，无网时继续显示缓存；
- OpenClaw 作为唯一数据来源和服务编排层；
- OnePage 只负责显示、缓存和简单导航；
- 适配 4.26 英寸黑白电子纸和三键操作；
- 将网络失败、数据过期和认证失败清晰区分。

### 2.2 第一版不做

- 不在 ESP32-C6 上直接调用天气供应商；
- 不在设备端运行 OpenClaw 或大模型；
- 不做复杂日历月视图；
- 不做设备端新增待办的长文本输入；
- 不做后台持续轮询；
- 不把 OpenClaw 的原始聊天记录全部同步到设备；
- 不允许服务器返回任意 HTML 或脚本在设备上执行。

待办的创建、编辑和删除继续由 OpenClaw 或其手机端管理；OnePage 第一版以**只读展示**为主。后续可增加“完成待办”操作。

---

## 3. 页面入口与交互

### 3.1 首页入口

在当前 HomeActivity 的菜单中新增：

```text
生活助手
```

建议放在「最近书籍」之后、「AI 助手」附近，方便用户把它理解为生活信息入口。

### 3.2 页面结构

页面采用纵向线性布局：

```text
生活助手                         09:30
------------------------------------
天气
常州  阴  24°C
今日  22°C ~ 28°C
最后更新：今天 09:25

待办事务  3 项
[ ] 归还图书馆的书      今天 18:00
[ ] 完成物理作业        今天
[ ] 给妈妈打电话        明天

数据：刚刚同步
```

由于屏幕为黑白电子纸：

- 使用分隔线、方框和粗体区分区域；
- 不依赖颜色表示优先级；
- 使用 `!`、`今天`、`逾期` 等文字标签；
- 长文本截断并可进入详情页；
- 页面不使用高频动画。

### 3.3 三键操作

| 操作 | 功能 |
|---|---|
| 上键/左键 | 上移、查看上一条待办 |
| 下键/右键 | 下移、查看下一条待办 |
| 确认键 | 进入天气详情或待办详情 |
| 返回键/长按返回 | 退出生活助手页面 |
| 页面打开时确认键 | 可提供“立即同步”操作，非必要 |

具体按键名称沿用当前固件的前面板映射，不在新页面中硬编码物理位置。

### 3.4 页面打开行为

页面打开后必须遵循以下顺序：

```text
读取本地缓存
  ↓ 立即显示缓存内容
启动一次同步任务
  ↓ 网络成功
校验响应并写入缓存
  ↓
局部或整页刷新，显示“刚刚同步”
```

网络同步不能阻塞本地缓存首次显示。

### 3.5 页面状态文案

必须至少支持以下状态：

```text
正在同步…
刚刚同步
上次同步：今天 08:40
离线，显示缓存
缓存已过期：昨天 18:20
服务器认证失败
服务器数据格式错误
```

不要只显示“加载失败”，否则用户无法判断是没有网络、服务器异常还是配置错误。

---

## 4. OpenClaw 服务接口约定

由于当前未提供 OpenClaw 的固定 API 文档，建议在 OpenClaw 服务器侧提供一个稳定的适配接口，而不是让固件直接理解 OpenClaw 内部数据结构。

OnePage 只依赖一个聚合接口：

```http
GET /api/onepage/life-assistant
Authorization: Bearer <device-token>
Accept: application/json
If-None-Match: "<cached-etag>"
```

建议通过 HTTPS 提供。若 OpenClaw 当前接口名称不同，只需要在 OpenClaw 侧增加适配路由，固件不应绑定内部 Agent、数据库或插件格式。

### 4.1 成功响应

```json
{
  "schemaVersion": 1,
  "serverTime": "2026-09-30T09:30:00+08:00",
  "timezone": "Asia/Shanghai",
  "generatedAt": "2026-09-30T09:29:58+08:00",
  "weather": {
    "location": "常州",
    "condition": "阴",
    "temperatureC": 24,
    "lowC": 22,
    "highC": 28,
    "humidity": 76,
    "wind": "东风 2 级",
    "advice": "建议携带雨具",
    "updatedAt": "2026-09-30T09:25:00+08:00"
  },
  "todos": [
    {
      "id": "todo_01HXYZ",
      "title": "完成物理作业",
      "detail": "完成第三章课后题 1-8",
      "status": "open",
      "priority": "normal",
      "dueAt": "2026-09-30T20:00:00+08:00",
      "updatedAt": "2026-09-29T21:10:00+08:00",
      "source": "openclaw"
    }
  ],
  "etag": "life-20260930-092958"
}
```

### 4.2 字段约束

- `schemaVersion`：用于未来兼容；第一版固定为 `1`；
- `serverTime`：用于设备时间校准或展示；
- `timezone`：防止待办时间错位；
- `generatedAt`：整份数据生成时间；
- `weather`：允许为 `null`，天气失败不应导致待办全部不可用；
- `todos`：允许为空数组；
- `id`：服务端稳定 ID，不能用数组下标；
- `status`：第一版至少支持 `open`、`done`、`cancelled`；
- `dueAt`：允许为空，表示无截止时间；
- `detail`、`advice`：可选，设备端必须能处理缺失；
- 所有字符串需限制最大长度，避免异常响应耗尽内存。

### 4.3 HTTP 状态码处理

| 状态码 | 设备行为 |
|---:|---|
| 200 | 解析、校验、写缓存、显示最新数据 |
| 304 | 保留现有缓存，仅更新时间状态 |
| 401/403 | 停止重试，显示“服务器认证失败” |
| 404 | 显示“生活助手接口不存在” |
| 408/429 | 保留缓存，显示“稍后重试” |
| 500–599 | 保留缓存，显示“服务器暂时不可用” |
| JSON 解析失败 | 丢弃本次响应，不覆盖旧缓存 |

---

## 5. 本地缓存设计

### 5.1 文件位置

建议新增独立目录，不污染现有阅读器缓存：

```text
/.onepage/
└── life_assistant.json
```

如果项目希望所有设备数据统一放在 `.crosspoint` 下，也可以使用：

```text
/.crosspoint/life_assistant.json
```

本需求推荐 `/.onepage/`，便于未来把生活助手从阅读器核心中独立出来。

### 5.2 缓存文件结构

```json
{
  "cacheVersion": 1,
  "savedAt": "2026-09-30T09:30:01+08:00",
  "lastSuccessAt": "2026-09-30T09:29:58+08:00",
  "etag": "life-20260930-092958",
  "payload": {
    "schemaVersion": 1,
    "serverTime": "2026-09-30T09:30:00+08:00",
    "timezone": "Asia/Shanghai",
    "weather": {},
    "todos": []
  }
}
```

### 5.3 写入安全

禁止直接覆盖正在使用的缓存文件。采用：

```text
life_assistant.json.tmp
  ↓ 写入并 flush
校验 JSON 完整性
  ↓
rename 为 life_assistant.json
```

如果断电或写入失败，保留旧的 `life_assistant.json`。

### 5.4 缓存有效期

建议：

- 天气：显示缓存，但超过 6 小时标记为“天气已过期”；
- 待办：显示缓存，但超过 24 小时标记为“待办可能已更新”；
- 无论是否过期，都不自动删除；
- 用户没有网络时始终允许查看最近一次缓存。

缓存过期只是显示状态，不应让页面变为空白。

---

## 6. 网络同步策略

### 6.1 同步触发时机

第一版仅实现以下触发：

1. 打开生活助手页面时同步一次；
2. 用户在页面中选择“立即同步”时同步一次；
3. 从睡眠唤醒后不自动同步，避免每次唤醒耗电；
4. 不做定时后台轮询。

这符合电子纸设备低功耗定位，也避免 OpenClaw 服务被高频请求。

### 6.2 请求参数

```text
连接超时：3 秒
读取超时：6 秒
单次请求总超时：10 秒
失败重试：最多 1 次
重试间隔：500 ms
```

只在打开页面时建立网络请求。网络失败后立刻回退缓存，不连续重试。

### 6.3 同步流程伪代码

```cpp
void LifeAssistantActivity::onEnter() {
    cache_.load();
    renderCached(cache_);

    if (!settings_.lifeAssistantEnabled) {
        renderStatus("未配置服务器");
        return;
    }

    renderStatus("正在同步…");
    syncTask_.start([this]() {
        auto response = openClawClient_.fetchSnapshot(cache_.etag());

        if (response.notModified()) {
            cache_.markChecked(now());
            renderSyncStatus("刚刚同步");
            return;
        }

        if (!response.ok()) {
            renderSyncStatus(toUserStatus(response.error()));
            return;
        }

        LifeAssistantSnapshot snapshot;
        if (!parseAndValidate(response.body(), snapshot)) {
            renderSyncStatus("服务器数据格式错误");
            return;
        }

        if (!cache_.saveAtomically(snapshot, response.etag())) {
            renderSyncStatus("本地缓存写入失败");
            return;
        }

        renderSnapshot(snapshot);
        renderSyncStatus("刚刚同步");
    });
}
```

网络任务不能在电子纸渲染主循环中同步阻塞执行，应放到独立 FreeRTOS 任务或现有网络任务机制中，并通过线程安全的结果队列回到 Activity。

---

## 7. 配置项

建议在系统设置中新增「生活助手」分组：

```text
生活助手
├── 启用生活助手       开 / 关
├── OpenClaw 服务器地址
├── 设备令牌
├── 请求超时时间
├── 默认天气地点       可选
└── 测试连接
```

其中：

- 服务器地址允许包含 `https://` 和端口；
- 令牌不能在普通设置页面明文回显；
- 测试连接只显示成功/失败和服务器时间，不显示令牌；
- 配置保存后不要立即自动频繁请求；
- 若地址为空，页面显示“未配置服务器”。

如果视频中的 AI 助手已经有 OpenClaw 地址和凭据配置，建议复用已有配置，不新增第二套服务器配置。可抽象为共享的 `OpenClawConnectionSettings`。

---

## 8. 推荐实现拆分

### 8.1 Activity 页面

新增：

```text
src/activities/life_assistant/
├── LifeAssistantActivity.h
└── LifeAssistantActivity.cpp
```

职责：

- 页面状态机；
- 三键事件处理；
- 天气摘要和待办列表渲染；
- 进入/退出同步状态；
- 调用客户端和缓存类，不直接拼接 HTTP。

### 8.2 OpenClaw 客户端

新增：

```text
src/network/OpenClawClient.h
src/network/OpenClawClient.cpp
```

职责：

- 拼接 URL；
- 添加 Bearer Token；
- 发起 HTTPS/HTTP 请求；
- 处理超时和状态码；
- 返回原始响应和错误类型；
- 不负责页面显示。

如果现有 AI 助手已经有 OpenClaw 网络客户端，应将通用请求、认证和 TLS 逻辑抽出复用。

### 8.3 数据模型

新增：

```text
src/life_assistant/LifeAssistantModels.h
```

建议类型：

```cpp
struct WeatherSnapshot {
    std::string location;
    std::string condition;
    int temperatureC = 0;
    int lowC = 0;
    int highC = 0;
    std::string advice;
    std::string updatedAt;
};

struct TodoItem {
    std::string id;
    std::string title;
    std::string detail;
    std::string status;
    std::string priority;
    std::string dueAt;
};

struct LifeAssistantSnapshot {
    int schemaVersion = 1;
    std::string serverTime;
    std::string timezone;
    std::string generatedAt;
    WeatherSnapshot weather;
    std::vector<TodoItem> todos;
};
```

### 8.4 本地缓存

新增：

```text
src/life_assistant/LifeAssistantCache.h
src/life_assistant/LifeAssistantCache.cpp
```

职责：

- 从 SD 卡读取缓存；
- 校验 `cacheVersion`；
- 原子写入；
- 记录 `lastSuccessAt`、`etag`；
- 提供缓存是否过期的判断；
- 不负责网络访问。

### 8.5 设置与菜单注册

需要修改：

- HomeActivity：添加生活助手菜单项；
- ActivityManager：注册页面跳转；
- SettingsList / SettingsActivity：增加服务器配置；
- JsonSettingsIO：持久化配置；
- i18n 字符串：新增页面文案和错误状态；
- 构建文件：加入新增 `.cpp` 文件。

实际文件名以当前仓库 Activity 和设置注册方式为准。

---

## 9. 安全要求

### 9.1 传输安全

优先使用 HTTPS：

```text
https://openclaw.example.com/api/onepage/life-assistant
```

不建议在公网环境长期使用明文 HTTP 传输令牌和个人待办。

### 9.2 认证

- 使用每台设备独立 Bearer Token；
- Token 不显示在电子纸上；
- 日志中禁止输出完整 Token；
- 服务器端支持吊销设备 Token；
- 服务端只返回当前设备授权的数据。

### 9.3 响应限制

设备端必须限制：

- 最大响应体大小；
- 最大待办数量，例如 50 条；
- 单条标题和详情长度；
- 天气文本长度；
- JSON 嵌套深度。

服务器返回异常或超大数据时，应保留旧缓存，不直接写入 SD 卡。

### 9.4 当前 Web 服务安全提醒

仓库现有本地 Web 服务文档说明：HTTP 服务当前没有认证，局域网内访问者可以访问，热点模式也会创建开放网络。因此：

- OpenClaw Token 不应通过现有无认证文件上传接口明文传输；
- 新增配置接口应至少避免在 GET 响应中返回 Token；
- 测试阶段可以先在受信任局域网使用；
- 正式使用前应为生活助手配置增加脱敏和写入保护。

---

## 10. 两种可行架构

| 方案 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| 设备直连 OpenClaw（推荐） | OnePage 通过 Wi‑Fi 直接调用聚合接口，手机只负责初始配置 | 操作最少；符合视频中设备已有联网 AI 助手；打开页面即可同步 | 需要在 ESP32-C6 上处理 HTTPS、Token 和 JSON；服务器必须对设备可达 |
| 手机中转 | OnePage 只连接手机/PWA，手机调用 OpenClaw 后再把结果同步到设备 | 手机处理 TLS、复杂 JSON 和网络切换更容易；OpenClaw 可只暴露给手机 | 操作链更长；手机必须在场；不是真正的“打开页面即同步” |

建议第一版采用**设备直连 OpenClaw**，但把 OpenClaw 适配接口设计成单一聚合接口。若当前 OpenClaw 只能通过手机访问，再切换到手机中转，不改变页面、缓存和数据模型。

---

## 11. 验收标准

### 功能验收

- [ ] Home 菜单出现「生活助手」；
- [ ] 第一次进入无缓存时显示“暂无本地数据”，随后尝试同步；
- [ ] 有缓存时 1 秒内先显示缓存内容；
- [ ] 网络成功后显示天气和待办最新数据；
- [ ] OpenClaw 返回 304 时不重复写入完整缓存；
- [ ] 网络失败时保留旧缓存并显示缓存时间；
- [ ] 天气为空时待办仍可显示；
- [ ] 待办为空时显示“暂无待办”；
- [ ] 待办标题过长时正确截断或进入详情；
- [ ] 可通过三键浏览列表和详情；
- [ ] Token 不在页面、日志和普通 API 响应中明文显示；
- [ ] 断电或写入中断不会破坏上一次有效缓存。

### 性能与功耗验收

- [ ] 网络请求有明确超时，不阻塞页面主循环；
- [ ] 单次页面打开最多自动重试 1 次；
- [ ] 页面退出后停止同步任务；
- [ ] 失败时不持续唤醒 Wi‑Fi；
- [ ] 大 JSON 和异常响应不会导致崩溃或内存耗尽；
- [ ] 页面刷新次数控制在电子纸可接受范围内。

### 兼容性验收

- [ ] OpenClaw 返回可选字段缺失时仍能显示核心内容；
- [ ] `schemaVersion` 不支持时拒绝覆盖旧缓存；
- [ ] 时区变化不会把待办日期显示错；
- [ ] 服务器时间异常时不覆盖本地时间；
- [ ] 无 SD 卡或缓存目录异常时显示明确错误。

---

## 12. 推荐开发顺序

### 第 1 步：先做静态页面和缓存

用本地固定 JSON 验证：

- 页面布局；
- 三键导航；
- 长文本截断；
- 天气和待办显示；
- 缓存过期文案。

### 第 2 步：实现 OpenClawClient

先用一个模拟服务器返回固定 JSON，验证：

- 成功；
- 304；
- 超时；
- 401；
- 500；
- JSON 错误。

### 第 3 步：接入真实 OpenClaw

OpenClaw 侧只实现 `/api/onepage/life-assistant` 聚合接口。固件不直接依赖 OpenClaw 内部插件结构。

### 第 4 步：接入现有 AI 助手配置

如果视频中的 AI 助手已经保存了服务器地址或凭据，将其抽象为共享配置，避免用户重复输入。

### 第 5 步：做断网、断电和低功耗测试

重点测试：

- 打开页面时断网；
- DNS 失败；
- HTTPS 证书失败；
- OpenClaw 慢响应；
- SD 卡写入中断；
- 设备睡眠唤醒；
- 缓存数据跨版本升级。

---

## 13. 未来可扩展项

第一版稳定后，可以增加：

- 在设备上将待办标记为完成，并回传 OpenClaw；
- 语音创建待办；
- 天气详情和未来 3 天预报；
- 结合阅读进度生成“今天阅读任务”；
- 生活助手与学习助手共用提醒时间线；
- OpenClaw 推送提醒，但不要在第一版引入长连接和高频轮询。

---

## 结论

这个追加功能最适合采用：

```text
OnePage 生活助手页面
    ↓ 页面打开
先显示 SD 卡缓存
    ↓ 后台一次性请求
OpenClaw 聚合接口
    ↓ 成功
更新待办 + 天气缓存
    ↓ 失败
继续显示旧数据并提示缓存时间
```

实现重点不是做一个复杂的生活管理系统，而是保证：

> **打开就能看、联网就更新、断网不失效、数据不丢失、按键能完成全部浏览。**
