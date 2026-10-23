# Reading Companion Voice Relay

位于 DashScope（阿里云百炼 Alibaba Cloud Model Studio）实时 ASR 与流式 LLM
之前的 FastAPI 服务。ESP32-C61 onepage 设备永远不会接触到云端 API key 或端点；
它们通过一条 WebSocket 使用一套精简的 JSON + PCM 协议
与本中继通信。

部署：Ubuntu 一条命令或 Windows 安装包，两者最终都会落到管理页面（`/admin`），
给出阅读器的 URL / Device ID / Token——参见 `DEPLOY.md`。

```
device ──WS /api/v1/voice/stream──▶ relay ── WSS ──▶ DashScope ASR (paraformer-realtime-v2)
                                      │
                                      └── HTTPS SSE ─▶ DashScope LLM (qwen-flash, OpenAI-compatible)
```

## 协议（v1）

- 16 kHz 单声道 PCM s16le，40 ms 一帧（1280 字节），单次语音最长 30 s。
- 设备 → 中继：`session.start` → `audio.start` → 二进制音频帧 →
  `audio.end`（或 `session.cancel`）。
- 中继 → 设备：`session.started`、`asr.partial`、`asr.final`、`llm.start`、
  `llm.delta`、`llm.final`、`session.done`、`error`
  （错误码：`BAD_PROTOCOL`、`UNAUTHORIZED`、`SERVER_OVERLOADED`、
  `ASR_CONNECTION_ERROR`、`ASR_AUTH_ERROR`、`ASR_RATE_LIMITED`……）。
- 带内错误不会关闭连接；协议/认证违规才会关闭。
- 握手请求头：`Authorization: Bearer <token>`、`X-Device-Id`、
  `X-Voice-Protocol: 1`。

无需硬件即可运行的客户端：`python tools/test_client.py --pcm speech.wav`。

## 认证（文档 17.5 节）

`RELAY_DEVICE_TOKEN` 是共享 token；`RELAY_DEVICE_TOKENS` 可按
`deviceId:token,...` 的形式为各设备固定专属 token（设备级条目优先于共享
token）。`ALLOWED_DEVICE_IDS` 可选地对设备 id 做白名单。配置为空时
拒绝以开放模式运行。`RELAY_ADMIN_TOKEN` 启用仅管理员可用的
`POST /api/v1/chat` 文本调试接口。

## 多台 onepage 设备（多设备设计）

多台 onepage 设备连接到同一个中继；容量在三个层面上强制约束，
使一台行为异常的设备不会饿死其他设备：

| 层级 | 配置项（env） | 默认值 | 超限后 → |
|---|---|---|---|
| 打开的设备 socket 数 | `MAX_WS_CONNECTIONS` | 32 | 新 socket 先被接受、随即以 WS code 1013（稍后重试）关闭；`ws_rejected_total{reason="connections"}` |
| 并发语音会话数（所有设备） | `MAX_CONCURRENT_SESSIONS` | 4 | 带内 `error: SERVER_OVERLOADED`；`ws_rejected_total{reason="sessions"}` |
| 单设备并发会话数 | `MAX_SESSIONS_PER_DEVICE` | 1 | 带内 `error: SERVER_OVERLOADED`；`ws_rejected_total{reason="device_sessions"}` |

为多设备运行设计时考虑的要点：

- `MAX_CONCURRENT_SESSIONS` 是 DashScope 配额的调节阀：每个活跃会话占用一条
  上游 ASR WebSocket 和一个 LLM HTTPS 请求。应保持在账户对应 ASR 模型的
  并发配额以内；取值 4 已经过压测验证（4 个客户端 × 5 轮，
  零失败）。
- 单设备限额及所有日志所用的设备身份，是经过认证的 `X-Device-Id` 请求头，
  而不是 `session.start` 内部的 `deviceId`
  （设备无法靠改名规避自己的限额）。
- 无会话运行时，空闲 socket 会在 `SESSION_IDLE_TIMEOUT_SECONDS`（60 s）后被回收，
  因此被遗弃的连接不会占用限额。
- `SERVER_OVERLOADED` 可重试：设备应显示简短的“服务繁忙”提示，
  并回退到旧有的 MiMo STT + OpenClaw 路径。
- 中继刻意设计为单进程：会话、限额与指标都在内存中且保持一致。
  若将来需要水平扩展，请用 nginx 的 sticky 路由保持亲和性
  （一台设备 → 一个实例），或把计数/限额迁到 Redis；
  不要在简单轮询之后运行多个 uvicorn worker——
  那样限额会变成按 worker 计，
  聚合指标也会重复计数。

多设备验证用的压测：

```bash
RELAY_DEVICE_TOKEN=... python tools/load_test.py --pcm speech.pcm \
    --clients 4 --rounds 5            # 4 devices, one reused socket each
```

## Provider（提供方）

`ASR_PROVIDER`/`LLM_PROVIDER` 取值为 `mock`（开发用，无上游）或真实的
DashScope 适配器。真实凭据只存放在
`/etc/reading-companion/voice-relay.env`（`DASHSCOPE_API_KEY`、
`DASHSCOPE_WORKSPACE_ID`、`DASHSCOPE_REGION`）；参见
`deploy/voice-relay.env.example`。启动日志会为每个 provider 打印完整的
上游契约（即“契约门”）；每次改动后都要验证：

```bash
journalctl -u reading-companion-relay --since "5 min ago" | grep contracts
curl -s localhost:8000/readyz      # {"status":"ready","asr":"configured",...}
```

## 字典卡片（离线数据）

阅读器的字典卡片（`POST /api/v1/chat`，`mode="dict"`，由 `DictionaryActivity` 发送）
由 `dict-data/` 中的离线数据提供，而非来自模型：读音/部首/笔画来自 Unihan，
义项与组词来自 chinese-xinhua（MIT），并按 jieba 词频（MIT）排序；
`dict-data/META.json` 记录各数据源的 SHA-256 与许可证。
数据中未收录的字会得到一张明确的 `未收录` 卡片——
不做任何猜测。

```
【字】的
【拼音】de、dì、dí
【部首笔画】白·8画
【释义】de：用在词或词组后表明形容词性美丽的。 dí：真实，实在的确。的当（dàng）。……
【组词】目的、真的、似的、目的地、有的是
```

（原文在释义内部用 `～` 代替词条字；中继在发送前会替换为实际的字。
选中非汉字时会得到一张
`无法识别 / 请输入单个汉字` 卡片。）

设置项：`DICT_SOURCE=local`（默认）或 `llm`（模型生成卡片——引入本地数据之前的行为；
可能含有事实错误）。若设为 `DICT_SOURCE=local` 但数据缺失，中继会记录错误并降级到
LLM 路径；`/readyz` 报告
`"dict": "local" | "local_missing_data" | "llm"`。

数据源更新后重建（需要三个源文件；
脚本仅用标准库）：

```bash
python tools/build_dict_data.py \
    --word  xinhua/data/word.json --ci xinhua/data/ci.json \
    --unihan Unihan.zip --jieba dict.txt \
    --out dict-data
python -m pytest -q tests/test_dict_source.py   # rendering/lookup invariants
```

`dict-data/` 在首次查询时读入一次，从不写入（systemd 单元以
`ProtectSystem=strict` 运行）。`tools/make-package.sh` 要求包含该目录，
`deploy/install.sh` 会把它复制进每个 release；从字典功能之前的旧安装包
升级的运维人员，其 env 文件会自动回填
`DICT_SOURCE=local`。

## 文生图卡片

`POST /api/v1/image`（`{"text": "<prompt>"}`）为阅读器屏幕生成一张图片，
返回元数据加一句等待提示语；图片本身从响应中的资源 URL 获取
（同样的 token 认证）：

```bash
TOKEN=...  # device or admin token
curl -s -H "Authorization: Bearer $TOKEN" -H "X-Device-Id: debug-client" \
     -H 'Content-Type: application/json' \
     -d '{"text":"山间小屋，炊烟袅袅"}' localhost:8000/api/v1/image | jq .
# {"requestId":"...","text":"正在铺开画纸……",
#  "image":{"id":"<16 hex>","url":"/api/v1/image/<id>","width":480,"height":800,
#           "bytes":48062,"format":"bmp"},"usage":{"seconds":2.5}}
curl -s -H "Authorization: Bearer $TOKEN" -H "X-Device-Id: debug-client" \
     -o /tmp/card.bmp localhost:8000/api/v1/image/<id>
```

服务器用 Pillow 完成全部转换：源图 → 适配面板尺寸（默认 480x800 竖屏）→
自动对比度 → Floyd-Steinberg 抖动 → **1-bit BMP，恰好 48,062 字节**
（62 字节文件头 + 48,000 字节位图）。
这正是固件 Bitmap 读取器接受的格式
（`lib/GfxRenderer/BitmapHelpers.cpp`），因此设备只需把行数据流式写入
帧缓冲——无需在设备端解码
（在 ESP32-C61 上解 JPEG 约需 5-10 s；这条路径几乎瞬时完成）。

Provider：`T2I_PROVIDER=mock`（默认）在本地绘制一张确定性的占位图——
不调用云端、零成本，因此可在花费任何生图配额之前对完整的接口/资源路径
做端到端测试。`T2I_PROVIDER=dashscope` 提交 wanx 文生图任务
（`T2I_MODEL`，默认 `wanx2.1-t2i-turbo`）并轮询直至完成；需要
`DASHSCOPE_API_KEY`，缺少时以 `T2I_NOT_CONFIGURED` 拒绝关闭。
`T2I_PROVIDER=openai` 向 `<T2I_BASE_URL>/images/generations` POST
`{"model","prompt","size","response_format":"b64_json"}`，接受内联
`b64_json` 或需要下载的 `url`，需要 `T2I_BASE_URL` + `T2I_API_KEY`
（缺少时同样拒绝关闭）——覆盖 OpenAI 及任何 OpenAI 兼容的图像网关
（SiliconFlow、vLLM/SD-WebUI 适配器等）。
`T2I_SIZE` 是 provider 的生成尺寸，格式 `W*H`（默认 `768*1280`），
会转换为 OpenAI 的 `WxH`。内容审核拒绝在所有 provider 上都以
`T2I_REJECTED` 返回。其他调节项：`T2I_TIMEOUT_S`、
`T2I_POLL_INTERVAL_S`、`T2I_MOCK_DELAY_S`、`T2I_WIDTH`/`T2I_HEIGHT`、
`T2I_ASSET_TTL_S`（资源在此时间后删除，默认 24 小时）。
`T2I_ASSET_DIR` 必须位于 systemd 沙箱的可写路径内——
`install.sh` 将其固定为
`/opt/reading-companion-relay/run/t2i`。

文本与语音的 provider 彼此独立：可以让 `T2I_PROVIDER=dashscope` 而 ASR/LLM
保持 mock，反之亦然。`/readyz` 报告
`"t2i": "mock" | "configured" | "missing_config"`。

## 部署与回滚

完整运维手册（中文）：[VoiceRelay 部署手册](./DEPLOY.md)——打包、安装、
env/密钥、升级、回滚、验证、dev/prod 模式、安全基线。

```bash
# package: build a self-contained tarball under dist/
bash tools/make-package.sh
# install: unpack on the server and run the installer
tar xzf voice-relay-<TS>.tar.gz -C /tmp
sudo bash /tmp/voice-relay-<TS>/deploy/install.sh --source /tmp/voice-relay-<TS> --mode dev
# or from a source checkout directly
sudo bash deploy/install.sh --source /tmp/relay-src --mode dev
# rollback (<old-TS> from /opt/reading-companion-relay/releases/)
ln -sfn /opt/reading-companion-relay/releases/<old-TS> /opt/reading-companion-relay/current
systemctl restart reading-companion-relay
```

`install.sh` 是幂等的：创建新的时间戳 release、保留环境文件（之后新增的键会
自动回填）、管理 nginx 站点、冒烟测试、清理旧 release（保留 5 个）。
模式：`dev`（经 nginx 在 :8080 上走 ws://）和 `prod`（在云控制台放行端口且
有域名后，在 443 上走 wss://）。

DashScope 配置：首次交互式运行时，安装器会提示输入 API key（留空 = mock 模式）；
任何一次运行都可传 `--dashscope-key sk-...` 以非交互方式配置或轮换
（`--asr-model`、`--llm-model`、`--dashscope-region`、`--device-id`、
`--public-url` 均为可选覆盖项）。
运行结束会打印 Relay URL / Device ID / Token，粘贴到阅读器的 Web UI
（Settings > Voice Relay）即可。阅读器无需其他配置：
配好中继后，设备端的 MiMo key 与 OpenClaw 就变成可选的了。

## 可观测性

- `GET /metrics`（仅内网）：`voice_sessions_total{result}`、
  `voice_sessions_active`、`ws_connections_active`、`ws_rejected_total{reason}`、
  `voice_latency_ms{stage}`（`asrFirstPartial`、`asrFinal`、`llmFirstToken`、
  `llmFinal`、`visibleTotal`）、`asr_requests_total`、`llm_requests_total`、
  `websocket_disconnects_total{reason}`、`relay_process_rss_bytes`。
- 结构化 JSON 日志，每个会话状态变化记录一条事件；脱敏过滤器保证 API key
  绝不会被写入日志（即使调用方一时疏忽）。

## 测试

```bash
pip install -r requirements-dev.txt
python -m pytest -q          # protocol, session, auth, providers, capacity, mock e2e
```

真实凭据契约检查（无需硬件）：

```bash
python tools/test_client.py --pcm speech-16k.pcm --realtime
```
