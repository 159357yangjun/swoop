# IDM Next

对标 Internet Download Manager 的 **Windows 原生 C 语言下载器**。
纯 Win32 API + C11，不用 Qt / MFC / .NET / COM，全部静态链接，**下载即解压即用**。

> 状态：v0.1.0，功能已跑通并通过自测；GUI 交互仍在打磨。

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
所以 `idm.exe` 只依赖 7 个 Windows 自带系统 DLL：

```
ADVAPI32  COMCTL32  KERNEL32  msvcrt  SHELL32  USER32  WS2_32
```

`idm_nmhost.exe` 更少（4 个）。全部 `-static` 链接，**不需要 MinGW 运行时 DLL**。

### 为什么是两个 exe

发布包含 `idm.exe`（主程序）与 `idm_nmhost.exe`（浏览器扩展的 native messaging 宿主）。

`idm_nmhost.exe` 必须独立存在，原因是**硬约束**：Chrome/Edge 的 native messaging 靠
**stdin/stdout 传长度前缀帧**，而「控制台子系统」还是「GUI 子系统」写在 PE 头里、运行期无法切换
——GUI 子系统的 exe 没有 stdout。真实 IDM 同样是多进程结构（`IDMan.exe` + `IDMMsgHost.exe`
+ `IDMGrHlp.exe` + `idmBroker.exe` 等 8 个 exe）。

两个文件请放在同一目录；少 `idm_nmhost.exe` 不会崩溃，只是浏览器扩展失效。
另外宿主在注册表里存的是**绝对路径** —— 挪动目录或换机器后需重跑 `register-nmhost.cmd`，
否则扩展会**静默失效**（不报错，只是点了没反应）。

交付的 exe 在链接期带 `-s`（strip）：去掉符号表与调试节，`idm.exe` 227 KB → 148 KB，
`idm_nmhost.exe` 89 KB → 45 KB。这属于**瘦身 + 去符号，不是加密**
（原生 exe 无法真正加密 —— CPU 必须能执行明文机器码）。

## 快速开始

1. 到 [Releases](../../releases) 下载 `idm-next-<版本>-win64.zip` 并解压。
2. 双击 `idm.exe`。任务列表 → 菜单「文件 → 新建任务」，粘贴链接即可。

> 未做代码签名，首次运行 Windows SmartScreen 可能提示，选「仍要运行」。

## 浏览器扩展

1. 浏览器打开 `chrome://extensions`（Edge 是 `edge://extensions`），开启「开发者模式」。
2. 「加载已解压的扩展程序」→ 选中解压目录里的 `extension` 文件夹。
3. 复制扩展卡片上的 **ID**，运行目录里的 `register-nmhost.cmd <扩展ID>` 注册宿主。
4. 重启浏览器。之后右键链接选「用 IDM Next 下载」，或浏览器自带下载会被自动接管。

## 从源码构建

需要 MinGW-w64 gcc（本机用 Qt 自带的那份，CI 用 MSYS2 的）与 GNU make。

```bash
make all          # → idm.exe / idm_selftest.exe / idm_nmhost.exe
```

工具链路径可在命令行覆盖：

```bash
make all CC=gcc WINDRES=windres
```

执行自测（无窗口，控制台直跑）：

```bash
./idm_selftest.exe          # 期望 15/15 PASS，退出码 0
./idm_nmhost.exe --selftest # 期望 PASS
```

打发行包：

```bash
make dist         # → dist/idm-next-<版本>-win64.zip + SHA256SUMS.txt
```

## 目录结构

```
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

- 仅 Windows（x64），不支持 Win7 以下。
- BT / 磁力需**自行准备 `aria2c.exe` 放进 `idm.exe` 同目录**，否则该功能不可用。
- 引擎与打包有自动化验证；**GUI 交互**（托盘、气泡、对话框）目前需人工验证。
- 未做代码签名。

## 版本

版本号唯一来源是 `src/common/version.h`，改一处，exe 属性页 / 安装包 / 发布 tag 同步。

## 许可

[MIT](LICENSE) © 2026 杨珺

> 本项目与 Tonec Inc. 的 Internet Download Manager 无任何关联，命名仅为表明设计取向。
