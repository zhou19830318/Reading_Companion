# Web 服务器端点

本文档描述 CrossPoint Reader 处于文件传输模式或 Calibre 无线模式时，
可用的 HTTP、WebSocket、WebDAV 和发现端点。

- HTTP 服务器：端口 80
- WebSocket 上传服务器：端口 81
- UDP 发现监听器：端口 8134
- WebDAV：端口 80，由同一 HTTP 服务器处理

示例使用 `crosspoint.local`。在 OnePage (ESP32-C61) 构建中未编译 mDNS，
因此该主机名永远不会解析成功 —— 请在下面每个示例中将其替换为设备屏幕上
显示的 IP 地址。

## HTTP 页面

| 方法 | 路径 | 用途 |
|------|------|------|
| `GET` | `/` | 主页/状态页 |
| `GET` | `/files` | 文件管理器页面 |
| `GET` | `/settings` | Web 设置页面 |
| `GET` | `/fonts` | SD 卡字体管理器页面 |
| `GET` | `/js/jszip.min.js` | 文件管理器使用的 JavaScript 资源 |

## 设备状态

### `GET /api/status`

```bash
curl http://crosspoint.local/api/status
```

响应：

```json
{
  "version": "1.0.0",
  "ip": "192.168.1.100",
  "mode": "STA",
  "rssi": -45,
  "freeHeap": 123456,
  "uptime": 3600,
  "device": "X4"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `version` | string | 固件版本 |
| `ip` | string | 设备 IP 地址 |
| `mode` | string | 已加入 Wi-Fi 时为 `"STA"`，热点模式时为 `"AP"` |
| `rssi` | number | Wi-Fi RSSI（dBm）；AP 模式下为 `0` |
| `freeHeap` | number | 空闲堆内存（字节） |
| `uptime` | number | 开机以来的秒数 |
| `device` | string | 硬件 id：`"X3"` 或 `"X4"`。OnePage 构建跳过 I2C 指纹探测并始终报告 `"X4"`（它复用 X4 的布局度量） |

## 文件管理

### `GET /api/files`

列出目录下的文件和文件夹。

```bash
curl "http://crosspoint.local/api/files?path=/Books"
```

查询参数：

| 参数 | 必需 | 默认值 | 说明 |
|------|------|--------|------|
| `path` | 否 | `/` | 要列出的目录 |

响应：

```json
[
  {"name":"MyBook.epub","size":1234567,"isDirectory":false,"isEpub":true},
  {"name":"Notes","size":0,"isDirectory":true,"isEpub":false}
]
```

除非设备设置 `showHiddenFiles` 已启用，否则隐藏的点文件会被省略。
`System Volume Information` 和 `XTCache` 始终是隐藏/受保护的。

### `GET /download`

从 SD 卡下载文件。

```bash
curl -OJ "http://crosspoint.local/download?path=/Books/MyBook.epub"
```

查询参数：

| 参数 | 必需 | 说明 |
|------|------|------|
| `path` | 是 | 要下载的文件路径 |

受保护的点文件、`System Volume Information` 和 `XTCache` 无法下载。
EPUB 文件以 `application/epub+zip` 提供；其他文件使用
`application/octet-stream`。

### `POST /upload`

使用 HTTP multipart 表单数据上传文件。

```bash
curl -X POST -F "file=@mybook.epub" "http://crosspoint.local/upload?path=/Books"
```

查询参数：

| 参数 | 必需 | 默认值 | 说明 |
|------|------|--------|------|
| `path` | 否 | `/` | 目标目录 |

成功响应：

```text
File uploaded successfully: mybook.epub
```

注意：

- 同名的现有文件会被覆盖。
- 上传成功后会清除上传路径对应的 EPUB 缓存数据。
- HTTP 上传在刷写到 SD 卡之前使用 4 KB 写缓冲区。

### `POST /mkdir`

创建文件夹。

```bash
curl -X POST -d "name=NewFolder&path=/" http://crosspoint.local/mkdir
```

表单参数：

| 参数 | 必需 | 默认值 | 说明 |
|------|------|--------|------|
| `name` | 是 | - | 新文件夹名称 |
| `path` | 否 | `/` | 父文件夹 |

### `POST /rename`

重命名文件。

```bash
curl -X POST -d "path=/Books/old.epub&name=new.epub" http://crosspoint.local/rename
```

表单参数：

| 参数 | 必需 | 说明 |
|------|------|------|
| `path` | 是 | 现有文件路径 |
| `name` | 是 | 新文件名（不是路径） |

此端点只能重命名文件。重命名前会清除旧的 EPUB 缓存路径。

### `POST /move`

将文件移动到现有文件夹中。

```bash
curl -X POST -d "path=/Books/mybook.epub&dest=/Read" http://crosspoint.local/move
```

表单参数：

| 参数 | 必需 | 说明 |
|------|------|------|
| `path` | 是 | 现有文件路径 |
| `dest` | 是 | 现有目标文件夹 |

此端点只能移动文件。移动前会清除旧的 EPUB 缓存路径。

### `POST /delete`

删除一个或多个文件或空文件夹。

```bash
curl -X POST -d "path=/Books/mybook.epub" http://crosspoint.local/delete
curl -X POST -d 'paths=["/Books/old.epub","/OldFolder"]' http://crosspoint.local/delete
```

表单参数：

| 参数 | 必需 | 说明 |
|------|------|------|
| `path` | 是（除非提供 `paths`） | 要删除的单个路径 |
| `paths` | 是（除非提供 `path`） | 要删除的路径的 JSON 数组 |

受保护的项目无法删除。非空文件夹会被拒绝。已删除文件的 EPUB 缓存
数据会被清除。

## 设置 API

### `GET /api/settings`

返回可编辑设置的流式 JSON 数组。每一项包含公共字段以及类型特定的字段。

```bash
curl "http://crosspoint.local/api/settings?lang=en"
```

`lang` 接受 `en` 或 `zh-Hans`，会对 `name` 和选项标签进行本地化；
省略时默认使用设备自身的语言。服务器不对响应做缓存清除，因此
浏览器在更改后可能需要 `?_=<timestamp>` 来重新获取。

示例项：

```json
{
  "key": "fontSize",
  "name": "Font Size",
  "category": "Reader",
  "type": "enum",
  "value": 1,
  "options": ["Small", "Medium", "Large"]
}
```

类型：

| 类型 | 附加字段 |
|------|----------|
| `toggle` | `value`（`0` 或 `1`） |
| `enum` | `value`、`options` |
| `value` | `value`、`min`、`max`、`step` |
| `string` | `value` |

字体系列设置在 SD 卡字体族安装后会包含它们。

### `POST /api/settings`

从 JSON 对象应用部分设置更新。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"fontSize":2,"showHiddenFiles":1}' \
  http://crosspoint.local/api/settings
```

成功响应：

```text
Applied 2 setting(s)
```

### `POST /api/settings/reset`

将 `settings.json` 的每个字段恢复为编译时的出厂默认值并保存。
Wi-Fi 网络、OPDS 服务器和云凭据存储在单独的存储中，会被保留。
设备 UI 语言也会恢复为英文。

```bash
curl -X POST http://crosspoint.local/api/settings/reset
```

响应：`{"ok":true}`（200），或保存失败时返回 500 及纯文本原因。

### `POST /api/reboot`

重启设备。`Rebooting` 响应会先发送，然后固件在约 0.5 秒后重启，
因此该请求仍可能报告连接中断。

```bash
curl -X POST http://crosspoint.local/api/reboot
```

## 字体管理 API

### `GET /api/fonts`

列出已安装的 SD 卡字体族。

```bash
curl http://crosspoint.local/api/fonts
```

响应：

```json
{
  "maxFamilies": 128,
  "families": [
    {
      "name": "Literata",
      "sizes": [12, 14, 16, 18],
      "files": [
        {"name": "Literata_12.cpfont", "size": 123456}
      ]
    }
  ]
}
```

### `POST /api/fonts/upload`

将一个 `.cpfont` 文件上传到字体族文件夹中。

```bash
curl -X POST \
  -F "family=Literata" \
  -F "file=@Literata_12.cpfont" \
  http://crosspoint.local/api/fonts/upload
```

处理程序在接受文件之前会校验字体族名称、`.cpfont` 文件名以及
`CPFONT` 魔数。

成功响应：

```json
{"ok":true}
```

### `POST /api/fonts/delete`

删除已安装的字体族。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"family":"Literata"}' \
  http://crosspoint.local/api/fonts/delete
```

成功响应：

```json
{"ok":true}
```

## OPDS 服务器 API

### `GET /api/opds`

列出已保存的 OPDS 服务器。密码永不返回。

```bash
curl http://crosspoint.local/api/opds
```

响应：

```json
[
  {
    "index": 0,
    "name": "My Catalog",
    "url": "http://calibre.local:8080/opds",
    "username": "reader",
    "hasPassword": true
  }
]
```

### `POST /api/opds`

添加或更新 OPDS 服务器。包含 `index` 即更新现有条目。
更新时若省略 `password`，则保留现有密码。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"name":"My Catalog","url":"http://calibre.local:8080/opds","username":"reader","password":"secret"}' \
  http://crosspoint.local/api/opds
```

### `POST /api/opds/delete`

按索引删除 OPDS 服务器。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/opds/delete
```

## Wi-Fi 凭据 API

### `GET /api/wifi`

列出已保存的 Wi-Fi 网络。密码永不返回。

```bash
curl http://crosspoint.local/api/wifi
```

响应：

```json
[
  {
    "index": 0,
    "ssid": "HomeWiFi",
    "hasPassword": true,
    "isLastConnected": true
  }
]
```

### `POST /api/wifi`

添加或更新已保存的 Wi-Fi 网络。包含 `index` 即更新现有条目。
更新时若省略 `password`，则保留现有密码。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"ssid":"HomeWiFi","password":"secret"}' \
  http://crosspoint.local/api/wifi
```

### `POST /api/wifi/delete`

按索引删除已保存的 Wi-Fi 网络。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"index":0}' \
  http://crosspoint.local/api/wifi/delete
```

## 云服务 API (NVS)

### `GET /api/cloud`

返回 OpenClaw 网关配置和 MiMo 语音转文字配置。密钥永不返回 ——
仅返回存在标志 `hasToken` / `hasSttKey`。

```bash
curl http://crosspoint.local/api/cloud
```

响应：

```json
{
  "host": "192.168.1.10",
  "port": 18789,
  "path": "/",
  "tls": false,
  "protocol": "v2.openclaw.io",
  "caPath": "",
  "hasToken": true,
  "sttUrl": "",
  "hasSttKey": false
}
```

### `POST /api/cloud`

部分更新：请求体中省略的字段保持其存储的 NVS 值；密钥
（`token` / `sttKey`）发送 `""` 即清除，省略则表示不变。

```bash
curl -X POST \
  -H "Content-Type: application/json" \
  -d '{"host":"192.168.1.10","port":18789,"token":"..."}' \
  http://crosspoint.local/api/cloud
```

### `POST /api/openclaw/test`

对网关进行一次性的 WebSocket 可达性 + 握手探测（使用请求体中的
值，而非已保存的值；省略的 `token` 会回退到存储的值）。不会持久化
任何内容。响应 JSON：`ok`、`stage`、`detail`、`ms`、`url`、
`firstFrame`（当握手收到一帧时）以及网关回复 *pairing pending* 时的
`requestId` —— 设置页面会显示对应的
`openclaw devices approve <id>` 命令。

### `POST /api/stt/test`

使用给定的 `sttKey` / `sttUrl` 向 MiMo STT 端点发送一个最小请求
（省略的字段回退到存储值/默认值）。响应 JSON：`ok`、
`accepted`（任何非 401/403 状态都表示密钥被接受）、`httpStatus`、
`stage`、`detail`、`ms`。

## WebSocket 上传

### 端口 81

WebSocket 路径用于来自文件管理器和 Calibre 插件工作流的快速二进制
上传。

连接：

```text
ws://crosspoint.local:81/
```

协议：

1. 客户端发送文本：`START:<filename>:<size>:<path>`
2. 服务器回复 `READY`
3. 客户端发送二进制分块
4. 服务器每 64 KB 或完成时发送 `PROGRESS:<received>:<total>`
5. 完成时服务器发送 `DONE`，失败时发送 `ERROR:<message>`

示例会话：

```text
Client -> START:mybook.epub:1234567:/Books
Server -> READY
Client -> [binary chunk]
Server -> PROGRESS:65536:1234567
...
Server -> DONE
```

错误消息包括：

| 消息 | 原因 |
|------|------|
| `ERROR:Upload already in progress` | 第一次上传尚未完成就启动了第二次上传 |
| `ERROR:Invalid START format` | START 消息格式错误或 size 令牌无效 |
| `ERROR:Failed to create file` | 无法打开目标文件 |
| `ERROR:No upload in progress` | 收到二进制数据但没有匹配的 START |
| `ERROR:Upload overflow` | 客户端发送的字节数超过声明值 |
| `ERROR:Write failed - disk full?` | SD 写入失败 |

未完成的 WebSocket 上传会在断开连接或出错时被删除。

## WebDAV

同一 HTTP 服务器为文件管理器客户端注册了 WebDAV 兼容的处理程序。

支持的方法：

```text
OPTIONS, GET, HEAD, PUT, DELETE, PROPFIND, MKCOL, MOVE, COPY, LOCK, UNLOCK
```

注意：

- `PUT` 先写入临时的 `.davtmp` 文件，然后重命名到目标位置。
- 受保护的路径会被拒绝。
- `LOCK` 和 `UNLOCK` 仅为客户端兼容性而被接受。服务器未实现完整的
  WebDAV Class 2 锁定语义，例如持久锁或锁发现。

## UDP 发现

服务器监听 UDP 端口 `8134`。当它收到文本负载
`hello` 时，会向发送方回复：

```text
crosspoint (on <hostname>);81
```

最后一个字段是 WebSocket 上传端口。

## 网络模式

### 站点模式 (STA)

- 设备加入现有的 2.4 GHz Wi-Fi 网络。
- 可用时通过 mDNS 广播 `crosspoint.local`。
- `/api/status` 返回 `"mode": "STA"` 和 dBm 表示的 RSSI。

### 接入点模式 (AP)

- 设备创建一个名为 `CrossPoint-Reader` 的开放热点。
- 设备显示 Wi-Fi 二维码和 URL 二维码。
- 备用 IP 通常为 `192.168.4.1`。
- `/api/status` 返回 `"mode": "AP"` 和 `"rssi": 0`。

### Calibre 无线

Calibre 无线在 STA 模式下启动相同的 web 服务器，并在设备屏幕上
显示设置说明以及 WebSocket 上传进度。