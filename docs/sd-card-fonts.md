# SD 卡字体

CrossPoint 支持从 SD 卡加载额外字体，包括具有扩展 Unicode 覆盖范围（CJK、西里尔字母、希腊字母等）的字体。

## 安装字体

有三种安装字体的方式：

### 方式一：从设备下载（推荐）

1. 将你的 CrossPoint 阅读器连接到 Wi-Fi
2. 进入 **设置 > 系统 > 管理字体**
3. 浏览可用的字体家族并点按下载
4. 下载的字体会立即出现在 **设置 > 阅读器 > 字体家族** 中

### 方式二：通过网络浏览器上传

1. 启动 **文件传输**，并通过 **加入网络** 或 **创建热点** 连接
2. 打开阅读器上显示的 Web 界面 URL
3. 导航到 **字体** 标签页
4. 使用上传表单上传 `.cpfont` 文件

### 方式三：手动复制到 SD 卡

1. 从 [crosspoint-fonts 仓库](https://github.com/crosspoint-reader/crosspoint-fonts) 下载字体文件
2. 将字体家族文件夹复制到 SD 卡上的两个位置之一：

   - `/.fonts/` —— 隐藏目录（首选；在桌面挂载时可保持 SD 卡根目录整洁）
   - `/fonts/` —— 可见目录（如果你的操作系统隐藏点文件、而你更希望在文件管理器中看到该文件夹，请使用此目录）

   两个根目录在启动时都会被扫描，结果会合并：安装在 `/fonts/` 中的字体家族即使 `/.fonts/` 同样存在也会显示，反之亦然。只有当同名字体家族同时出现在两个根目录中时才会冲突——此时 `/.fonts/` 中的副本优先，`/fonts/` 中的重复项会被忽略。

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. 插入 SD 卡并开启 CrossPoint 阅读器电源

## 可用的预构建字体

当前的预构建字体列表维护在 [crosspoint-fonts 仓库](https://github.com/crosspoint-reader/crosspoint-fonts) 中。

## 转换自定义字体

要转换你自己的 TrueType/OpenType 字体：

### 前提条件

    pip install freetype-py fonttools

### 单字体（单一样式）

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### 多样式字体

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### 可用的 Unicode 区间预设

| 预设 | 覆盖范围 |
|--------|----------|
| `ascii` | U+0020–U+007E（基本拉丁字母） |
| `latin1` | U+0080–U+00FF（Latin-1 补充） |
| `latin-ext` | 欧洲语言（拉丁字母 + Extended-A/B + 标点 + 连字） |
| `greek` | 希腊字母 + 扩展希腊字母 |
| `cyrillic` | 西里尔字母 + 补充 |
| `hebrew` | 希伯来字母 + 字母表现形式 |
| `georgian` | 格鲁吉亚字母 + 格鲁吉亚补充 |
| `armenian` | 亚美尼亚字母 |
| `ethiopic` | 埃塞俄比亚字母 + 扩展 |
| `vietnamese` | 越南语子集（ơ/ư 和组合符号） |
| `punctuation` | 通用标点（U+2000–U+206F） |
| `cjk` | CJK 统一表意文字 + 平假名 + 片假名 + 全角字符 |
| `hangul` | 韩文谚文音节 + Jamo + 兼容 Jamo |
| `cherokee` | 切罗基字母（历史字符 + 补充区段） |
| `tifinagh` | 提非纳字母 |
| `symbols` | 数学、货币、箭头、制表符、杂项符号、装饰符号 |
| `reading` | 文学类书籍覆盖：拉丁字母、希腊字母、西里尔字母、数学/符号区段、补充标点以及 CJK 引号 |
| `builtin` | 与固件内置的字体转换区间一致 |

用逗号组合预设：`--intervals latin-ext,greek,cyrillic`

你也可以直接指定任意 Unicode 范围：
`--intervals latin-ext,(0x2100-0x214F)`

要列出所有预设及其码点数量：

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### 附加选项

`--force-autohint` —— 强制使用 FreeType 的自动微调器，而不是字体自带的原生微调（当字体内置微调在小字号下效果不佳时很有用）。

通过 Web 界面或手动复制到 SD 卡来安装自定义字体。
