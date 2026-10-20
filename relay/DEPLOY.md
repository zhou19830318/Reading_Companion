# VoiceRelay 部署手册（DEPLOY）

本文是服务端**部署与运维**手册。协议、接口、配置项含义见同目录 [README.md](./README.md)。

## 架构总览

```
阅读器 ──ws://<server-ip>:8080/api/v1/voice/stream──▶ nginx (:8080, dev)
                                                        │
                                          uvicorn 127.0.0.1:8000 (单 worker)
                                                        │
                            ┌───────────────────────────┴──────────────────────┐
                            ▼                                                  ▼
                    DashScope 实时 ASR                                DashScope LLM / 生图
                  (paraformer-realtime-v2)                          (qwen-flash / wanx)
```

- 设备**只持有 token**，云厂商 API Key 只存在服务器上。
- 进程由 systemd 托管，单进程（多 worker 会破坏全局限流计数，见 README "Multiple onepage devices"）。

## 服务器目录结构

| 路径 | 内容 |
|---|---|
| `/opt/reading-companion-relay/releases/<TS>/` | 每次安装生成的时间戳 release（`relay/`、`dict-data/`、`requirements.txt`、独立 `.venv`、`pip-freeze.txt`） |
| `/opt/reading-companion-relay/current` | 指向生效 release 的软链（回滚=改指旧 release） |
| `/opt/reading-companion-relay/logs/`、`run/` | 日志与可写运行时目录（含 `run/t2i/` 生图资产，systemd 沙箱仅放行这两处写入） |
| `/etc/reading-companion/voice-relay.env` | **唯一存放密钥的文件**（root:voice-relay，640） |
| `/etc/systemd/system/reading-companion-relay.service` | systemd 单元（install.sh 安装） |
| `/etc/nginx/sites-available/reading-companion-relay.conf` | nginx 站点（dev=8080 / prod=443） |

查看当前生效版本：

```bash
readlink -f /opt/reading-companion-relay/current
```

## 前置条件

- Debian/Ubuntu 系服务器 + sudo（`install.sh` 以 root 运行，缺的 apt 包会自动补装：python3、python3-venv、nginx、curl、jq 等）。
- 云安全组放行 **TCP 8080**（dev 模式）；prod 模式需域名 + 443。
- 本机有仓库检出（打包用）与 `scp` 到服务器的权限。

## 1. 打包

在仓库的 `relay/` 目录执行：

```bash
bash tools/make-package.sh
# 产物: dist/voice-relay-<TS>.tar.gz
# 内容: relay/ + dict-data/ + deploy/ + tools/ + requirements.txt + README.md + DEPLOY.md
```

`dict-data/`（离线字典数据）是打包**必需**项；缺失时脚本报错退出。

## 2. 上传并安装

```bash
scp dist/voice-relay-<TS>.tar.gz <user>@<server-ip>:/tmp/
ssh <user>@<server-ip>
tar xzf /tmp/voice-relay-<TS>.tar.gz -C /tmp
sudo bash /tmp/voice-relay-<TS>/deploy/install.sh \
    --source /tmp/voice-relay-<TS> \
    --mode dev \
    --dashscope-key sk-...        # 可省略；省略且在终端时会交互提示（回车=mock 模式）
```

常用参数：`--device-id`（设备 ID）、`--asr-model` / `--llm-model`、`--dashscope-region`、`--public-url`（手动生成设备连接地址）、`--mode prod --domain voice.example.com`。

`install.sh` 是**幂等**的，一次执行完成：

1. 校验 `--source` 含 `relay/` 与 `requirements.txt`；
2. 建 `voice-relay` 系统用户与目录；
3. 复制出新 release（去 `__pycache__`），建 `.venv` 并 `pip install -r requirements.txt`（写入 `pip-freeze.txt`）；
4. env 文件：首次生成（随机 device/admin token），已存在则保留并**回填**新增配置键、更新 `RELAY_VERSION`；
5. `current` 软链切到新 release，安装/重启 systemd 服务；
6. 安装 nginx 站点（dev/prod）并 reload；
7. 冒烟测试：`127.0.0.1:8000/healthz`、`/readyz`（校验 `dict=local`），dev 模式再测 `:8080`；
8. 清理旧 release（保留最近 5 个）；
9. 打印阅读器需要的 **Relay URL / Device ID / Token**。

## 3. 配置阅读器

1. 阅读器联网，打开其 Web UI（`http://<reader-ip>/settings`）；
2. **Voice Relay** 卡片粘贴安装脚本输出的三项：

   ```
   Relay URL:  ws://<server-ip>:8080/api/v1/voice/stream
   Device ID:  reading-companion-001
   Token:      <RELAY_DEVICE_TOKEN>
   ```

3. 保存（设备侧 Voice Relay 开关默认开启）。设备端 MiMo 语音 key 与 OpenClaw 可留空——relay 同时取代两者。

## 4. 密钥与配置变更

所有配置都在 `/etc/reading-companion/voice-relay.env`（改完必须重启）：

```bash
sudoedit /etc/reading-companion/voice-relay.env
sudo systemctl restart reading-companion-relay
```

部署后最常见的三件事（完整键表见 `deploy/voice-relay.env.example` 与 README）：

| 目的 | 改动 |
|---|---|
| 从 mock 切到真实语音/LLM | `DASHSCOPE_API_KEY=sk-...`、`ASR_PROVIDER=paraformer`、`LLM_PROVIDER=dashscope-openai-compatible`；或直接重跑 `install.sh --dashscope-key sk-...` |
| 开通生图 | `T2I_PROVIDER=dashscope`（或 `openai` 配 `T2I_BASE_URL`/`T2I_API_KEY`；`mock`=本地占位图零成本） |
| 轮换设备 token | `RELAY_DEVICE_TOKEN=...`（或 `RELAY_DEVICE_TOKENS=device:token,...` 按设备区分） |

字典数据随 release 发布（`DICT_SOURCE=local`），升级 release 即升级词库；改成 `DICT_SOURCE=llm` 可回退到模型生成卡片（可能有事实错误，不推荐）。

## 5. 升级

重新 `make-package.sh` → 上传 → 重跑 `install.sh --source ...`。环境文件、token、密钥全部保留；旧 release 自动清理（留 5 个）。

## 6. 回滚

```bash
ln -sfn /opt/reading-companion-relay/releases/<old-TS> \
        /opt/reading-companion-relay/current
sudo systemctl restart reading-companion-relay
```

`<old-TS>` 从 `ls /opt/reading-companion-relay/releases/` 里挑（按时间倒序）。

## 7. 验证与排障

```bash
systemctl status reading-companion-relay --no-pager
journalctl -u reading-companion-relay -f              # 结构化 JSON 一行一事件
journalctl -u reading-companion-relay --since "5 min ago" | grep contracts   # 上游契约门

curl -s localhost:8000/healthz | jq .                 # 存活
curl -s localhost:8000/readyz  | jq .                 # asr/llm/dict/t2i 配置状态
curl -s localhost:8080/healthz | jq .                 # 经 nginx（dev）
```

- `/readyz` 里 `"dict": "local"` 才说明离线字典生效；`local_missing_data` 表示 release 缺 `dict-data/`。
- 无硬件联调：`python tools/test_client.py --pcm speech.wav`（协议）；`--realtime` 走真实上游；4 设备压测 `tools/load_test.py --clients 4 --rounds 5`。
- 设备连不上：先确认安全组 8080、nginx 站点已 enable、token 与 env 一致（`sudo grep RELAY_DEVICE_TOKEN /etc/reading-companion/voice-relay.env`）。
- 日志脱敏过滤器保证 API key 不会进日志；`/metrics` 仅内网可访问。

## 8. dev 与 prod 模式

| | dev（当前现网） | prod |
|---|---|---|
| 入口 | nginx `:8080`，`ws://` | nginx `:443`，`wss://`（需域名 + 证书） |
| 安装 | `--mode dev` | `--mode prod --domain voice.example.com` |
| 云安全组 | 开 TCP 8080 | 开 TCP 443 |

## 9. 安全基线

- 密钥只在 `voice-relay.env`（root:voice-relay 640），永不入库、永不下发到设备、不回显到 Web UI。
- systemd 沙箱：`ProtectSystem=strict` + 仅放行 `logs/`、`run/` 两个写路径；`NoNewPrivileges`、`PrivateTmp`。
- 设备认证靠 `Authorization: Bearer` + 可选 `ALLOWED_DEVICE_IDS` 白名单；空 token 配置拒绝启动。
- 结构化日志带脱敏过滤，API key 不可能被打印。
