# Reading_Companion 实时语音改造完整需求文档

> 面向 `zhou19830318/Reading_Companion` 的 ESP32-C61 电子墨水阅读设备。本文档用于直接交给 AI 编程助手执行，覆盖设备端、Voice Relay、实时 ASR、流式 LLM、兼容回退和服务器部署。
>
> 目标：**极致交流响应速度、个人使用性价比、稳定可回退、避免把云厂商协议耦合进设备固件**。

## 1. 当前问题与改造目标

当前链路是“录音结束 → 整段上传 → 等待 STT → 再调用 OpenClaw → 等完整结果 → 刷屏”。主要问题：

- 音频不是实时上传，录音结束前云端没有任何识别工作；
- 完整 STT 结果出来后才开始模型回复；
- STT 和 OpenClaw 分别建立连接，TLS/握手和状态管理重复；
- 设备端承担云服务鉴权、协议解析、会话和错误恢复，复杂且容易阻塞 UI；
- 电子墨水屏不适合按 token 高频刷新，流式结果如果没有节流会降低体验；
- OpenClaw 可能引入工具调用和额外等待，不适合每个简单问答都走完整 Agent 链路。

目标：

1. 设备采集 PCM 后立即以小帧发送；
2. Relay 同时处理实时 ASR 和 LLM；
3. 先显示 ASR partial/final，再尽快显示 LLM 首段文本；
4. 普通问题走轻量直接 LLM，复杂工具任务才走 OpenClaw；
5. 设备只保存 Relay token，不保存云 API Key；
6. Legacy MiMo + OpenClaw 路径保留为回退模式；
7. 记录每个阶段延迟，不能用主观感觉优化。

## 2. 推荐总体架构

```text
ESP32-C61
  │ 16 kHz mono PCM16LE, 40 ms frames
  │ WSS /api/v1/voice/stream
  ▼
Voice Relay（<server-ip>）
  ├── Device Protocol / Auth / Session
  ├── VAD / audio aggregation
  ├── Streaming ASR Adapter
  │     └── Paraformer / Fun-ASR / Qwen-ASR
  ├── Fast LLM Adapter
  │     └── Qwen Flash OpenAI-compatible streaming
  ├── Optional OpenClaw Adapter
  └── metrics / logs / cancellation / fallback

ESP32 ← asr.partial / asr.final / llm.delta / llm.final
```

关键原则：

- ESP32 只连接一个 Relay；
- Relay 对外统一设备协议，对内适配不同供应商；
- 设备协议的 40 ms PCM 帧不等同于云厂商的音频块，ProviderAdapter 负责聚合和转换；
- 普通阅读问答不强制进入 OpenClaw；
- 任何上游连接都必须可取消、可超时、可有限重试。

## 3. 响应速度目标

以“用户停止说话”为起点，首屏可见文本目标：

| 指标 | 目标 | 说明 |
|---|---:|---|
| Relay 连接复用 | 已连接 | 不在每轮语音重新建立多条云连接 |
| ASR first partial | 150～400 ms | 取决于网络和云端 ASR |
| ASR final | 300～900 ms | 用户结束上传后 |
| LLM first token | 500～1200 ms | 从收到最终文本开始计算 |
| 首段可见文本 | 700～1500 ms | 不等待完整回答 |
| 完整短回复 | 1～3 s | 目标，不作硬 SLA |
| 单轮最长语音 | 30 s | 超限主动结束 |

所有目标必须以真机日志验证，不应把供应商宣传延迟当作承诺。

## 4. 设备端音频与协议要求

### 4.1 音频格式

第一版统一：

```text
采样率：16000 Hz
声道：单声道
采样格式：PCM signed 16-bit little-endian
设备业务帧：40 ms
每帧字节数：16000 × 0.04 × 2 = 1280 bytes
```

保留 200～500 ms pre-roll，避免首字被 VAD 截断。设备端不要生成 WAV 和 Base64；实时路径只发送二进制 PCM。

### 4.2 设备事件

```json
{"type":"session.start","requestId":"uuid","deviceId":"reading-companion-001","audio":{"sampleRate":16000,"channels":1,"format":"pcm_s16le","frameMs":40},"mode":"relay"}
{"type":"audio.start","requestId":"uuid","sessionId":"uuid"}
```

随后发送二进制音频帧；结束时发送：

```json
{"type":"audio.end","requestId":"uuid","sessionId":"uuid","reason":"vad_silence"}
```

取消时发送：

```json
{"type":"session.cancel","requestId":"uuid","sessionId":"uuid","reason":"user_cancel"}
```

### 4.3 服务端事件

```json
{"type":"session.started","requestId":"uuid","sessionId":"uuid"}
{"type":"asr.partial","requestId":"uuid","sessionId":"uuid","text":"今天天气","seq":12}
{"type":"asr.final","requestId":"uuid","sessionId":"uuid","text":"今天天气怎么样","seq":13}
{"type":"llm.start","requestId":"uuid","sessionId":"uuid"}
{"type":"llm.delta","requestId":"uuid","sessionId":"uuid","text":"今天","seq":1}
{"type":"llm.final","requestId":"uuid","sessionId":"uuid","text":"今天适合阅读。"}
{"type":"session.done","requestId":"uuid","sessionId":"uuid"}
```

统一错误事件必须包含 `code`、用户可见 `message`、`retryable` 和 `requestId`，不透传 API Key 或上游敏感信息。

## 5. 设备端状态机

```text
Idle
  → Connecting
  → Ready
  → Capturing
  → FinalizingAudio
  → WaitingAsrFinal
  → ReceivingLlm
  → Rendering
  → Idle
```

任意阶段可进入：

```text
Cancelled / Timeout / Error / LegacyFallback
```

要求：

- callback 只复制数据或入队，不能阻塞、不能直接刷新屏幕；
- 录音、网络、显示分离为独立任务/队列；
- 所有 session 有 requestId/sessionId；
- Activity 退出后迟到事件不得访问已释放对象；
- watchdog 能恢复 Wi-Fi/WebSocket，但不能造成无限重连；
- 单轮失败后清空音频队列和 parser，再决定是否回退；
- Relay 失败时提供 Legacy OpenClaw 回退，不在设备上保存第三方云 Key。

## 6. VAD、端点检测和交互体验

第一版使用本地轻量 RMS/VAD，不在 ESP32 上部署大型 turn model：

- `VOICED_MIN_MS` 防止敲击和瞬时噪声触发；
- 静音结束阈值初始 500～900 ms，根据真实录音调整；
- 短词“好”“对”“嗯”不能被过短阈值误丢；
- partial ASR 只显示为临时状态，不触发 LLM；
- 明确的按键/确认动作可作为 manual turn control；
- 未来连续对话再增加 interruption 和 TTS barge-in。

电子墨水屏：

- partial 文本合并后每 300～800 ms 刷新一次；
- 首次收到 LLM delta 立即显示“回答中”；
- 不逐 token 全屏刷新；
- final 到达后做一次完整排版刷新；
- 所有刷新调用放显示任务，不能放 WebSocket callback。

## 7. Relay 内部模块要求

```text
relay/
  main.py
  protocol.py
  session.py
  auth.py
  metrics.py
  providers/
    asr_base.py
    asr_dashscope.py
    llm_base.py
    llm_dashscope.py
    openclaw.py
  tests/
```

每个 provider adapter 必须声明：endpoint、鉴权方式、地域、模型、输入格式、chunk 时长、事件映射、结束事件、heartbeat、限流和重连策略。设备端不得直接拼供应商 JSON。

普通问题路由：

```text
ASR final
  ├── 简单阅读/问答 → Qwen Flash streaming
  ├── 需要工具/书库操作 → OpenClaw adapter
  └── provider 失败 → 有限重试或 LegacyFallback
```

上下文策略：

- 系统提示词短且稳定；
- 仅保留必要对话历史；
- 阅读上下文按需注入，不把整本书重复发送；
- 普通回答限制 128～256 tokens；
- 关闭不需要的深度思考模式；
- 不等待完整模型结果才展示首段文本。

## 8. 错误、超时和重试

默认建议：

```text
设备连接：5 s
Relay session idle：60 s
单轮音频：30 s
ASR 建连：5 s
ASR 等待 task-started：3 s
LLM 首 token：5 s
上游总超时：30 s
重试：认证错误不重试；网络错误最多一次；429 有限退避一次
```

401/403 必须立即报告配置错误；429 不能无限重试；设备断开必须取消上游 ASR/LLM；服务端重启不应留下持续消耗配额的后台任务。

## 9. 安全和配置

- ESP32 仅保存 Relay token；
- Relay 云 API Key 通过 root-only EnvironmentFile 或 Docker secret 注入；
- 日志脱敏，不记录 Authorization、完整音频和必要范围之外的个人内容；
- 生产使用 WSS，设备验证 CA；
- Relay 只监听本机应用端口，公网仅暴露 Nginx 443；
- 管理、调试、metrics 接口默认不向公网开放；
- token 支持轮换和按设备撤销；
- 配置按 `region + workspaceId + endpoint + apiKey` 成组校验。

## 10. 实现阶段

### Phase 0：基线和观测

- 给当前 Legacy 链路增加 t0～tN 延迟日志；
- 记录录音、上传、STT 首结果、STT final、OpenClaw 首输出、屏幕首刷和完成时间；
- 保存当前行为作为回归基线；
- 确认 C61 RAM、PSRAM、Wi-Fi 和 TLS 余量。

### Phase 1：设备内部实时协议

- 新增 VoiceRelayClient；
- 新增二进制 PCM frame sender；
- 新增事件 parser 和有界队列；
- 新增状态机和错误码；
- 暂不接入真实云 ASR，可用本地 echo/mock Relay 测试。

### Phase 2：Provider Contract Gate

用真实测试音频验证 ASR endpoint、地域、鉴权、`run-task`、`task-started`、音频格式、`result-generated`、`finish-task`、`task-finished`，并验证 Qwen SSE delta、`[DONE]`、usage-only chunk 和取消。

### Phase 3：Voice Relay

- 完成 WSS 设备接口；
- 完成 ASR adapter；
- 完成 Qwen streaming adapter；
- 完成 session cancellation、timeout、metrics、日志；
- 完成服务器部署和独立测试客户端。

### Phase 4：真机端到端

- 先接 Relay；
- 再接 partial/final 显示；
- 再接 LLM delta 合并刷新；
- 连续 20～50 轮测试；
- 检查内存、掉线、重启和回退。

### Phase 5：优化和扩展

- 通过真实 P50/P95 调整 chunk 和 endpointing；
- 增加热词、阅读上下文和简单路由；
- 需要时接入 OpenClaw；
- 只有出现多实例、持久会话或本地推理需求时才引入 Redis/GPU 等复杂组件。

## 11. 验收标准

### 功能

- [ ] 设备可以通过 WSS 建立 session；
- [ ] PCM 二进制帧按序到达 Relay；
- [ ] 能收到 partial/final ASR；
- [ ] LLM delta 能流式显示；
- [ ] 完成事件和错误事件完整；
- [ ] Cancel 能取消云端任务；
- [ ] Legacy 路径仍可使用。

### 性能

- [ ] 录音过程中已开始 ASR，而不是录音结束后才上传；
- [ ] 首次 ASR partial 低于目标；
- [ ] LLM 首 token 低于目标；
- [ ] 电子墨水屏不逐 token 刷新；
- [ ] 连续 20 轮无单调内存泄漏；
- [ ] 网络波动下不无限重连。

### 测试

- [ ] 安静环境、噪声环境、短词、长句；
- [ ] ASR/LLM 401、403、429；
- [ ] TLS 证书失败、DNS 失败、上游断线；
- [ ] WebSocket 分片和背压；
- [ ] Relay 重启、服务器重启、设备断电；
- [ ] finish-task 后 final result 仍能到达；
- [ ] PCM/WAV 参数和真实字节一致；
- [ ] 所有日志不泄露密钥。

## 12. 交付给 AI 编程助手时的执行要求

请按以下顺序修改，不要一次性重写整个项目：

1. 先阅读现有 Activity、网络、麦克风、显示和 NVS 配置；
2. 先提交协议类型、状态机和 mock 测试；
3. 再实现 VoiceRelayClient，保留 Legacy client；
4. 所有新增配置增加默认值和向后兼容迁移；
5. 所有网络事件通过队列进入 Activity，不得直接跨线程改 UI；
6. 使用固定 buffer 和明确内存上限；
7. 每完成一个阶段运行编译、单元测试和真机日志检查；
8. 只有通过 Provider Contract Gate 后，才连接真实 ASR/LLM；
9. 不在源码、网页配置和日志中硬编码云 API Key；
10. 输出变更文件清单、协议示例、构建命令、测试结果和回滚方法。


---

## 17. Voice Relay 服务器部署需求

### 17.1 已确定的部署环境

Voice Relay 部署在用户现有服务器：

```text
公网 IPv4：<server-ip>
服务名称：Reading Companion Voice Relay
建议服务端口：8443（应用内部监听 0.0.0.0:8000）
传输协议：设备到 Relay 使用 WSS
服务形态：长期运行的单进程/容器化 WebSocket 服务
```

建议不要让 ESP32 直接连接云厂商 ASR 和 LLM，而是采用：

```text
ESP32
  │ WSS
  ▼
<server-ip>:443
  │ HTTPS / WebSocket reverse proxy
  ▼
Voice Relay:127.0.0.1:8000
  ├── 阿里云实时 ASR WebSocket
  └── 阿里云百炼 Qwen Streaming Chat API
```

公网只暴露 443；Relay 应用本身只监听 `127.0.0.1:8000`，禁止直接暴露到公网。

### 17.2 服务器最低要求

服务器最低配置建议：

| 项目 | 最低要求 | 推荐要求 |
|---|---:|---:|
| CPU | 2 vCPU | 2～4 vCPU |
| 内存 | 2 GB | 4 GB |
| 磁盘 | 20 GB SSD | 40 GB SSD |
| 系统 | Ubuntu 22.04/24.04 LTS 或 Debian 12 | Ubuntu 24.04 LTS |
| 网络 | 能访问阿里云百炼 WSS/HTTPS endpoint | 中国大陆地域网络稳定、低丢包 |
| 公网入口 | TCP 443 | TCP 80 + 443，用于证书签发和续期 |
| 时间同步 | 必须启用 NTP | `systemd-timesyncd` 或 `chrony` |

Voice Relay 不运行 ASR 或 LLM 本地推理，因此通常不需要 GPU。服务器 CPU 主要用于：

- WebSocket 连接与协议解析；
- 音频小块聚合和转发；
- JSON/SSE 解析；
- 日志与指标；
- TLS reverse proxy。

若未来切换到本地 FunASR 或本地 LLM，应另行评估 GPU、内存和磁盘模型容量，不得把本需求中的配置直接当作本地推理配置。

### 17.3 必须安装的软件和依赖

#### 系统软件

```bash
sudo apt update
sudo apt install -y \
  ca-certificates \
  curl \
  git \
  nginx \
  certbot \
  python3 \
  python3-venv \
  python3-pip \
  build-essential \
  openssl \
  jq \
  htop \
  lsof \
  chrony
```

如果采用 Docker 部署，额外安装：

```bash
sudo apt install -y docker.io docker-compose-plugin
sudo systemctl enable --now docker
```

不要求在生产服务器安装 Node.js，除非 Relay 实现明确选择 Node.js。第一版推荐 Python，降低 WebSocket 和异步音频转发的开发门槛。

#### Python 运行时

建议使用 Python 3.11 或更高版本，但必须先确认服务器操作系统提供的版本：

```bash
python3 --version
python3 -m venv --help
```

建议的 `requirements.txt`：

```txt
fastapi>=0.115,<1.0
uvicorn[standard]>=0.30,<1.0
websockets>=13,<16
httpx>=0.27,<1.0
pydantic>=2.8,<3.0
pydantic-settings>=2.4,<3.0
python-json-logger>=2.0,<4.0
prometheus-client>=0.20,<1.0
orjson>=3.10,<4.0
```

依赖用途：

| 依赖 | 用途 |
|---|---|
| FastAPI | HTTP 健康检查、管理 API 和 WebSocket endpoint |
| Uvicorn | ASGI 服务器 |
| websockets | Relay 到阿里云 ASR 的原始 WebSocket 客户端 |
| httpx | Qwen Chat 流式 HTTPS/SSE 请求 |
| Pydantic | 校验设备事件、配置和 provider 返回数据 |
| python-json-logger | 结构化 JSON 日志 |
| prometheus-client | 可选指标导出 |
| orjson | 低开销 JSON 编解码 |

第一版不要引入大型 Agent 框架。Pipecat、LiveKit 和 LangChain 不属于基础 Relay 的必需依赖，会增加部署体积、调试面和升级风险。

### 17.4 服务器目录和用户权限

创建专用系统用户，禁止使用 root 运行 Relay：

```bash
sudo useradd --system --home /opt/reading-companion-relay \
  --shell /usr/sbin/nologin voice-relay
sudo mkdir -p /opt/reading-companion-relay/{app,logs,run}
sudo chown -R voice-relay:voice-relay /opt/reading-companion-relay
sudo chmod 750 /opt/reading-companion-relay
```

建议目录：

```text
/opt/reading-companion-relay/
├── app/                    # 代码、venv、requirements.txt
├── config/                 # 非敏感配置模板，不保存真实 API Key
├── logs/                   # 仅在不使用 journald 时使用
├── run/                    # pid 或临时文件
└── deploy/                 # systemd、nginx、备份脚本
```

API Key 不允许提交到 Git，不允许写入仓库，不允许通过设备下发。推荐使用 root-only 的 EnvironmentFile：

```text
/etc/reading-companion/voice-relay.env
```

权限要求：

```bash
sudo chown root:voice-relay /etc/reading-companion/voice-relay.env
sudo chmod 640 /etc/reading-companion/voice-relay.env
```

文件内容结构：

```dotenv
RELAY_ENV=production
RELAY_BIND=127.0.0.1
RELAY_PORT=8000
RELAY_LOG_LEVEL=INFO
RELAY_DEVICE_TOKEN=<random-long-token>
DASHSCOPE_API_KEY=<regional-api-key>
DASHSCOPE_WORKSPACE_ID=<workspace-id>
DASHSCOPE_REGION=cn-beijing
ASR_PROVIDER=paraformer
ASR_MODEL=paraformer-realtime-v2
LLM_PROVIDER=dashscope-openai-compatible
LLM_MODEL=qwen3.7-flash
LLM_BASE_URL=https://<workspace-id>.cn-beijing.maas.aliyuncs.com/compatible-mode/v1
ALLOWED_DEVICE_IDS=<device-id-list>
MAX_AUDIO_SECONDS=30
MAX_LLM_OUTPUT_TOKENS=256
```

真实部署时不要把上述占位符原样使用。API Key 只保存在服务器环境变量中，ESP32 只保存 Relay 的设备认证 token，且该 token 只允许访问当前设备的语音接口。

### 17.5 Relay 必须提供的 API

所有接口统一增加：

```text
requestId：客户端生成或服务端生成的 UUID
设备认证：Authorization: Bearer <relay-device-token>
设备标识：X-Device-Id: <device-id>
协议版本：X-Voice-Protocol: 1
```

禁止把阿里云 API Key 返回给设备，禁止将上游错误原文完整透传给设备。

#### 17.5.1 健康检查

```http
GET /healthz
```

用途：进程存活检查。该接口不检查阿里云服务，不应因为上游短暂故障而导致容器被无限重启。

成功响应：

```json
{
  "status": "ok",
  "service": "voice-relay",
  "version": "0.1.0"
}
```

```http
GET /readyz
```

用途：部署和监控检查。检查配置是否完整、DNS 是否可用、上游 endpoint 是否配置，但不建议每次请求都建立真实 ASR 任务。

响应示例：

```json
{
  "status": "ready",
  "asr": "configured",
  "llm": "configured",
  "region": "cn-beijing"
}
```

#### 17.5.2 设备能力和配置检查

```http
GET /api/v1/device/config
```

返回当前设备可用能力，但不返回任何密钥：

```json
{
  "protocolVersion": 1,
  "audio": {
    "sampleRate": 16000,
    "channels": 1,
    "format": "pcm_s16le",
    "frameMs": 40
  },
  "limits": {
    "maxAudioSeconds": 30,
    "maxReplyChars": 500
  },
  "features": {
    "partialAsr": true,
    "streamingLlm": true,
    "legacyFallback": true
  }
}
```

#### 17.5.3 实时语音 WebSocket

```text
WSS /api/v1/voice/stream
```

这是设备使用的主接口。建议使用单个 WebSocket 承载控制事件、PCM 二进制帧、ASR partial/final 和 LLM partial/final。

连接建立后，设备发送：

```json
{
  "type": "session.start",
  "requestId": "uuid",
  "deviceId": "reading-companion-001",
  "audio": {
    "sampleRate": 16000,
    "channels": 1,
    "format": "pcm_s16le",
    "frameMs": 40
  },
  "mode": "relay"
}
```

服务器响应：

```json
{
  "type": "session.started",
  "requestId": "uuid",
  "sessionId": "uuid",
  "serverTime": "2026-10-16T03:00:00Z"
}
```

设备开始采集后，发送：

```json
{
  "type": "audio.start",
  "requestId": "uuid",
  "sessionId": "uuid"
}
```

随后发送二进制 PCM 帧。每个二进制 WebSocket message 对应一帧或一组完整的 PCM 字节，第一版建议一帧一个 message：

```text
16000 Hz × 40 ms × 2 bytes × 1 channel = 1280 bytes
```

设备停止采集后发送：

```json
{
  "type": "audio.end",
  "requestId": "uuid",
  "sessionId": "uuid",
  "reason": "vad_silence"
}
```

取消时发送：

```json
{
  "type": "session.cancel",
  "requestId": "uuid",
  "sessionId": "uuid",
  "reason": "user_cancel"
}
```

Relay 事件：

```json
{
  "type": "asr.partial",
  "requestId": "uuid",
  "sessionId": "uuid",
  "text": "今天天气",
  "seq": 12
}
```

```json
{
  "type": "asr.final",
  "requestId": "uuid",
  "sessionId": "uuid",
  "text": "今天天气怎么样",
  "seq": 13
}
```

```json
{
  "type": "llm.start",
  "requestId": "uuid",
  "sessionId": "uuid"
}
```

```json
{
  "type": "llm.delta",
  "requestId": "uuid",
  "sessionId": "uuid",
  "text": "今天",
  "seq": 1
}
```

```json
{
  "type": "llm.final",
  "requestId": "uuid",
  "sessionId": "uuid",
  "text": "今天适合阅读，室外天气晴朗。",
  "usage": {
    "inputTokens": 120,
    "outputTokens": 18
  }
}
```

```json
{
  "type": "session.done",
  "requestId": "uuid",
  "sessionId": "uuid",
  "latencyMs": {
    "connect": 85,
    "asrFirstPartial": 180,
    "asrFinal": 420,
    "llmFirstToken": 650,
    "llmFinal": 1100
  }
}
```

统一错误格式：

```json
{
  "type": "error",
  "requestId": "uuid",
  "sessionId": "uuid",
  "code": "UPSTREAM_AUTH_ERROR",
  "message": "语音服务认证失败",
  "retryable": false,
  "seq": 14
}
```

设备必须至少处理以下错误码：

| 错误码 | 含义 | 是否自动重试 |
|---|---|---|
| `BAD_PROTOCOL` | 设备事件或协议版本错误 | 否 |
| `UNAUTHORIZED` | Relay token 错误 | 否 |
| `DEVICE_NOT_ALLOWED` | 设备未注册 | 否 |
| `AUDIO_FORMAT_UNSUPPORTED` | 采样率、声道或格式错误 | 否 |
| `AUDIO_TIMEOUT` | 音频接收超时 | 当前请求否 |
| `ASR_CONNECTION_ERROR` | ASR 上游连接失败 | 最多一次 |
| `ASR_AUTH_ERROR` | ASR Key/地域/Workspace 错误 | 否 |
| `ASR_RATE_LIMITED` | ASR 限流 | 有退避时一次 |
| `LLM_CONNECTION_ERROR` | LLM 上游连接失败 | 最多一次 |
| `LLM_AUTH_ERROR` | LLM Key/地域/Endpoint 错误 | 否 |
| `LLM_RATE_LIMITED` | LLM 限流 | 有退避时一次 |
| `SESSION_TIMEOUT` | 单次会话超过时间限制 | 否 |
| `SERVER_OVERLOADED` | Relay 并发或资源超限 | 稍后一次 |
| `INTERNAL_ERROR` | 未分类内部错误 | 视 requestId 防重情况 |

#### 17.5.4 文本调试接口

为便于不依赖 ESP32 调试 Relay，提供：

```http
POST /api/v1/chat
Content-Type: application/json
```

请求：

```json
{
  "requestId": "uuid",
  "deviceId": "debug-client",
  "text": "请总结当前阅读内容",
  "stream": true,
  "maxTokens": 256
}
```

`stream=true` 时返回 SSE；`stream=false` 时返回完整 JSON。该接口必须默认关闭公网访问，或者只允许服务器内网/管理员 token 访问，避免被扫描后消耗 LLM 配额。

#### 17.5.5 管理指标接口

```http
GET /metrics
```

如果启用 Prometheus，至少记录：

```text
voice_sessions_total{result}
voice_sessions_active
audio_frames_received_total
audio_frames_dropped_total
asr_requests_total{result}
llm_requests_total{result}
voice_latency_ms{stage}
upstream_reconnects_total{provider}
websocket_disconnects_total{reason}
relay_heap_or_process_rss_bytes
```

`/metrics` 不得包含 API Key、Authorization header、完整音频、完整用户问题或完整 LLM 回复。

### 17.6 Relay 会话状态机

服务器必须采用有限状态机，禁止用多个无约束后台线程拼接请求：

```text
CONNECTED
  → SESSION_STARTED
  → RECEIVING_AUDIO
  → ASR_RUNNING
  → FINISHING_ASR
  → LLM_RUNNING
  → DONE
```

任意阶段都可以进入：

```text
CANCELLED / FAILED / TIMEOUT
```

关键约束：

- 单个 WebSocket 连接默认只允许一个活动 session；
- `requestId` 重复时必须拒绝或幂等处理；
- `audio.end` 只能处理一次；
- `session.cancel` 必须取消上游 ASR 和 LLM；
- 设备断开时，服务端必须在有限时间内取消所有上游任务；
- 单次音频最大 30 秒，单次 LLM 最大输出 256 tokens；
- 空闲 WebSocket 超时建议 60 秒；
- 服务器关闭时先停止接收新 session，再等待已有 session 有限时间退出。

### 17.7 Nginx 反向代理配置要求

建议域名方案：

```text
voice.example.com → <server-ip>
```

如果暂时没有域名，不建议直接让 ESP32 连接 `wss://<server-ip>`，因为证书验证和后续换证不便。应先准备一个解析到该 IP 的域名。

Nginx 配置示例：

```nginx
map $http_upgrade $connection_upgrade {
    default upgrade;
    ''      close;
}

server {
    listen 80;
    listen [::]:80;
    server_name voice.example.com;

    location /.well-known/acme-challenge/ {
        root /var/www/certbot;
    }

    location / {
        return 301 https://$host$request_uri;
    }
}

server {
    listen 443 ssl http2;
    listen [::]:443 ssl http2;
    server_name voice.example.com;

    ssl_certificate     /etc/letsencrypt/live/voice.example.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/voice.example.com/privkey.pem;

    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_session_timeout 1d;
    ssl_session_cache shared:SSL:10m;
    ssl_prefer_server_ciphers off;

    client_max_body_size 2m;
    proxy_read_timeout 90s;
    proxy_send_timeout 90s;
    proxy_buffering off;

    location /api/v1/voice/stream {
        proxy_pass http://127.0.0.1:8000;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection $connection_upgrade;
    }

    location /healthz {
        proxy_pass http://127.0.0.1:8000;
        proxy_set_header Host $host;
    }

    location /readyz {
        proxy_pass http://127.0.0.1:8000;
        proxy_set_header Host $host;
    }
}
```

注意：

- WebSocket location 必须设置 `proxy_http_version 1.1`；
- 必须透传 `Upgrade` 和 `Connection`；
- 流式 LLM 接口必须关闭 Nginx buffering；
- `proxy_read_timeout` 必须大于一次语音会话的最大预期时间；
- 不要用全局 `proxy_buffering on` 造成 delta 聚集后才一次性返回；
- 只把 `/api/v1/voice/stream` 公开给设备，调试和管理接口必须加访问控制。

### 17.8 TLS 证书部署

推荐使用真实域名和 Let's Encrypt：

```bash
sudo mkdir -p /var/www/certbot
sudo certbot certonly --webroot \
  -w /var/www/certbot \
  -d voice.example.com \
  --email <your-email> \
  --agree-tos \
  --no-eff-email
sudo nginx -t
sudo systemctl reload nginx
```

续期检查：

```bash
sudo certbot renew --dry-run
```

验收：

```bash
openssl s_client -connect voice.example.com:443 \
  -servername voice.example.com </dev/null 2>/dev/null \
  | openssl x509 -noout -subject -issuer -dates
```

不允许：

- 设备端关闭证书校验；
- 使用过期或自签名生产证书；
- 把私钥提交到 Git；
- 只配置证书而不配置完整证书链；
- 让 Relay 应用直接在 443 上以 root 运行。

如果当前只有 IP 没有域名，开发阶段可以使用 `ws://<server-ip>:<port>` 或临时测试域名，但该方案不能作为最终生产部署方案。生产验收必须切换到 `wss://voice.example.com/api/v1/voice/stream`。

### 17.9 防火墙和安全组

服务器云安全组和系统防火墙至少开放：

```text
TCP 22   SSH，仅允许管理来源 IP，若条件允许不要对全网开放
TCP 80   HTTP，证书签发和跳转 HTTPS
TCP 443  HTTPS/WSS，设备正式访问
```

Relay 应用端口 `8000` 不得对公网开放：

```bash
sudo ufw default deny incoming
sudo ufw default allow outgoing
sudo ufw allow 80/tcp
sudo ufw allow 443/tcp
sudo ufw allow from <admin-ip> to any port 22 proto tcp
sudo ufw enable
sudo ufw status verbose
```

如果使用云服务器安全组，必须同时检查：

1. 云安全组；
2. UFW/iptables；
3. Nginx listen 地址；
4. 云厂商网络 ACL；
5. 服务器 DNS 和默认路由。

### 17.10 systemd 部署方式

推荐生产环境第一版使用 systemd，便于查看日志和控制资源。

安装应用：

```bash
cd /opt/reading-companion-relay/app
sudo -u voice-relay python3 -m venv .venv
sudo -u voice-relay .venv/bin/pip install --upgrade pip
sudo -u voice-relay .venv/bin/pip install -r requirements.txt
```

创建 `/etc/systemd/system/reading-companion-relay.service`：

```ini
[Unit]
Description=Reading Companion Voice Relay
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=voice-relay
Group=voice-relay
WorkingDirectory=/opt/reading-companion-relay/app
EnvironmentFile=/etc/reading-companion/voice-relay.env
ExecStart=/opt/reading-companion-relay/app/.venv/bin/uvicorn \
  relay.main:app \
  --host 127.0.0.1 \
  --port 8000 \
  --workers 1
Restart=on-failure
RestartSec=3
TimeoutStopSec=20
KillSignal=SIGTERM

# 基础资源保护
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/reading-companion-relay/logs /opt/reading-companion-relay/run

# 限制泄漏和文件句柄
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

启用和启动：

```bash
sudo systemctl daemon-reload
sudo systemctl enable reading-companion-relay
sudo systemctl start reading-companion-relay
sudo systemctl status reading-companion-relay --no-pager
```

日志：

```bash
journalctl -u reading-companion-relay -f
journalctl -u reading-companion-relay --since '1 hour ago'
```

第一版使用 `--workers 1`。WebSocket session 状态存储在进程内，多 worker 会导致同一个设备连接到不同进程、指标分散和取消逻辑复杂。除非后续设计 Redis/session broker，否则不要直接增加 worker 数量。

### 17.11 Docker 部署方式（可选）

如果服务器已经采用 Docker 运维，可以使用以下方式：

`Dockerfile`：

```dockerfile
FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

RUN useradd --system --create-home --uid 10001 voice-relay
WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY relay ./relay
RUN chown -R voice-relay:voice-relay /app
USER voice-relay

EXPOSE 8000
CMD ["uvicorn", "relay.main:app", "--host", "0.0.0.0", "--port", "8000", "--workers", "1"]
```

`docker-compose.yml`：

```yaml
services:
  voice-relay:
    build: .
    container_name: reading-companion-voice-relay
    restart: unless-stopped
    env_file:
      - /etc/reading-companion/voice-relay.env
    expose:
      - "8000"
    networks:
      - relay-net
    healthcheck:
      test: ["CMD", "python", "-c", "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8000/healthz')"]
      interval: 30s
      timeout: 5s
      retries: 3

networks:
  relay-net:
    driver: bridge
```

如果 Nginx 在宿主机运行，需要将宿主机端口映射到 `127.0.0.1`，例如：

```yaml
ports:
  - "127.0.0.1:8000:8000"
```

不要使用：

```yaml
ports:
  - "8000:8000"
```

否则 Relay 会绕过 Nginx 直接暴露公网。

### 17.12 首次部署顺序

严格按以下顺序执行：

```text
1. 确认服务器系统、CPU、内存、磁盘和网络
2. 配置云安全组和 UFW
3. 配置域名 A 记录：voice.example.com → <server-ip>
4. 安装 Nginx 和 Certbot
5. 创建 voice-relay 系统用户和目录
6. 上传 Relay 代码
7. 安装 Python venv 和固定版本依赖
8. 创建 root-only environment 文件
9. 执行配置静态校验，不发起真实模型请求
10. 启动 Relay，验证 /healthz 和 /readyz
11. 配置 TLS 证书
12. 配置 Nginx WebSocket reverse proxy
13. 使用 wscat/Python 最小客户端验证 WSS
14. 使用测试音频验证 ASR provider contract
15. 使用短文本验证 LLM streaming
16. 用 ESP32 接入，验证设备 token 和状态机
17. 连续执行 20 轮语音压力/内存测试
18. 配置 systemd 自启动和证书自动续期
19. 完成断线、重启、上游限流和错误恢复验收
```

### 17.13 不依赖 ESP32 的服务器端验证命令

健康检查：

```bash
curl -fsS https://voice.example.com/healthz | jq
curl -fsS https://voice.example.com/readyz | jq
```

WebSocket 验证：

```bash
python3 -m pip install websockets
```

最小测试客户端必须支持：

- WSS TLS 校验；
- Bearer token；
- 发送 `session.start`；
- 发送 16 kHz 单声道 PCM；
- 发送 `audio.end`；
- 打印 `asr.partial`、`asr.final`、`llm.delta`、`session.done`；
- 在 5 秒内无服务端事件时输出 timeout；
- 退出前发送 `session.cancel` 或正常关闭 WebSocket。

测试时不要在命令行参数中直接传 API Key，避免进入 shell history。服务器日志中也不能打印 Authorization header。

### 17.14 服务器端延迟指标

Relay 必须为每个 requestId 记录以下时间戳：

```text
t0 = WebSocket session.start 接收
t1 = audio.start 接收
t2 = 第一帧音频接收
t3 = audio.end 接收
t4 = ASR first partial
t5 = ASR final
t6 = LLM 请求发出
t7 = LLM first token
t8 = LLM final
t9 = session.done 发出
```

核心指标：

```text
设备连接延迟       = t1 - t0
ASR 首字延迟        = t4 - t2
ASR 最终延迟        = t5 - t3
LLM 首 token 延迟   = t7 - t6
Relay 总响应延迟    = t8 - t3
设备可见响应延迟    = t9 - t3
```

日志示例：

```json
{
  "event": "voice_session_finished",
  "requestId": "uuid",
  "deviceId": "reading-companion-001",
  "result": "success",
  "asrFirstPartialMs": 180,
  "asrFinalMs": 420,
  "llmFirstTokenMs": 650,
  "llmFinalMs": 1100,
  "audioFrames": 84,
  "droppedFrames": 0
}
```

日志必须支持按 `requestId` 串联一次完整会话，不能只打印模糊的“收到音频”“调用模型成功”。

### 17.15 运维、升级和回滚

部署必须支持可回滚：

```text
/releases/20261016-001/
/releases/20261016-002/
/current -> /releases/20261016-002
```

升级流程：

```text
1. 上传新 release，不覆盖 current
2. 创建新 venv 或锁定依赖版本
3. 执行单元测试和配置校验
4. 运行 /healthz 本地检查
5. 短时间停止 Relay
6. 切换 current symlink
7. systemctl restart
8. 运行健康检查和 WSS 冒烟测试
9. 观察 10～20 分钟日志
10. 出现异常时切回上一个 release
```

升级约束：

- 不允许无锁定版本的 `pip install -U` 直接覆盖生产环境；
- 不能先升级 WebSocket、HTTP、Pydantic 等核心库再现场排错；
- 任何 provider 模型变更都必须先进行 Provider Contract Gate；
- 版本升级前保存当前配置和 systemd/Nginx 配置；
- 不要把用户语音和完整对话永久写入日志；
- 日志轮转，防止磁盘占满；
- API Key 轮换后重启 Relay 并验证旧 Key 不再使用；
- 每月至少验证一次证书续期和备份恢复流程。

### 17.16 部署验收标准

#### 网络和 TLS

- [ ] `voice.example.com` A 记录指向 `<server-ip>`；
- [ ] TCP 443 可访问；
- [ ] 证书域名匹配且未过期；
- [ ] ESP32 不需要关闭 TLS 证书验证；
- [ ] TCP 8000 无法从公网直接访问；
- [ ] HTTP 自动跳转 HTTPS；
- [ ] WSS Upgrade 成功。

#### Relay 应用

- [ ] `/healthz` 返回 200；
- [ ] `/readyz` 能反映配置缺失；
- [ ] `/metrics` 不泄露密钥和完整语音内容；
- [ ] systemd 开机自启动；
- [ ] 进程异常退出后自动重启；
- [ ] SIGTERM 能优雅取消上游任务；
- [ ] 单 worker 下可稳定运行；
- [ ] 日志可以通过 requestId 串联。

#### ASR/LLM

- [ ] ASR 任务严格等待 `task-started`；
- [ ] `finish-task` 后能收到最终结果；
- [ ] PCM/WAV 参数和真实字节一致；
- [ ] 北京 endpoint 使用北京 API Key；
- [ ] LLM 首 token 可在界面流式显示；
- [ ] SSE 拆包、空 delta、usage-only 事件处理正确；
- [ ] 401/403 不无限重试；
- [ ] 429 有有限退避；
- [ ] 断开设备会取消上游请求。

#### 设备体验

- [ ] 设备只保存 Relay token，不保存云 API Key；
- [ ] 设备可以显示 partial ASR 或“正在理解”状态；
- [ ] LLM delta 不触发每 token 的电子墨水刷新；
- [ ] 服务器重启后设备可以重新连接；
- [ ] Legacy OpenClaw 回退路径仍然可用；
- [ ] 连续 20 轮交互无明显内存泄漏；
- [ ] 首次可见文本和完整响应延迟均被日志记录。

### 17.17 建议的第一版实际部署组合

针对个人使用、追求响应速度和性价比，服务器 `<server-ip>` 上建议采用：

```text
操作系统：Ubuntu 24.04 LTS
公网入口：Nginx + Let's Encrypt
应用：Python 3.11 + FastAPI + Uvicorn
设备传输：WSS /api/v1/voice/stream
ASR：阿里云实时 Paraformer 或 Fun-ASR WebSocket
LLM：阿里云百炼 Qwen Flash OpenAI-compatible streaming
进程管理：systemd
监控：journald + 基础 /metrics
数据库：第一版不需要
Redis：第一版不需要
Docker：可选，不是必需
GPU：不需要
```

第一版不要引入数据库、Redis、Kubernetes、LiveKit 或复杂 Agent 编排。只有在出现以下需求时再增加基础设施：

- 多台设备并发；
- 多 Relay 实例；
- 需要跨进程共享 session；
- 需要持久化会话和配额；
- 需要 WebRTC/多人实时音视频；
- 需要本地 ASR/LLM 模型。

这套部署方案的目标是先用最少组件获得可靠的：

```text
ESP32 → WSS → <server-ip> → 实时 ASR → 流式 LLM → 设备文本显示
```

并保留将来更换 ASR、LLM、TTS 或接入 OpenClaw 的能力。
