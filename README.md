# Swoop

A lightweight native Windows download manager written in C (C11 + Win32).
Multi-segment HTTP/HTTPS downloads, per-segment resume, browser extension via
native messaging, BT/magnet delegated to aria2c.

对标 Internet Download Manager 的 **Windows 原生 C 语言下载器**。
纯 Win32 API + C11，不用 Qt / MFC / .NET / COM，全部静态链接，**下载即解压即用**。

> 状态：v0.2.0，功能已跑通并通过自测；GUI 交互仍在打磨。

### 名字

**Swoop** = 猛禽从高空俯冲、一把抓走猎物。下载的本质就是「定位目标 → 高速取回」，
一个词讲完；单音节、5 个字母，好打也好念。

叫 Swoop 而不是 IDM Next，是为了**避开 Tonec Inc. 的 IDM 商标**——本项目只在设计取向上
对标 Internet Download Manager，与 Tonec 无任何关联（见文末「许可」）。

## 特性

| 分类 | 说明 |
|---|---|
| 下载引擎 | 多线程分片（1–16 连接）、断点续传（**逐段进度**，乱序写入不留空洞） |
| 网络 | 自研 HTTP/HTTPS 引擎（WinHTTP 动态加载）、系统代理 / 直连 |
| 队列 | 并发上限控制，超出自动排队，空槽自动补启动 |
| 目录 | 按文件类型自动归类（视频 / 音乐 / 压缩 / 文档 / 程序 / 其他） |
| 限速 | 全局限速（虚拟时钟算法，跨线程精确聚合） |
| 调度 | 定时到点自动 开始 / 暂停全部 |
| 持久化 | 任务列表 + 逐段进度落盘，重启后可继续 |
| 通知 | 完成时托盘气泡 + 提示音（可分别开关） |
| 托盘 | 关闭最小化到托盘、启动隐藏 |
| 浏览器 | MV3 扩展 + native messaging 宿主，接管浏览器下载 |
| BT / 磁力 | 委托外部 `aria2c.exe`（不自研 BT 协议栈） |

### 为什么体积小、无依赖

`winhttp.dll`、`winmm.dll` 都是运行时 `LoadLibrary` 加载，**不进静态导入表**，
所以 `swoop.exe` 只依赖 7 个 Windows 自带系统 DLL：

```text
ADVAPI32  COMCTL32  KERNEL32  msvcrt  SHELL32  USER32  WS2_32
```

`swoop_nmhost.exe` 更少（4 个）。全部 `-static` 链接，**不需要 MinGW 运行时 DLL**。

### 为什么是两个 exe

发布包含 `swoop.exe`（主程序）与 `swoop_nmhost.exe`（浏览器扩展的 native messaging 宿主）。

`swoop_nmhost.exe` 必须独立存在，原因是**硬约束**：Chrome/Edge 的 native messaging 靠
**stdin/stdout 传长度前缀帧**，而「控制台子系统」还是「GUI 子系统」写在 PE 头里、运行期无法切换
——GUI 子系统的 exe 没有 stdout。

两个文件请放在同一目录；少 `swoop_nmhost.exe` 不会崩溃，只是浏览器扩展失效。
另外宿主在注册表里存的是**绝对路径** —— 挪动目录或换机器后需重跑 `register-nmhost.cmd`，
否则扩展会静默失效。

交付的 exe 在链接期带 `-s`（strip），用于去掉符号表与调试节。这是发行瘦身，不是加密。

## 快速开始

1. 到 [Releases](../../releases) 下载 `swoop-<版本>-win64.zip` 并解压。
2. 双击 `swoop.exe`。
3. 任务列表 → 菜单「文件 → 新建任务」，粘贴链接即可。

> 未做代码签名，首次运行 Windows SmartScreen 可能提示，选「仍要运行」。

## 浏览器扩展

1. 浏览器打开 `chrome://extensions`（Edge 是 `edge://extensions`），开启「开发者模式」。
2. 「加载已解压的扩展程序」→ 选中发行包里的 `extension` 文件夹。
3. 复制扩展卡片上的 **ID**，运行 `register-nmhost.cmd <扩展ID>` 注册宿主，然后重新加载扩展。
4. 点击工具栏 Swoop 图标，在 Radar 中选择「捕获当前页面」扫描视频、音频、文档和压缩包；点击「接管」即可交给 Swoop。
5. 右键链接仍可选「用 Swoop 下载」。宿主不可用时，原浏览器下载不会被取消。

## 从源码构建

需要 MinGW-w64 GCC 与 GNU make。

```bash
make all
```

生成：

```text
swoop.exe
swoop_selftest.exe
swoop_nmhost.exe
```

工具链路径可覆盖：

```bash
make all CC=gcc WINDRES=windres
```

执行自测：

```bash
./swoop_selftest.exe
./swoop_nmhost.exe --selftest
```

打发行包：

```bash
make dist
```

生成：

```text
dist/swoop-<版本>-win64.zip
dist/SHA256SUMS.txt
```

`extension/` 会递归完整收录进 ZIP，CI 同时验证打包测试和 SHA256，并上传可下载的构建 artifact。

## GitHub 自动发布

Swoop 与仓库 `master` 分支上的 IDM Next 使用**独立 tag 命名空间**，避免两个产品互相触发发布。

版本号唯一来源为 `src/common/version.h`。

正式发布示例：

```bash
git tag swoop-v0.2.1
git push origin swoop-v0.2.1
```

`swoop-v*` 会触发 Swoop 自己的 GitHub Actions：

```text
编译
→ 引擎自测
→ Native Messaging Host 自测
→ DLL 导入约束检查
→ 打包测试
→ 生成便携 ZIP
→ SHA256 校验
→ GitHub Release
```

不要使用通用的 `v*` tag；仓库中的另一个项目 IDM Next 使用 `idm-next-v*`。

## 目录结构

```text
src/
  common/     util / jsonlite / version / ipcmsg
  engine/     http / download / queue / category / torrent
              speedlimit / sched / taskstore
  gui/        main_window / resources(.rc/.h)
  main.c      入口、单实例、命令行 URL
  nmhost.c    浏览器 native messaging 宿主
  selftest_run.c  共享自测（GUI 与自测程序复用）
extension/    MV3 浏览器扩展
packaging/    发行打包脚本
```

## 已知限制

- 仅 Windows x64，不支持 Win7 以下。
- BT / 磁力需自行准备 `aria2c.exe` 放进 `swoop.exe` 同目录，否则该功能不可用。
- 引擎、宿主与打包有自动化验证；GUI 交互（托盘、气泡、对话框）仍需人工验证。
- 未做代码签名。
- 浏览器开发者模式加载的扩展 ID 需要在首次配置时手动传给 `register-nmhost.cmd`。

## 许可

[MIT](LICENSE) © 2026 杨珺

> 本项目与 Tonec Inc. 的 Internet Download Manager 无任何关联，命名仅为表明设计取向。
