<img src="https://r2cdn.perplexity.ai/pplx-full-logo-primary-dark%402x.png" style="height:64px;margin-right:32px"/>

# OnePage 软件功能需求文档（v4.0 最终版）

**文档版本**：v4.0（最终版）\
**日期**：2026-09-28\
**范围**：仅软件功能需求 + 技术栈 + 开发难度 + 项目建议\
**排除**：外观工业设计、硬件结构、配件等非软件内容

**基于**：

- 硬件：OnePage ESP32-C61 主板（bsp_onepage_c61）[^1][^2]
- 芯片：ESP32-C61 单核 RISC-V 160 MHz，320 KB SRAM，可选 PSRAM[^3][^4]
- 显示：4.26" 800×480 黑白电子纸，SSD1677，旋转为 480×800 竖屏[^1]
- 输入：7 键（3 GPIO 侧键 + 4 ADC 正面键）[^1]
- 存储：Micro SD（共享 SPI 总线）[^1]
- 音频：PDM 麦克风，16 kHz 单声道 PCM[^1]
- 无线：Wi‑Fi + BLE（需 ≥2 MB app 分区，Flash 40 MHz）[^5][^1]
- 固件基础：crosspoint-onepage（CrossPoint Reader 移植版）[^6][^7]
- AI 后端：OpenClaw（Gateway + Skills + Memory 架构）

**本次更新**：在 v3.1 基础上完成硬件核实、架构重排、P0 收缩、数据协议细化、验收指标明确，形成可直接驱动开发与测试的最终需求基线。

______________________________________________________________________

## 1. 软件产品定位

> OnePage 是一款**本地优先**的电子纸阅读器。第一目标是**可靠阅读、快速恢复和良好的中文排版**；统计、计划和 AI 伴读均为**可选模块**。所有 AI 功能必须在网络不可用时安全降级，不得影响打开书籍、翻页、保存进度和进入深度睡眠。

软件必须严格遵守四条硬性原则：

1. **纯阅读可独立运行**：没有 Wi‑Fi、OpenClaw 或 AI 服务时，核心阅读功能完整可用。
2. **数据写入可恢复**：任何单次断电都不得破坏书库、进度、计划和历史主数据。
3. **主动优于主动推送**：AI 默认由用户触发，系统建议不得打断阅读。
4. **协议隔离**：固件只依赖 OnePage Agent Bridge，不直接耦合具体 LLM 或 OpenClaw 内部实现。

______________________________________________________________________

## 2. 硬件事实与约束（软件视角）

### 2.1 芯片与内存

- ESP32-C61：单核 RISC‑V，最高 160 MHz，320 KB SRAM，256 KB ROM。[^4][^3]
- PSRAM：芯片支持外部 PSRAM，但是否焊接取决于具体模组和板卡设计；ESP‑IDF 可通过 `esp_psram` 组件使用。[^8][^9][^3]
- Flash：项目 BSP 明确要求 16 MB Flash 配置，40 MHz 频率，否则会出现启动问题。[^1]

软件不得假设固定 PSRAM 大小，必须通过启动日志和 `board_caps()` 动态探测。

### 2.2 显示与输入

- 电子纸：Osptek EPD0426A02，800×480，SSD1677，BSP 中已旋转为 480×800 竖屏并封装为 moui 后端。[^1]
- 按键：3 个 GPIO 侧键（Wake/Prev/Next）+ 4 个 ADC 正面键（Back/Left/Right/Enter），支持长按、双击等事件。[^1]


### 2.3 存储与电源

- Micro SD：与 EPD 共享 SPI 总线，支持卡检测；BSP 提供 `board_sd_mount/unmount/size_mb/present` 等接口。[^1]
- 电池：ADC 测量，1:1 分压，`board_battery_mv()` 自动 2× 换算为真实毫伏值。[^1]
- USB/充电：`board_usb_plugged()`、`board_charge_enable()` 可用。[^1]
- 休眠：支持深度睡眠，唤醒源诊断通过 `board_wake_cause()`。[^1]


### 2.4 音频与无线

- 麦克风：PDM + 软件 CIC‑3 解码，输出 16 kHz 单声道 PCM。[^1]
- Wi‑Fi/BLE：BSP 支持扫描和初始化；无线固件要求 app 分区 ≥2 MB。[^5][^1]


### 2.5 关键工程约束

- Flash 频率必须为 40 MHz，80 MHz 会导致启动镜像损坏。[^1]
- 共享 SPI 单次传输 ≤32767 字节，BSP 显示桥已处理 16 KB 分块。[^1]
- GPIO27 同时控制 EPD 复位和 SD/MIC 电源使能，软件不得随意拉低。[^1]

______________________________________________________________________

## 3. 软件功能需求清单

### 3.1 核心阅读功能（P0）

| ID | 功能名称 | 描述 | 优先级 |
| :-- | :-- | :-- | :-- |
| FR‑01 | 进度与缓存高可靠 | 低电量/休眠/复位前强制保存；缓存校验与自动修复；进度本地双写（A/B 槽位）；异常恢复后准确回到上次位置 | P0 |
| FR‑02 | 传书体验简化 | 扫码或 Web 直达上传；断点续传；自动 EPUB 优化；实时进度反馈；支持 WebDAV / Calibre OPDS（可选） | P0 |
| FR‑03 | 中文排版优化 | CJK 渲染性能与断行优化；多字体快速切换；行距/字重/首行缩进预设；竖屏 480×800 适配 | P0 |
| FR‑04 | 书签与笔记 | 章节/位置书签；轻量文本笔记（Markdown）；全部绑定书籍和位置；离线可用 | P0 |

### 3.2 阅读增强功能（P1）

| ID | 功能名称 | 描述 | 优先级 |
| :-- | :-- | :-- | :-- |
| FR‑05 | 轻量阅读统计 | 今日/累计时长、连续天数、字数；极简展示，不打断阅读 | P1 |
| FR‑06 | 按键快捷映射强化 | 侧键/正面键支持长按/双击映射（查词、AI、章节跳转、书签、笔记） | P1 |
| FR‑07 | 离线词典 + 生词本 | 长按或快捷键查词，结果简洁显示；一键加入生词本，支持间隔复习 | P1 |

### 3.3 阅读历史与陪读（本地核心，P0/P1）

| ID | 功能名称 | 描述 | 优先级 |
| :-- | :-- | :-- | :-- |
| RH‑01 | 阅读会话追踪 | 基于“打开书/翻页 → 无输入超时 → 休眠/退出”的状态机，记录有效阅读时长 | P0 |
| RH‑02 | 每日历史聚合 | 按日聚合总时长、会话数、翻页数、按书分布；写入 SD 卡聚合文件 | P0 |
| RH‑03 | 阅读热力图 | 以 GitHub 贡献图风格展示最近 12–16 周每日阅读时长；4 级灰度（无/少/中/多） | P1 |
| RP‑01 | 阅读计划管理 | 创建/编辑/归档计划：目标书、目标日期、每周目标（分钟为主，页数为辅）；支持多计划并行 | P1 |
| RP‑02 | 空余时间设定 | 用户设置周期性空余时段（工作日/周末、具体钟点）；支持“灵活”模式 | P1 |
| RP‑03 | 本地规则建议 | 无 AI 时，根据剩余页数/天数/历史速度生成简单每日建议（分钟数） | P1 |

### 3.4 AI 深度融入功能（基于 OpenClaw，P1/P2）

| ID | 功能名称 | 描述 | 优先级 |
| :-- | :-- | :-- | :-- |
| AI‑01 | 上下文感知阅读助手 | Agent 获取当前书名、章节、位置、选中/当前页文本；支持“解释这段”“总结本章”等 | P1 |
| AI‑02 | 语音笔记自动关联 | 长按唤醒后口述，自动绑定当前书/页，保存为 Markdown | P1 |
| AI‑03 | 智能生词与概念本 | 查词或 AI 解释后一键收藏，支持按书分类与间隔复习 | P1 |
| AI‑04 | 适时主动陪伴 | 可选晨间简报、读完章节轻提示；全部可关闭，频率可控；仅睡眠界面或下次唤醒显示 | P1 |
| AI‑05 | 跨书知识关联 | 基于本地书库的相似观点/相关书籍推荐 | P2 |
| AI‑06 | 对话生图（电子纸优化） | 生成黑白/灰度插图、人物关系图，可作为笔记或壁纸 | P2 |
| AI‑07 | 任务与真实世界闭环 | 利用 OpenClaw Skills 完成设提醒、查资料总结、笔记外发等 | P2 |
| **AI‑08** | **智能陪读助手（AI 规划 + 温和提醒）** | **OpenClaw 根据计划进度、空余时间、历史完成率，生成未来 7 天的建议阅读时段与时长；设备端以非模态方式展示，支持一键忽略或调整。详见第 5 章。** | **P1** |

**AI 交互统一规范**：

- 短按侧边键 → 普通唤醒
- 长按侧边键 → AI 语音模式
- 阅读中快捷键 → 上下文提问
- 陪读计划相关操作集中在独立「陪读」入口或语音指令，避免污染阅读界面

______________________________________________________________________

## 4. 数据模型与存储结构

### 4.1 目录结构

```text
.crosspoint/
├── state/
│   ├── reader_state_a.bin
│   └── reader_state_b.bin
├── history/
│   ├── 2026-09-28.log
│   └── 2026-09-29.log
├── aggregate/
│   └── 2026.json
├── plans/
│   └── plan_xxx.json
├── books/
│   └── ...
├── cache/
│   └── ...
└── export/
    └── ...
```

- `state/`：阅读器状态 A/B 双槽位，含当前书、位置、字体设置等。
- `history/`：当日阅读会话 append‑only 日志。
- `aggregate/`：按年聚合的每日统计。
- `plans/`：阅读计划 JSON。
- `export/`：用户导出数据。


### 4.2 阅读会话日志（append‑only）

```json
{
  "schema_version": 1,
  "entries": [
    {
      "ts_start": "2026-09-28T21:05:10+08:00",
      "ts_end": "2026-09-28T21:35:40+08:00",
      "book_id": "sha256:...",
      "chapter_id": "chapter-08",
      "position": {
        "cfi": "...",
        "byte_offset": 123456
      },
      "duration_sec": 1830,
      "pages_advanced": 7
    }
  ]
}
```


### 4.3 每日聚合

```json
{
  "date": "2026-09-28",
  "total_sec": 1840,
  "sessions": 3,
  "pages_advanced": 27,
  "books": {
    "sha256:...": 1720,
    "sha256:...": 120
  },
  "updated_at": "2026-09-28T23:10:00+08:00",
  "schema_version": 1
}
```


### 4.4 阅读计划

```json
{
  "id": "plan_20260928_001",
  "book_id": "sha256:...",
  "status": "active",
  "created_at": "2026-09-28T08:00:00+08:00",
  "target_date": "2026-10-31",
  "goal": {
    "type": "minutes_per_week",
    "value": 180
  },
  "priority": 1,
  "timezone": "Asia/Hong_Kong",
  "paused_until": null,
  "notification_policy": "sleep_or_next_wakeup"
}
```


### 4.5 空余时间

```json
{
  "slots": [
    {
      "name": "工作日晚上",
      "days": [1, 2, 3, 4, 5],
      "start": "21:00",
      "end": "22:30",
      "flexible": false
    },
    {
      "name": "周末上午",
      "days": [0, 6],
      "start": "10:00",
      "end": "11:30",
      "flexible": false
    }
  ],
  "updated_at": "2026-09-28T08:00:00+08:00"
}
```


______________________________________________________________________

## 5. 智能陪读助手（AI‑08）详细设计

### 5.1 功能拆解

| 子 ID | 子功能 | 描述 | 优先级 |
| :-- | :-- | :-- | :-- |
| AI‑08.1 | 阅读计划管理 | 创建/编辑/归档计划：目标书、目标日期、每周目标（分钟为主）；支持多计划并行 | P1 |
| AI‑08.2 | 空余时间设定 | 用户设置周期性空余时段（工作日/周末、具体钟点）；支持“灵活”模式 | P1 |
| AI‑08.3 | 本地规则建议 | 无 AI 时，根据剩余页数/天数/历史速度生成简单每日建议（分钟数） | P1 |
| AI‑08.4 | AI 建议性时间规划 | OpenClaw 根据计划进度、空余时间、历史完成率，生成未来 7 天的建议阅读时段与时长 | P1 |
| AI‑08.5 | 温和提醒 | 可选开启：在睡眠界面或下次唤醒时轻提示；支持“今日跳过”；禁止阅读中弹窗 | P1 |
| AI‑08.6 | 阅读热力图 | 以 GitHub 贡献图风格展示最近 12–16 周的每日阅读时长；4 级灰度 | P1 |

### 5.2 本地规则建议算法（降级模式）

当 OpenClaw 不可用时，设备端运行简化逻辑：

1. 计算剩余天数：\

$$
D\_{\\text{remain}} = \\max(1, \\text{target_date} - \\text{today})
$$
2. 估算剩余阅读时长（基于历史平均速度）：\

$$
T\_{\\text{remain}} = \\frac{\\text{remaining_pages}}{\\text{avg_pages_per_minute}}
$$
3. 每日建议时长：\

$$
T\_{\\text{daily}} = \\frac{T\_{\\text{remain}}}{D\_{\\text{remain}}}
$$
4. 将 $T_{\text{daily}}$ 映射到用户空余时段，给出 1–3 个建议时间段（例如 20–30 分钟一段）。

输出示例（设备端文案）：

> 根据你的计划，《xxx》还剩约 180 页。按你最近的速度，建议每天阅读约 25 分钟。\
> 可在：21:00–21:30 或 21:30–22:00 任选一段。你可以随时忽略或调整。

### 5.3 AI 规划接口（通过 OnePage Bridge）

设备端请求：

```json
{
  "request_id": "req_schedule_001",
  "type": "reading_schedule_advice",
  "plans": [
    {
      "id": "plan_20260928_001",
      "book_title": "xxx",
      "remaining_pages": 180,
      "target_date": "2026-10-31",
      "goal": { "type": "minutes_per_week", "value": 180 }
    }
  ],
  "free_slots": [
    { "days": [1,2,3,4,5], "start": "21:00", "end": "22:30" },
    { "days": [0,6], "start": "10:00", "end": "11:30" }
  ],
  "history_14d": [
    { "date": "2026-09-15", "total_sec": 1200 },
    ...
  ],
  "timezone": "Asia/Hong_Kong"
}
```

AI 返回：

```json
{
  "request_id": "req_schedule_001",
  "status": "ok",
  "advice": {
    "summary": "根据你的计划和空余时间，本周建议阅读 3 次，每次约 25–30 分钟。",
    "slots": [
      { "day": "周一", "start": "21:10", "end": "21:40", "duration_min": 30 },
      { "day": "周三", "start": "21:00", "end": "21:45", "duration_min": 45 },
      { "day": "周六", "start": "10:00", "end": "11:00", "duration_min": 60 }
    ],
    "note": "你可以随时调整或忽略这些建议。"
  },
  "cacheable": true,
  "expires_at": "2026-09-29T08:00:00+08:00"
}
```


### 5.4 温和提醒策略

- 禁止在阅读界面弹出全屏或模态提示。
- 允许两种形式：

1. 睡眠界面显示一行建议（例如：“今日建议：21:10–21:40 阅读约 30 分钟”）。
2. 下次唤醒时，在首页或陪读页显示轻提示，用户可一键“今日跳过”。
- 用户可随时在设置中关闭所有提醒。

______________________________________________________________________

## 6. 技术栈评估

### 6.1 设备端（Firmware）

| 层级 | 技术选型 | 说明 | 成熟度 |
| :-- | :-- | :-- | :-- |
| SoC / SDK | ESP32‑C61 + ESP‑IDF 5.5+ / 6.x | 单核 RISC‑V 160 MHz，320 KB SRAM，可选 PSRAM | 官方支持已成熟 [^3][^10] |
| 板级支持 | bsp_onepage_c61 | 统一 `board_*` API（显示、按键、SD、电池、麦克风、无线） | 项目自有，已可用 [^1] |
| 阅读引擎 | CrossPoint Reader（移植） | Activity 架构 + lib/Epub 流式解析 + SD 强缓存 | 高度成熟，社区活跃 [^6][^7] |
| 显示驱动 | moui + SSD1677 | 支持局部刷新（Partial Update） | 已验证 [^1] |
| 存储 | Micro SD（FAT32）+ `.crosspoint` 缓存目录 | 所有书库、进度、笔记、计划、历史均在 SD | 核心设计 |
| 网络 | ESP‑IDF Wi‑Fi + 自研 Web Server / WebDAV / OPDS | 按需开启，用完即关 | 已有基础 |
| 语音 | PDM 麦克风 + 软件 CIC 解码 → 16 kHz PCM | 本地采集，上传到 OnePage Bridge / OpenClaw 做 STT | 硬件已支持 [^1] |
| AI 客户端 | OnePage Bridge 轻量协议 | 设备不直接耦合 OpenClaw 内部结构 | 建议新增 |

**关键约束**：

- 可用 RAM 极为有限，必须继续沿用「流式解析 + 章节级 SD 缓存」。
- 电子墨水优先局部刷新，热力图等静态图可用全刷。
- 所有新增数据文件必须轻量、可清理。


### 6.2 AI 后端（OpenClaw）

| 组件 | 作用 | 与 OnePage 的契合点 |
| :-- | :-- | :-- |
| Gateway | 中央进程，消息路由 | 设备通过 OnePage Bridge 连接 |
| Brain | LLM 推理 | 负责计划建议、Tips 生成、总结 |
| Skills | Markdown 定义的可扩展能力 | 可新增 `reading-plan`、`schedule-advisor`、`history-summary` 等 Skill |
| Memory | 本地 Markdown 持久记忆 | 可记录用户阅读节奏偏好、常完成/常跳过的时段 |
| Heartbeat | 定时自主任务 | 用于可选的每日规划刷新或温和提醒触发（仅后端） |

**推荐架构**：设备端只做采集与展示，重规划逻辑全部在 OpenClaw 完成；设备通过 OnePage Bridge 访问。

### 6.3 技术栈总结评价

| 评价维度 | 评分 | 说明 |
| :-- | :-- | :-- |
| 成熟度 | ★★★★☆ | CrossPoint + ESP‑IDF + BSP 均有实际基础 [^1][^6] |
| 资源匹配度 | ★★★★☆ | 本地记录 + 后端规划的模式匹配低 RAM 约束 |
| 扩展性 | ★★★★★ | Skills 机制让陪读能力可社区化 |
| 风险点 | 中 | 提醒的「克制感」需要反复打磨；热力图在小屏上的信息密度需验证 |


______________________________________________________________________

## 7. 开发难度评估

| 功能模块 | 难度 | 主要工作量 | 风险点 | 预估人天（单人） |
| :-- | :-- | :-- | :-- | :-- |
| FR‑01 进度/缓存高可靠 | 中 | 缓存校验、低电量强制保存、恢复逻辑 | 边界条件多 | 5–8 |
| FR‑02 传书体验 | 低‑中 | Web 上传优化、断点续传、进度反馈 | 网络稳定性 | 4–6 |
| FR‑03 中文排版 | 中 | CJK 断行、字体切换性能、预设管理 | 渲染性能 | 6–10 |
| FR‑04 书签与笔记 | 低‑中 | 数据模型、UI、存储 | 与进度耦合 | 4–6 |
| RH‑01/02 会话与历史 | 中 | 状态机、日志、聚合、断电恢复 | 计时准确性 | 5–8 |
| RH‑03 热力图 | 中 | 方格布局、灰度分级、局部刷新 | 信息密度 | 4–6 |
| RP‑01/02 计划与空余时间 | 低‑中 | CRUD、UI、存储 | 用户体验 | 4–6 |
| RP‑03 本地规则建议 | 低 | 简单计算与文案 | 算法合理性 | 2–3 |
| AI‑01 上下文感知 | 中高 | 当前页文本提取、Bridge 协议、缓存 | 协议设计 | 8–12 |
| AI‑02 语音笔记 | 中 | 录音 → 上传 → 关联存储 | 音频链路稳定性 | 6–9 |
| AI‑08 智能陪读（AI 规划 + 提醒） | 中 | Bridge 协议、OpenClaw Skill、温和提醒通路 | 提醒打扰感 | 8–12 |
| AI‑03/04 生词与主动陪伴 | 中 | Skill 编写 + 设备端展示逻辑 | 打扰控制 | 5–8 |
| AI‑05~07 进阶 AI | 高 | 跨书检索、生图、外部 Skills 集成 | 复杂度与稳定性 | 15+ |


______________________________________________________________________

## 8. 开发项目建议

### 8.1 推荐项目结构

```text
crosspoint-onepage/
├── firmware/                 # 设备端固件（基于 CrossPoint 移植）
│   ├── components/
│   │   ├── board/            # bsp_onepage_c61
│   │   ├── reader/           # 阅读引擎扩展（书签、笔记等）
│   │   ├── ai_client/        # OnePage Bridge 轻量客户端
│   │   ├── reading_history/  # 会话追踪、历史聚合、热力图
│   │   ├── reading_plan/     # 计划、空余时间、本地建议
│   │   └── ...
│   └── main/
├── onepage-bridge/           # 可选：本地 Bridge 服务（PC/服务器）
│   └── ...
├── openclaw-skills/          # OnePage 专用 Skills（Markdown）
│   ├── reading-assistant/
│   ├── voice-note/
│   ├── vocab-book/
│   ├── reading-plan/         # 计划管理与建议规划
│   └── schedule-advisor/     # 时间建议生成
├── docs/                     # 协议、接口、交互说明
└── tools/                    # 传书工具、字体转换、调试脚本
```


### 8.2 推荐开发阶段与里程碑

**Phase 0：硬件与基础工程验证（1–2 周）**

- 固件成功运行在 OnePage 板；
- 确认 Flash、PSRAM、SD、RTC、麦克风；
- 测量电子纸全刷和局刷；
- 确认按键扫描和休眠唤醒；
- 导出启动日志和 `sdkconfig`；
- 建立最小 CI 编译流程。

**Phase 1：纯阅读稳定版（3–5 周）**

- 目标：FR‑01 ~ FR‑04 可用；
- 交付物：稳定阅读 + 可靠进度 + 中文体验明显提升 + 书签笔记；
- 验收标准：连续阅读 2 小时无崩溃、进度零丢失、中文排版主观满意度高。

**Phase 2：本地数据层（2–3 周）**

- 目标：RH‑01/02、RH‑03、RP‑01/02、RP‑03；
- 交付物：阅读会话追踪、每日聚合、热力图、计划 CRUD、本地建议；
- 验收标准：热力图正确反映真实阅读历史；计划可创建/编辑/归档。

**Phase 3：AI Bridge 和上下文问答（3–5 周）**

- 目标：AI‑01、AI‑02；
- 关键点：设备协议、配网和认证、请求超时、文本长度限制、Markdown 转设备富文本、结果缓存、断网降级；
- 验收标准：AI 请求超时后 \<3 秒显示可理解的离线提示；网络不可用时纯阅读不受影响。

**Phase 4：智能陪读与温和提醒（3–5 周）**

- 目标：AI‑08、AI‑03/04；
- 关键点：OpenClaw 规划 Skill、温和提醒通路、用户可忽略/调整；
- 验收标准：提醒在任何情况下都不强制打断阅读；建议被采纳或调整的比例健康；提醒相关投诉接近 0。

**Phase 5：打磨与扩展（持续）**

- AI‑05~07 及更高阶能力；
- 社区 Skills 生态建设；
- 性能与续航极致优化；
- 热力图与计划的更多维度分析（可选）。


### 8.3 关键技术决策建议

1. **坚持双分支策略**
    - `main`：纯阅读稳定版（可完全关闭 AI 与陪读）
    - `ai-experimental`：AI 与陪读功能集中开发和验证
2. **上下文协议 + 计划协议优先设计**\
在写功能前先定义清晰的数据契约（计划 JSON schema、历史记录格式、设备与 Bridge 的请求/响应格式）。
3. **提醒必须「可忽略、可关闭、非模态」**\
优先使用睡眠界面与下次唤醒提示，禁止阅读中全屏弹窗。
4. **热力图采用固定灰度等级**\
建议 4 级：无阅读 / 少（\<15 min）/ 中（15–40 min）/ 多（>40 min），电子墨水友好。
5. **OpenClaw Skills 全部用 Markdown 编写**\
降低贡献门槛，方便社区扩展陪读相关能力。
6. **强制资源预算**
    - 设备端新增功能 RAM 占用目标 \<40 KB
    - 历史数据按年可归档或清理
    - 任何网络请求必须可被用户一键禁用

### 8.4 团队与协作建议

- 建议最小团队：1 名固件（熟悉 ESP‑IDF + CrossPoint）+ 1 名 AI/后端（熟悉 OpenClaw Skills）
- 每周固定一次「上下文/计划协议 + 提醒克制性」专项同步
- 所有 AI 与陪读功能必须提供「完全关闭」开关，并通过测试验证关闭后零副作用

______________________________________________________________________

## 9. 成功指标（软件维度）

| 类别 | 指标 |
| :-- | :-- |
| 可靠性 | 进度丢失率 \<0.1%；连续阅读崩溃率接近 0；500 次异常复位测试中进度丢失 ≤1 次 |
| 阅读体验 | 中文用户主观排版满意度显著高于当前版本 |
| 本地数据 | 热力图可正确反映真实阅读历史；计划 CRUD 操作无数据丢失 |
| AI 价值 | Tips 开启用户中主动互动率 ≥30%；语音笔记周活可观 |
| 陪读价值 | 创建计划的用户中，≥40% 会查看热力图；建议被采纳或调整的比例健康；提醒相关投诉接近 0 |
| 克制性 | 未开启 AI/陪读的用户零相关投诉 |
| 性能 | 普通翻页首屏响应时间目标 \<1.5 秒；已缓存章节翻页 \<800 ms；AI 与陪读功能开启后平均续航下降 \<5%（在定义场景下测量） |


______________________________________________________________________

## 10. 风险与底线

1. 任何功能若导致明显卡顿、残影加剧或续航明显下降，必须降级或推迟。
2. AI 与陪读相关代码必须可被编译开关完全剔除。
3. 所有用户数据（笔记、生词、计划、历史）必须可导出、可删除。
4. 优先保证「纯阅读模式」的极致体验，AI 与陪读永远是可选项。
5. **提醒与规划必须保持建议性，禁止任何形式的强制打断或焦虑诱导。**
6. 设备端不直接耦合 OpenClaw 内部实现，仅通过 OnePage Bridge 协议交互。

______________________________________________________________________

**文档总结（v4.0）**\
本版在 v3.1 基础上完成：

- 硬件事实核实（ESP32‑C61、BSP、显示、按键、SD、麦克风、无线）；[^3][^4][^1]
- P0 收缩为「纯阅读 + 本地数据核心」；
- 阅读会话状态机、历史聚合、热力图、计划模型细化；
- AI 功能分层：本地规则建议 + OpenClaw 规划；
- 增加 OnePage Bridge 作为协议隔离层；
- 明确开发阶段、验收指标和风险底线。

文档可作为固件与 OpenClaw 后端开发的直接输入基线。

______________________________________________________________________

*End of Software Requirements Document v4.0*

<span style="display:none">[^11][^12][^13][^14][^15][^16][^17][^18][^19][^20][^21]</span>

<div align="center">⁂</div>

[^1]: https://components.espressif.com/components/movecall/bsp_onepage_c61/versions/0.1.4/readme

[^2]: https://github.com/MoveCall/bsp_onepage_c61

[^3]: https://documentation.espressif.com/esp32-c61_datasheet_en.pdf

[^4]: https://www.espressif.com/en/products/socs/esp32-c61

[^5]: https://documentation.espressif.com/esp32-c61-wroom-1_wroom-1u_datasheet_en.html

[^6]: https://github.com/MoveCall/crosspoint-onepage/tree/main/.github

[^7]: https://github.com/MoveCall/crosspoint-onepage/tree/main

[^8]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c61/api-guides/external-ram.html

[^9]: https://documentation.espressif.com/esp-hardware-design-guidelines/en/latest/esp32c61/index.html

[^10]: https://www.espressif.com/en/producttype/esp32-c61

[^11]: file-1.md

[^12]: https://www.espressif.com/en/support/download/documents/chips

[^13]: https://www.espressif.com/en/support/download/documents/modules

[^14]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c61/api-reference/index.html

[^15]: https://www.espressif.com/en/support/documents/technical-documents/esp32-wroom

[^16]: https://docs.espressif.com/projects/esp-idf/en/latest/esp32c61/migration-guides/index.html

[^17]: https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32c61/esp-hardware-design-guidelines-en-master-esp32c61.pdf

[^18]: https://www.espressif.com/en/support/documents/technical-documents

[^19]: https://www.espressif.com/en/products/modules

[^20]: https://docs.espressif.com/projects/esp-idf/en/stable/esp32c61/api-guides/memory-types.html

[^21]: https://github.com/MoveCall/crosspoint-onepage/blob/main/boards/onepage-c61.json

