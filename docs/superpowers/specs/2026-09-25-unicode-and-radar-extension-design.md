# Swoop Unicode GUI and Radar Extension Design

## Goal

修复 Windows GUI 菜单的中文乱码，并把现有只有后台拦截逻辑的浏览器扩展升级为 A 方案的 Radar 弹窗：显示宿主连接状态、扫描当前页面资源、列出最近任务，并通过 native messaging 把用户选择的资源交给 Swoop。

## Scope

### Included

- `src/gui/resources.rc` 的 UTF-8 资源编译声明，保证菜单和对话框中文按 UTF-8 解释。
- Makefile 的资源编译参数，避免本机和 CI 使用不同默认代码页。
- MV3 扩展弹窗：连接状态、扫描当前页面、最近两条任务、打开主程序。
- 当前页面资源扫描：链接、`video`、`audio`、`source` 元素；按下载文件后缀和 MIME 提示去重。
- Popup/页面脚本/后台脚本复用现有 `{action:"download",url,filename,referer}` native messaging 协议。
- 宿主不可用时的可见错误提示；自动下载接管失败时继续保留浏览器下载。
- 发布包收录新增扩展文件。
- README 的扩展安装和 Radar 使用说明。

### Not included

- 不做网络层抓包、登录态资源解密或视频流合并。
- 不改 Swoop C 下载引擎、任务持久化格式或 native host 的下载协议。
- 不做独立的前端构建工具链；扩展使用原生 HTML/CSS/JavaScript。

## Architecture

### GUI encoding

资源脚本显式声明 UTF-8，并在 `windres` 调用中显式传递 UTF-8 code page。C 源文件继续使用现有 Unicode Win32 API；资源编译只修复 `.rc` 窄字符串被按 CP1252 解码的问题。

### Extension components

- `manifest.json`: 声明 popup、content script、`scripting`/`storage` 权限和已有下载/native messaging/context menu 权限。
- `popup.html`: Radar 结构和可访问的按钮、状态区域、任务列表。
- `popup.css`: Swoop 的深蓝、酸橙绿、暖白配色，窄弹窗布局和状态/错误样式。
- `popup.js`: ping 宿主、请求当前页扫描、提交选中资源、显示最近任务。
- `content.js`: 在当前页面提取唯一候选下载资源，返回 URL、文件名、页面标题和资源类型。
- `background.js`: 保留现有自动下载和右键菜单逻辑，新增 popup/content message 路由、native host ping、最近任务缓存。
- `icons/`: 使用轻量 SVG/PNG 图标，保持浏览器工具栏和弹窗品牌一致。

### Data flow

1. Popup 请求 `chrome.tabs.query({active:true,currentWindow:true})`，向当前页 content script 发送 `scan`。
2. Content script 返回候选 URL 列表，popup 去重并渲染。
3. 用户点击候选项，popup 发送 `{action:"download",url,filename,referer}` 给 background。
4. Background 调用现有 `sendToHost`；成功后记录最近任务并返回结果，失败则显示“请注册/启动宿主”的可操作提示。
5. 已有 `downloads.onCreated` 和 context menu 逻辑继续走同一 native messaging 路径。

## Error handling

- `chrome.runtime.lastError` 转换为用户可读状态：“宿主未连接”；不吞掉错误。
- Content script 无法注入到 Chrome 内置页、扩展页或受限页面时，显示“此页面不允许扫描”，右键/自动下载接管仍可用。
- 资源 URL 只接受 `http:`/`https:`，过滤 `blob:`, `data:`, `filesystem:` 和重复 URL。
- Native host 失败时不取消原浏览器下载。

## Testing and acceptance

- 资源编译回归：`make clean && make all` 后运行自测，检查菜单资源字符串不再按 CP1252 产生 mojibake。
- Extension static checks：解析 manifest JSON、检查 popup/content/background 的语法，确认 manifest 引用的文件全部存在。
- Native messaging regression：现有 `swoop_nmhost.exe --selftest` 必须继续通过。
- Manual browser acceptance：加载解压扩展后，popup 能显示宿主状态；扫描测试页面能列出资源；点击后任务进入 Swoop；宿主断开时原浏览器下载不丢失。
- Package acceptance：`make dist` 的 zip 包含所有扩展 UI 文件和图标。
