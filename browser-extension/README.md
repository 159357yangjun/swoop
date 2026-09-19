# IDM Next 浏览器扩展

MV3 扩展，嗅探网页中的视频/音频/文件下载链接，一键发送到 IDM Next 桌面端下载。

## 功能

- **资源嗅探**：通过 `webRequest` API 自动捕获页面中的媒体请求（mp4/mkv/mp3/flac/zip/exe 等）
- **DOM 扫描**：内容脚本扫描 `<video>`、`<audio>`、`<a download>` 元素及直链
- **一键下载**：popup 弹窗展示嗅探到的资源，点击即可发送到 IDM Next
- **悬浮下载按钮**：鼠标悬停网页视频时，右上角浮现「下载此视频」按钮（IDM 同款体验）
- **本页资源面板**：右下角悬浮球进入批量勾选面板，带搜索过滤、全选、按类型图标分列，
  可整批「下载」或「加入队列」（`content.js` 自动在嗅探到非图片资源时出现）
- **页面内提示**：下载成功/失败以浮动 toast 直接显示在**视频下方**（而非仅弹窗内），来自 popup 与右键菜单的下载同样会落到页面
- **Native Messaging**：通过原生 C++ Host（idm-next-host.exe）直连运行中的 IDM Next GUI，无需 Python、也不每条下载拉起子进程

## 界面

三处界面（桌面端 `resources/qss/`、popup、页面内注入浮层）共用**同一套设计语言**：
中性灰底 + 单一克制强调色 + 6~8px 小圆角 + 扁平控件；图标一律为内联 SVG 线性描边。

- **禁用**：蓝紫/粉蓝渐变、毛玻璃 `backdrop-filter`、emoji 当图标（🎬🎵📄 在不同系统上字形不一致）、大于 12px 的大圆角。
- **主题**：`storage.local.uiTheme = auto | light | dark`。popup 顶部按钮切换，**页面内浮层读同一个键**，
  所以在弹窗里切成暗色，网页上的悬浮球/面板/toast 会一起变暗。`auto` 跟随 `prefers-color-scheme`。
- **改完必须量**：`bash tools/ext_shot/run.sh` 会起 headless Edge 加载真实 `popup.html`，
  并把 `content.js` 引入 `tools/ext_shot/content-harness.html` 仿真页跑一遍，
  13 个场景出图到 `build/ext-shots/`，同时断言主题令牌、行数/计数、布局零溢出、图标 `getBBox()` 非空。

## 架构

```
浏览器扩展                     Native Messaging Host           IDM Next GUI
┌─────────────┐               ┌──────────────────┐  QLocal   ┌────────────┐
│ background   │  sendNative   │ idm-next-host.exe│  Server   │ MainWindow │
│ (sniff)      │ ───────────→  │ (stdio JSON)     │ ─────────→│ (idm-next- │
│              │  Message      │  直连 GUI        │  idm-next │  ipc)      │
│ popup        │ ←───────────  │  回传结果        │  -ipc     │ 添加任务   │
│ (list/send)  │               │                  │           │            │
└─────────────┘               └──────────────────┘           └────────────┘
                                          │
                              GUI 未运行时回退：idm-next --cli add（无界面 CLI）
```

> 说明：host 用 C++ 实现（源码 `src/host/idm_next_host.cpp`，CMake 目标 `idm-next-host`），
> 取代早期 Python 脚本，去掉 Python 依赖，下载请求直接转发给 GUI，并读取 GUI 真实返回结果。

## 安装步骤

### 1. 加载扩展（开发者模式）

1. 打开 Chrome / Edge，地址栏输入 `chrome://extensions`
2. 右上角开启「开发者模式」
3. 点击「加载已解压的扩展程序」，选择 `browser-extension/` 目录
4. 扩展安装后，记下扩展 ID（在扩展卡片上显示，形如 `abcdefghijklmnop...`）

### 2. 配置 Native Messaging Host

#### Windows

1. 确认 `com.tencent.idm_next.json` 的 `path` 指向 `idm-next-host.exe`（运行 `install_host.ps1` 会自动改为同目录的 host exe）
   - 扩展 ID 已通过 manifest 的 `key` 固定为 `fmnehlhldcoknecihnlbbcmhpaaapmbp`，`allowed_origins` 已预填，**无需手动替换**
2. **以管理员或当前用户打开 PowerShell**，进入 `native-messaging-host/` 目录，运行注册脚本：
   ```powershell
   cd "D:\visual studio\lianxi\MFC\idm-next\browser-extension\native-messaging-host"
   .\install_host.ps1
   ```
   脚本会为 Chrome 与 Edge 写入注册表，指向本目录下的 manifest（HKCU，无需管理员）
3. 到 `chrome://extensions` 找到「IDM Next 嗅探器」点击刷新图标，使注册生效

> 若仍报 `Access to the specified native messaging host is forbidden`：
> 多半是注册表指向旧的 manifest 或扩展 ID 不匹配。重新运行 `install_host.ps1` 并刷新扩展即可。

#### macOS / Linux

1. 将 manifest JSON 复制到：
   - macOS: `/Library/Google/Chrome/NativeMessagingHosts/com.tencent.idm_next.json`
   - Linux: `~/.config/google-chrome/NativeMessagingHosts/com.tencent.idm_next.json`
2. 编辑 manifest 中的 `path`，指向 `idm_next_host.py` 的绝对路径
3. 确保 `idm_next_host.py` 有执行权限：`chmod +x idm_next_host.py`
4. 编辑 manifest 中的 `allowed_origins`，替换扩展 ID

### 3. 验证

1. 打开任意视频网站，播放视频
2. 点击浏览器工具栏的 IDM Next 图标
3. popup 中应显示嗅探到的资源列表
4. 点击「下载」，IDM Next 应自动添加任务

## 文件结构

```
browser-extension/
├── manifest.json                          # MV3 清单
├── background.js                          # Service Worker（webRequest 嗅探 + Native Messaging）
├── content.js                             # 内容脚本（DOM 扫描媒体元素）
├── popup.html                             # 弹窗 UI
├── popup.css                              # 弹窗样式
├── popup.js                               # 弹窗逻辑
├── icons/                                 # 扩展图标（需自行放置 16/48/128px PNG）
└── native-messaging-host/
    ├── com.tencent.idm_next.json          # Native Messaging manifest（path 指向 idm-next-host.exe）
    └── install_host.ps1                   # 注册脚本（自动把 path 改为同目录的 host exe）
```

> 原生 Host 由主工程编译产出 `idm-next-host.exe`（见根目录 `CMakeLists.txt` 的
> `idm-next-host` 目标），与 `idm-next.exe` 一同随安装包分发；不再需要 Python 运行时。
> 早期 Python 版 `idm_next_host.py` / `idm_next_host.bat` 已弃用。

## 支持的资源类型

| 类型 | 扩展名 |
|------|--------|
| 视频 | mp4 mkv avi mov wmv flv webm m4v mpg mpeg ts m2ts |
| 音频 | mp3 flac aac ogg wav m4a wma opus |
| 压缩 | zip rar 7z tar gz bz2 xz iso |
| 程序 | exe msi apk dmg pkg deb rpm |
| 文档 | pdf doc docx xls xlsx ppt pptx epub |
| 字幕 | srt ass ssa vtt sub |
| 协议 | magnet: ed2k: thunder: .torrent |
