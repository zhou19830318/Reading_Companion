# Voice Relay 服务端部署说明

本文件夹是 ESP32-C61 阅读伴侣"语音中继"（FastAPI 服务）的服务端部署快照，
打包于 2026-10-17，内容与主仓库 `relay/` 目录一致（已脱敏：服务器地址以
`<server-ip>` 占位，部署时替换为实际地址）。

设备端通过一个 WebSocket 连接中继，中继再对接 DashScope 的实时语音识别
（paraformer-realtime-v2）和流式大模型（qwen-flash）。设备端不接触云端
API Key。

## 目录结构

```
VoiceRelay/
├── voice-relay-20261017-135119.tar.gz   # 部署包（上传服务器用）
├── relay/          # FastAPI 服务源码（Python 包）
├── deploy/         # install.sh、systemd unit、nginx 配置、env 模板
├── tools/          # test_client.py、load_test.py、make-package.sh
├── requirements.txt
├── README.md       # 中继自身的协议/接口文档
└── DEPLOY.md       # 本文件
```

本文件夹根目录可直接作为 `install.sh --source` 的源目录使用。

## 现网部署信息（2026-10-17）

| 项 | 值 |
|---|---|
| 服务器 | `<server-ip>`（部署时替换） |
| 模式 | dev（nginx 监听 8080 `ws://<server-ip>:8080/api/v1/voice/stream`） |
| 现网 release | `20261016-233902`（与本地快照一致） |
| 安装根目录 | `/opt/reading-companion-relay`（`releases/<TS>` + `current` 软链） |
| 配置文件 | `/etc/reading-companion/voice-relay.env`（root:voice-relay 640，唯一存密钥的地方） |
| 服务名 | `reading-companion-relay` |

## 全新安装

```bash
scp voice-relay-20261017-135119.tar.gz <user>@<server-ip>:/tmp/
ssh <user>@<server-ip>
tar xzf /tmp/voice-relay-20261017-135119.tar.gz -C /tmp
sudo bash /tmp/voice-relay-20261017-135119/deploy/install.sh \
    --source /tmp/voice-relay-20261017-135119 --mode dev
```

首次交互式运行会提示输入 DashScope API Key（留空 = mock 模式，只有罐头
回复）；装完脚本会打印 Relay URL / 设备 ID / 令牌。需要真实语音+大模型时
传 `--dashscope-key sk-...`（任何一次运行都可传，会轮换 Key）。

## 升级 / 重跑

`install.sh` 是幂等的：新建一个时间戳 release、保留现有 env 文件、原子切换
`current` 软链并重启服务。同样解包后跑上面的 install 命令即可。

## 回滚

```bash
ls /opt/reading-companion-relay/releases/     # 选一个旧版本
sudo ln -sfn /opt/reading-companion-relay/releases/<旧TS> /opt/reading-companion-relay/current
sudo systemctl restart reading-companion-relay
```

## 取设备令牌（填到阅读器网页）

```bash
sudo grep RELAY_DEVICE_TOKEN /etc/reading-companion/voice-relay.env
```

设备端：阅读器网页 `http://<阅读器IP>/settings` → Voice Relay 卡片，
**Relay URL / 设备 ID / 令牌三栏都填**（固件要求三者非空才走中继）。
网页只回显 URL 和设备 ID，不回显令牌明文。

- 多台设备可共用同一个令牌，但**每台要填不同的设备 ID**；
- 想每台独立令牌：在 env 里加 `RELAY_DEVICE_TOKENS="设备ID:令牌,..."`
  （优先于共享令牌）；`ALLOWED_DEVICE_IDS` 可选白名单。

## 多设备并发上限

| 限制 | 默认值 | 超出时 |
|---|---|---|
| 同时 WebSocket 连接 | 32（`MAX_WS_CONNECTIONS`） | close 1013 |
| 同时进行的语音会话 | 4（`MAX_CONCURRENT_SESSIONS`） | 该轮收到 SERVER_OVERLOADED |
| 单台设备同时会话 | 1（`MAX_SESSIONS_PER_DEVICE`） | 同设备第二轮被拒 |

词典/问答（`POST /api/v1/chat`）不占语音会话名额。4 路不够时改 env 后
`sudo systemctl restart reading-companion-relay`（前提是 DashScope 账号
并发额度足够）。

## 运维命令

```bash
systemctl status reading-companion-relay --no-pager
journalctl -u reading-companion-relay -f
curl -s http://127.0.0.1:8000/healthz
curl -s http://127.0.0.1:8000/readyz
```

## 快照维护

本文件夹是主仓库 `relay/` 目录的快照，**不会自动同步**。源码改动后重新打包：

```bash
# 在项目根目录
bash relay/tools/make-package.sh VoiceRelay
tar xzf VoiceRelay/voice-relay-<新TS>.tar.gz -C VoiceRelay --strip-components=1
# 视情况删除 VoiceRelay/ 里的旧 tar.gz
```

开发/测试（pytest、requirements-dev.txt、tests/）不在本快照内，
需要时从主仓库 `relay/` 目录取。
