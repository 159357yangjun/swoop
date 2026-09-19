# third_party/libcurl — 项目内 vendored libcurl

本目录存放 **libcurl** 的官方 Windows(MinGW-w64) 预编译构建，作为 IDM Next 的 HTTP/HTTPS
传输层依赖（替换原 `src/core/network.c` 的 WinHTTP 实现，见 `src/core/network_curl.c`）。

## 来源（官方、可信，非随机下载）
- 项目：curl-for-win（curl 官方维护的 Windows 构建）
- 版本：**curl 8.22.0_1**（build 8.22.0_1，2026-09-02）
- 下载页：https://curl.se/windows/
- 直接下载 URL：
  `https://curl.se/windows/dl-8.22.0_1/curl-8.22.0_1-win64-mingw.zip`
- SHA256（校验用）：
  `7f23b039f6ea4197362d4468e1a0e71428201222e1bef3b680d5ef7b2aefb714`
- PGP/签名（可选校验）：同目录下的 `curl-8.22.0_1-win64-mingw.zip.asc`（minisign/cosign/sigstore/SSH）

## 构建信息
- 工具链：llvm-mingw 20260826（mingw-w64 15.0.0），与本项目 Qt 6.11.1 的 MinGW 工具链
  同为 MinGW-w64，ABI 兼容，可直接链接。
- 静态链接依赖（已编进 libcurl.dll，**无独立依赖 DLL**）：
  nghttp2 1.70.0（HTTP/2）、libressl 4.3.2（TLS）、zlibng 2.3.3、zstd 1.5.7、
  brotli 1.2.0、libpsl 0.23.3、libssh2 1.11.1。
- 因此 `bin/libcurl-x64.dll` 是自包含的，运行时只需随 exe 拷贝该 DLL。

## 目录布局
- `include/curl/`   —— 头文件（`curl/curl.h` 等），CMake `find_path(LIBCURL_INC)` 用
- `lib/`            —— 导入库 `libcurl.dll.a`，CMake `find_library(LIBCURL_LIB)` 用
- `bin/`            —— 运行期 `libcurl-x64.dll`，POST_BUILD 自动拷贝到 exe 目录
- `docs/`           —— 许可证与文档（curl 本身 MIT；各组件许可证见其内）

## 为什么 vendored 而非 vcpkg
项目 `vcpkg.json` 已声明 `"libcurl"` 依赖，但本机尚未 bootstrap vcpkg 实例，且当前构建
使用系统 Qt（非 vcpkg 的 Qt）。为避免改动现有可用构建，这里把 libcurl 直接 vendored 到
项目内、并明确标注来源，符合「依赖必须落在项目文件中或清晰标注」的策略。若日后接入
vcpkg，`CMakeLists.txt` 会优先 `find_package(CURL)` 而忽略本目录。

## 升级方法
下载更新的 curl-for-win mingw zip，校验 SHA256 后替换 `bin/`、`lib/`、`include/` 即可。
