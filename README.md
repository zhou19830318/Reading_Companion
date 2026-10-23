# OnePage Reader — 固件

> **基于 [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)**（MIT 许可 © Dave Allie）。
> 本项目是 CrossPoint 移植到自研 **OnePage** 电子书阅读器（**ESP32-C61**）的**独立移植版**。
> 上游面向 ESP32-C3 的 Xteink X4/X3；本仓库将固件适配到 OnePage C61 硬件。
> 与 CrossPoint 项目及 Xteink 均无关联。

CrossPoint 是开源电子书阅读器固件——社区共建、完全可折腾、永久免费。本移植保留其引擎核心，并适配 OnePage 设备。

**运行于：** 自研 **OnePage** 阅读器（ESP32-C61）。OnePage 开源硬件项目的一部分——固件 · 板卡（PCB）· 3D 外壳 · 网站；硬件与 3D 文件与固件一同包含在该项目中。

## 相对上游 CrossPoint 的新增 / 改动

- **ESP32-C61 支持** —— 新增 `onepage` PlatformIO 环境与板卡定义、内嵌板卡 SDK（`open-onepage-sdk`，非子模块）、核心与 HAL 各处的 `#ifdef ONEPAGE_C61` 保护。
- **简体与繁体中文界面** —— 新增 i18n 翻译与 SD 卡 CJK 字体回退。
- **AI 语音助手** —— 按住说话、对接自建网关的智能体对话：语音经内置 **VoiceRelay** 服务器流转，智能体对话走 **OpenClaw**。设备端聊天历史、设备审批流程与云端 API 密钥只存放在你自己的服务器上。
- **AI 生图** —— 设备端文生图：生成、浏览设备端历史画廊、删除图片、或设为书籍封面——由 relay 的 t2i 提供方驱动。
- **离线词典查询** —— 不离开书籍即可查词，由 relay 的离线词典卡（`relay/dict-data/`，无云端调用）提供。
- **自建 VoiceRelay 服务器** —— 仓库自带 FastAPI 服务（`relay/`）：实时语音识别、流式 LLM 与图片生成统一在一个 API 之下，附 systemd/nginx 部署脚本、测试与离线词典数据。
- **工作台仪表盘** —— 与网关同步的卡片（待办、天气、正在阅读、网关在线时长）渲染在墨水屏上。
- **重新设计的 Web 界面** —— 四个浏览器页面（首页 / 文件 / 设置 / 字体）共用一套设计系统，完全双语（中文 / EN），设置按类别分栏，并新增保存 / 恢复默认 / 重启控件，以及管理网关与 STT 密钥的云服务面板（存于 NVS，绝不回显）。
- **蓝牙翻页器** —— BLE HID 主机，可学习任意遥控器的按键。
- **功耗优化** —— BLE 活跃空闲电流 42 → 23 mA（控制器 modem-sleep + 32.768 kHz 睡眠时钟 + CPU DFS）。
- **80 MHz flash 时钟**、OnePage 开机 Logo、阅读页横线排版选项。

下文列出的阅读引擎功能继承自 CrossPoint。


## CrossPoint 能做什么？

- **阅读引擎**：EPUB 2/3 渲染（含嵌入样式选项）、图片处理、断字连字、字距调整、章节导航、脚注、书签、跳转到百分比、自动翻页、方向控制、专注阅读、KOReader 进度同步等。

- **多种格式**：原生支持 `.epub`、`.xtc/.xtch`、`.txt` 与 `.bmp`。

- **截图**（OnePage 上为 电源键 + 下键）。

- **自定义字体**：把喜欢的字体装到 SD 卡，包括 CJK 字族。

- **倾斜翻页** —— 仅上游 X3 硬件支持；OnePage 板没有 IMU，本移植保留其代码路径可编译但不启用。

- **书库工作流**：文件夹浏览、隐藏文件开关、长按删除、最近阅读、SD 缓存管理。

- **无线工作流**：

  - 文件传输 Web 界面
  - EPUB 优化器
  - Web 设置界面 / API（在浏览器中修改大量设备设置）
  - WebSocket 快速上传
  - WebDAV 处理器
  - AP 模式（热点）与 STA 模式（加入现有 Wi-Fi），均带二维码助手
  - Calibre 无线连接流程
  - OPDS 浏览器：保存服务器（最多 8 个）、搜索、分页、直接下载
  - Wi-Fi 凭据、OPDS 服务器与云服务密钥均在浏览器中管理（密钥存于设备、绝不回传给界面）
  - 从 GitHub Releases 检查并安装 OTA 更新 —— ⚠️ 本移植仍在轮询**上游 CrossPoint** 的 Releases，其 `firmware.bin` 是 ESP32-C3 镜像。**切勿**在 OnePage 上使用 **设置 → 系统 → 检查更新**；请用 USB 烧录（见[安装固件](#安装固件)）。

- **个性化**：多套主题（Classic、Lyra、Lyra Extended、RoundedRaff）、睡眠屏幕模式、前 / 侧键重映射、状态栏控制、电源键行为、刷新节奏等。

- **AI 工具**：语音助手、带设备端历史的文生图、阅读内词典查询与预设提问快捷方式——全部由自建的 [VoiceRelay](./relay/README.md) / OpenClaw 网关支撑。

- **本地化**：28 种界面语言（含简体与繁体中文），支持 RTL（希伯来语）。Web 界面单独双语（中文 / EN），不依赖设备语言。

### 即将推出：

- 更多主题。

- 更多内容！敬请期待。

---

## USB 锁定设备（Xteink Unlocker）

> **与 OnePage 无关。** 本节描述 Xteink 品牌设备。OnePage 板为开源硬件，出厂无 USB 锁——直接用 `esptool` 或 `pio run -e onepage -t upload` 烧录即可。此处仅作参考保留。

<details>
<summary>Xteink USB 锁背景（上游 X3/X4 硬件）</summary>

部分从第三方店铺（如 AliExpress）购买的 Xteink 设备出厂即锁定 USB 烧录。
若你的设备被锁定，需先使用 https://crosspointreader.com/#unlock-tool 提供的 **Xteink Unlocker** 工具，
然后才能刷入 CrossPoint。

**如果你直接从 xteink.com 购买，则无需此工具**，那些设备没有锁。

**不确定设备是否锁定？** 开机、接上 USB-C 线，先尝试网页烧录器（见下文[安装固件](#安装固件)）。
若浏览器的串口设备选择器没有出现你的设备，先换 USB 口或浏览器再下结论；
只有当设备仍不出现时，才考虑使用解锁器。

> ### ⚠️ 警告：使用解锁器前必读 ⚠️
>
> **解锁工具官方支持的固件只有 CrossPoint 与 CrossInk。**
>
> 在 USB 锁定设备上刷入其他任何固件，可能**永久变砖**，或**永久卡在该固件上且无法恢复**。
> 一旦 USB 烧录被重新锁定，唯一的出路是 OTA——若所刷固件不支持 OTA，**将无路可走**。
>
> **Papyrix 分支已从代码中移除 OTA 更新支持。** 若在 USB 锁定设备上刷入 Papyrix，
> 你将**没有任何更新或恢复途径**并被永远卡在它上面。**切勿在锁定设备上刷入 Papyrix（或其他不支持的固件）。**

</details>

## 安装固件

> **OnePage（C61）提示：** crosspointreader.com 上的网页安装器只提供上游 **X3/X4（ESP32-C3）** 的构建——**不能**刷 OnePage。
> 请自行构建 `firmware.bin`（见[开发快速上手](#开发快速上手)），再用下方命令行烧录。
> 后文的网页安装器章节仅为上游 X 系列流程的参考。

### 命令行（OnePage / ESP32-C61）

1. 安装 [`esptool`](https://github.com/espressif/esptool)（`pip install esptool`）。
2. 自行构建（`~/.platformio/penv/bin/pio run -e onepage`）或从本仓库的 [Releases](https://github.com/zhou19830318/Reading_Companion/releases) 页面下载 `onepage-firmware-*.bin`。
3. 用 USB-C 连接；找到串口（macOS 上为 `ls /dev/cu.usbmodem*`）。
4. **烧录方式取决于板载 SPI flash 厂商** —— 先探测：`esptool --chip esp32c61 -p <port> flash-id`，读取 Manufacturer 字节：
   - **Winbond**（`ef`）：支持 stub + 压缩，较快（约 95 秒）：
     ```bash
     esptool --chip esp32c61 -p <port> -b 921600 write-flash \
       --flash-mode dio --flash-freq 80m --flash-size 16MB 0x10000 firmware.bin
     ```
   - **Puya** PY25Q128HA（`85`）：stub 压缩会崩溃 —— **必须**加 `--no-stub`（约 380 秒）：
     ```bash
     esptool --chip esp32c61 --no-stub -p <port> -b 115200 write-flash \
       --flash-mode dio --flash-freq 80m --flash-size 16MB 0x10000 firmware.bin
     ```
   成功与否以输出中的 "Hash of data verified" 行为准，不要依据 esptool 的退出码判断。

<details>
<summary>上游 X3/X4（ESP32-C3）安装流程 —— 参考</summary>

### 网页安装器（推荐）

1. 用 USB-C 将设备连到电脑并唤醒 / 解锁设备
2. 打开 https://crosspointreader.com/#flash-tools，选择设备（X3 或 X4），挑选官方 CrossPoint 发布版本

### 网页安装器（指定版本）

1. 用 USB-C 将设备连到电脑并唤醒 / 解锁设备
2. 从 [Releases](https://github.com/crosspoint-reader/crosspoint-reader/releases)、本地构建或 CI 产物中下载 `firmware.bin`。
3. 打开 https://crosspointreader.com/#flash-tools，选择设备（X3 或 X4），点击 "Custom .bin" 并上传 `firmware.bin`。

### 恢复官方固件

要恢复官方固件，也可以通过 https://crosspointreader.com/#flash-tools 刷入最新官方固件。

### 命令行

1. 安装 [`esptool`](https://github.com/espressif/esptool)：

```bash
pip install esptool
```

2. 从 [releases 页面](https://github.com/crosspoint-reader/crosspoint-reader/releases) 下载 `firmware.bin`。
3. 用 USB-C 连接设备。
4. 找到设备串口。Linux 上连接后执行 `dmesg`；macOS 上：

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. 烧录：

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/firmware.bin
```

按你的系统实际情况修改 `/dev/ttyACM0`。

### 手动方式

参见下文[开发快速上手](#开发快速上手)。

</details>

---

## 自定义 SD 卡字体

把你自己的 TTF/OTF 文件转换成从 SD 卡加载的 `.cpfont` 文件，无需重刷固件。

1. 打开 https://crosspointreader.com/fonts，进入「SD-card font builder」表单。
2. 最多上传四种样式（常规、粗体、斜体、粗斜体），设置字族名、磅值与 Unicode 范围。
3. 下载生成的 `.cpfont` 文件。
4. 复制到 SD 卡的 `/fonts/YourFont/`（或 `/.fonts/YourFont/` 以隐藏该文件夹）。
5. 在设备的字体设置中选择该字体。

转换过程原样调用固件仓库的 `lib/EpdFont/scripts/fontconvert_sdcard.py` 脚本，因此产物与本地主机构建完全一致。

---

## 文档

完整索引：[docs/README.md](./docs/README.md)。

**使用**

- [用户指南](./USER_GUIDE.md)
- [VoiceRelay 服务器搭建与协议](./relay/README.md)
- [VoiceRelay 部署手册（安装 / 运维）](./relay/DEPLOY.md)
- [Web 服务器使用](./docs/webserver.md)
- [Web 服务器端点](./docs/webserver-endpoints.md)
- [SD 卡字体](./docs/sd-card-fonts.md)
- [故障排查](./docs/troubleshooting.md)

**架构与模块**

- [Activity 管理器](./docs/activity-manager.md) —— 页面栈与生命周期
- [文件格式](./docs/file-formats.md) —— 书籍 / 缓存的磁盘布局
- [i18n](./docs/i18n.md) —— 翻译与 CJK 字体覆盖
- [专注阅读](./docs/focus-reading.md)
- [翻页性能基线](./docs/reading-page-turn-baseline.md)

**计划与需求**

- [v4.0 开发计划](./docs/v4.0-development-plan.md)
- [产品需求文档（PRD）](<./docs/OnePage_additional%20_PRD.md>)
- [语音助手 / OpenClaw 移植计划](./docs/voice-openclaw-port-plan.md)
- [项目范围](./SCOPE.md)

**贡献**

- [贡献者文档](./docs/contributing/README.md)
- [翻译指南](./docs/translators.md)

---

## 开发快速上手

### 前置条件

- [pioarduino](https://github.com/pioarduino/pioarduino)，或 VS Code + pioarduino 插件
- Python 3.8+
- `clang-format` 21
- 支持数据传输的 USB-C 线

### 环境搭建

```bash
git clone https://github.com/zhou19830318/Reading_Companion.git
cd Reading_Companion
```

无需子模块：板卡 SDK（`open-onepage-sdk/` —— EInkDisplay、InputManager、
BatteryMonitor、SDCardManager）已内嵌仓库，并通过 `platformio.ini`
中的 `symlink://` 条目接入。

### 构建 / 烧录 / 监控

```bash
# pio 需运行在 Python 3.10+ 上；若你的 `pio` 用的是旧版 Python，请用 penv 里的那个：
~/.platformio/penv/bin/pio run -e onepage            # 构建（C61）
~/.platformio/penv/bin/pio run -e onepage -t upload  # 构建 + 烧录
```

Web 页面是生成产物 —— 编辑 `src/network/html/` 下的任何内容后：

```bash
python3 scripts/sync_web_core.py   # 把共享设计系统的 CSS/JS 重新注入全部 4 个页面
python3 scripts/build_html.py      # 压缩 + gzip + 生成 src/network/html/*.generated.h
```

`sync_web_core.py --check` 会在页面与共享核心不一致时失败，因此也可当作门禁使用。

要从上游 CrossPoint 拉取个别改进（本仓库是独立移植，与上游无共享 git 历史——不要 `git merge` 上游）：

```bash
git remote add upstream https://github.com/crosspoint-reader/crosspoint-reader.git
git fetch upstream
git cherry-pick <commit>   # 挑选与板卡无关的阅读引擎修复
```

### 贡献者 PR 前检查

```bash
./bin/clang-format-fix                # 格式化代码树（clang-format 21）
python3 scripts/sync_web_core.py --check   # 共享 Web CSS/JS 漂移门禁
python3 scripts/build_html.py         # 重新生成内联的 Web 页面
pio run -e onepage                    # C61 目标必须构建无警告错误
```

### 调试

刷入新功能后，建议从串口采集详细日志。

首先确认所需 Python 依赖已安装：

```python
python3 -m pip install pyserial colorama matplotlib
```

然后运行脚本：

```sh
# Linux
# 在 Debian 上测试通过，多数 Linux 系统应可直接使用。
python3 scripts/debugging_monitor.py

# macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Windows 上可能需要少量调整。

---

## 仓库结构

| 路径 | 内容 |
|------|------|
| `src/` | 固件入口（`main.cpp`）、设置 / 状态存储、逻辑→物理按键映射、Web 服务器、OPDS 存储 |
| `src/activities/` | 每个界面对应一个子包，由 Activity 生命周期驱动（`onEnter` → `loop` → `onExit`）；Activity 堆上分配、退出即删除 |
| `lib/Epub`、`lib/GfxRenderer`、`lib/ZipFile`、`lib/InflateReader`、`lib/uzlib` | 阅读引擎：EPUB 解析、布局缓存、墨水屏渲染、归档 / 解压 |
| `lib/hal` | 硬件抽象 —— `HalDisplay` / `HalGPIO` / `HalStorage`；**所有** SD 访问都经由 `HalStorage`（SdFat 非线程安全） |
| `lib/EpdFont`、`lib/I18n`、`lib/Logging`、`lib/Memory` | 字体、翻译、日志、分配辅助（`makeUniqueNoThrow`、PSRAM 分配器） |
| `lib/OpenClaw`、`lib/VoiceRelay`、`lib/voice`、`lib/NetBootstrap`、`lib/OpdsParser` | 语音助手与 AI 客户端（OpenClaw 智能体对话、VoiceRelay 语音 / 对话 / 图片协议、STT）、网关与网络辅助 |
| `open-onepage-sdk/` | 内嵌的 OnePage C61 板卡 SDK（EInkDisplay、InputManager、BatteryMonitor、SDCardManager）—— 非子模块，经 `platformio.ini` 的 `symlink://` 条目接入 |
| `boards/`、`partitions.csv`、`sdkconfig.onepage` | C61 目标的 PlatformIO 板卡定义与 flash / PSRAM 配置 |
| `scripts/` | 构建辅助 —— `build_html.py`、`sync_web_core.py`、`gen_i18n.py`、`debugging_monitor.py` |
| `src/network/html/` | Web 界面源页面；`*.generated.h` 产物已 gitignore，每次构建重新生成 |
| `relay/` | 自建 **VoiceRelay** FastAPI 服务器 —— 实时 ASR、流式 LLM、图片生成（`relay/relay/`）、离线词典数据（`dict-data/`）、部署脚本与测试 |
| `docs/` | 完整文档索引（[docs/README.md](./docs/README.md)） |
| `test/`、`simulator/` | 可在主机运行的单元测试与 WASM/Arduino 主机模拟器 |

克隆与构建无需额外步骤：`.pio/`、`*.generated.h`、生成的 i18n 头文件以及 `lib/EpdFont/fontsrc` 下的原始字体均已 gitignore，由构建重新生成。

---

## 内部机制

CrossPoint Reader 在把数据缓存到 SD 卡以最小化 RAM 占用方面非常彻底。ESP32-C3 只有约 380KB 可用 RAM，必须精打细算；固件设计中的许多决策都源于这一约束。

**OnePage 说明：** C61 目标保持了这套 SD 优先设计，但本板还带来了四线 PSRAM（40 MHz，`CONFIG_SPIRAM=y`），因此少数大缓冲——例如语音助手的聊天缓冲——特意从 `MALLOC_CAP_SPIRAM` 分配。该构建上 `ESP.getPsramSize()` 汇报不准（`psramFound()` 返回 false 而 SPIRAM 堆实际在用），所以周期性的 `MEM` 日志行改用 `heap_caps_*`。

### 数据缓存

书籍章节首次加载时会缓存到 SD 卡，之后的加载直接命中缓存。缓存目录位于 SD 卡的 `.crosspoint`，结构如下：

```text
.crosspoint/
├── epub_<hash>/         # 每本书一个目录，按内容哈希命名
│   ├── progress.bin     # 阅读位置（章节、页等）
│   ├── cover.bmp        # 生成的封面图
│   ├── book.bin         # 元数据：标题、作者、spine、目录
│   ├── css_rules.cache  # 已解析 CSS 规则缓存
│   ├── img_*            # 渲染后的图片缓存文件
│   └── sections/        # 按章节的布局缓存
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # 设备设置
├── state.json           # 恢复 / 运行时状态
└── recent.json          # 最近阅读列表
```

删除 `/.crosspoint` 会清空全部缓存元数据，下次打开时强制完整重建。通过固件或 Web 界面进行的书籍删除、覆盖与移动会同步清理或重键匹配缓存；手动改动 SD 卡可能留下过期缓存目录。

更多内部文件结构细节见[文件格式文档](./docs/file-formats.md)。

---

## 贡献

欢迎贡献。如果你刚接触代码库，先看[贡献者文档](./docs/contributing/README.md)。想找可做的事项，看看[想法讨论区](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas)——动手前请先留言，避免重复劳动。

这里人人都是志愿者，请保持尊重与耐心。治理与社区规范见 [GOVERNANCE.md](./GOVERNANCE.md)。

---

## 社区分支

开源最棒的一点是任何人都能把代码带向不同的方向。如果你需要的东西超出 CrossPoint 的[范围](./SCOPE.md)，可以看看这些社区分支：

- [CrossInk](https://github.com/uxjulia/CrossInk) —— 排版与阅读追踪：Bionic Reading（加粗词干形成注视点）、词间引导点、改进的段首缩进，并以 ChareInk/Lexend/Bitter 替换默认字体。

- [papyrix-reader](https://github.com/bigbag/papyrix-reader) —— 增加 FB2 与 MD 格式支持，积极维护并支持阿拉伯文，可通过 SD 卡自定义主题。

- [crosspet](https://github.com/trilwu/crosspet) —— 越南语分支，加入一只随你阅读里程碑（页数、连续天数、照料）成长的电子鸡。另有抽认卡、天气、番茄钟与小游戏。

- [crosspoint-reader-cjk](https://github.com/aBER0724/crosspoint-reader-cjk) —— 面向中日韩阅读专门打造。

- [inx](https://github.com/obijuankenobiii/inx) —— 以标签页导航彻底重新设计的界面。

- ~~[PlusPoint](https://github.com/ngxson/pluspoint-reader) —— 自定义 JS 应用支持。~~（已停止维护）

- [crosspoint-reader-papers3](https://github.com/juicecultus/crosspoint-reader-papers3) —— CrossPoint 移植到 M5Stack Paper S3。

- [t5s3-reader](https://github.com/ShallowGreen123/t5s3-reader) —— CrossPoint 移植到 LilyGo T5 ePaper S3 / T5S3 4.7 英寸墨水屏设备。

**注：** 这些功能许多会随时间进入 CrossPoint。我们节奏较慢，以确保绝对稳定、把 bug 扼杀在到达你的设备之前。

想自己造一台设备？别忘了看看 [de-link](https://github.com/iandchasse/de-link) 项目。

---

CrossPoint Reader **与 Xteink 或任何设备制造商均无关联**。

特别致谢 [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader)，它启发了本项目。
