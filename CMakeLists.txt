cmake_minimum_required(VERSION 3.16)

project(idm-next
    VERSION 0.1.0
    DESCRIPTION "全协议下载平台 - Qt 6 + libcurl + libtorrent"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── 依赖查找 ─────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Network Charts Sql)

# ── libcurl 解析（应用层与引擎自测共用，解析一次）────────
# 优先 find_package(CURL)（vcpkg），否则回退 vendored third_party/libcurl。
# 结果写入 CURL_FOUND / LIBCURL_INC / LIBCURL_LIB，供下方目标复用。
if(WIN32)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "network: 使用 vcpkg 提供的 libcurl")
    else()
        set(LIBCURL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libcurl")
        find_path(LIBCURL_INC NAMES curl/curl.h REQUIRED
                  PATHS "${LIBCURL_DIR}/include" NO_DEFAULT_PATH)
        find_library(LIBCURL_LIB NAMES libcurl libcurl.dll.a REQUIRED
                     PATHS "${LIBCURL_DIR}/lib" NO_DEFAULT_PATH)
        message(STATUS "network: 使用 vendored libcurl @ ${LIBCURL_DIR}")
    endif()
endif()

# NOTE: 网络层已切换到 libcurl（src/core/network_curl.c，API 与 network.h 完全一致，
#       download_core.c 等调用方无需改动）。libcurl 由项目第三方库 third_party/libcurl
#       （curl 8.22.0 官方 MinGW-w64 构建）提供，或由 vcpkg 提供（vcpkg.json 已声明依赖）。
#       链接策略：优先 find_package(CURL)（vcpkg），否则回退到 vendored
#       third_party/libcurl 的显式 include/lib 路径，并在 POST_BUILD 拷贝 libcurl DLL。
# FTP 由 aria2 后端（#34）经由 JSON-RPC 托管 aria2c 原生支持，与 libcurl 无关。
# Sql 组件用于下载历史持久化（#46，Qt6 自带 QSQLite 驱动 qsqlite.dll）。

# 阶段3才需要的依赖（BT/磁力、SQLite、日志等），届时取消注释
# find_package(libtorrent-rasterbar REQUIRED)
# find_package(sqlite3 CONFIG REQUIRED)
# find_package(spdlog CONFIG REQUIRED)
# find_package(nlohmann_json CONFIG REQUIRED)

# ── 源文件收集 ───────────────────────────────────
set(CORE_SOURCES
    src/core/download_core.c
    src/core/network_curl.c
)

set(STORAGE_SOURCES
    src/storage/storage.c
    src/storage/html_parser.c
    src/storage/history_store.cpp
)

set(GUI_SOURCES
    src/gui/main_window.cpp
    src/gui/progress_delegate.cpp
    src/gui/sidebar_panel.cpp
    src/gui/app_icons.cpp
    src/gui/app_icons.h
    src/gui/new_task_dialog.cpp
    src/gui/settings_dialog.cpp
    src/gui/task_detail_dialog.cpp
    src/gui/speed_chart_widget.cpp
    src/gui/batch_import_dialog.cpp
    src/gui/schedule_dialog.cpp
    src/gui/hash_verify_dialog.cpp
    src/gui/queue_manager.cpp
    src/gui/queue_manager_dialog.cpp
    src/gui/site_explorer_dialog.cpp
    src/gui/history_dialog.cpp
)

# 数据模型层（L1/L2 之间的 Qt 模型与代理）：任务列表模型 + 分类/分组代理
set(MODEL_SOURCES
    src/model/task_list_model.cpp
    src/model/task_list_model.h
    src/model/category_filter_proxy.cpp
    src/model/category_filter_proxy.h
    src/model/task_group_proxy.cpp
    src/model/task_group_proxy.h
)

# Web 远程管理层（控制面）：内置 HTTP 服务，供浏览器扩展与远程管理
set(WEB_SOURCES
    src/web/web_server.cpp
    src/web/web_server.h
)

# L2 应用编排层：跨 UI 与引擎的「行为组件」（自动关机/休眠等），
# 通过注入回调解耦 MainWindow 内部状态，便于独立测试与复用。
set(APP_SOURCES
    src/app/auto_power.cpp
    src/app/auto_power.h
    src/app/ipc_server.cpp
    src/app/ipc_server.h
    src/app/schedule_service.cpp
    src/app/schedule_service.h
    src/app/tray_controller.cpp
    src/app/tray_controller.h
    src/app/queue_scheduler.cpp
    src/app/queue_scheduler.h
    src/app/task_controller.cpp
    src/app/task_controller.h
    src/app/notification_aggregator.cpp
    src/app/notification_aggregator.h
    src/app/traffic_mode_controller.cpp
    src/app/traffic_mode_controller.h
    src/app/web_server_controller.cpp
    src/app/web_server_controller.h
    src/app/appearance_controller.cpp
    src/app/appearance_controller.h
)

set(CORE_WRAPPER_SOURCES
    src/core/download_manager.cpp
)

set(PROTOCOL_SOURCES
    src/protocols/idownloader.cpp
    src/protocols/http_downloader.cpp
    src/protocols/video_downloader.cpp
    src/protocols/video_backend.cpp
    src/protocols/hls_downloader.cpp
    src/protocols/torrent_downloader.cpp
    src/protocols/aria2_daemon.cpp
    src/protocols/torrent_backend.cpp
    src/protocols/ffmpeg_backend.cpp
    src/protocols/plugin_manager.cpp
    src/protocols/protocol_factory.cpp
)

set(CLI_SOURCES
    src/cli/cli_app.cpp
)

set(UTILS_SOURCES
    src/utils/settings.cpp
    src/utils/app_paths.cpp
)

# ── 共享静态库：聚合应用层全部源码，供 idm-next 与 ui_snapshot 复用 ──
# 消除两个可执行目标之间重复的源列表；仅入口（main.cpp / ui_snapshot.cpp）
# 与资源 qrc 留在各可执行目标，确保 Qt 资源在静态库外被正确初始化。
set(IDM_APP_SOURCES
    ${CORE_SOURCES}
    ${STORAGE_SOURCES}
    ${GUI_SOURCES}
    ${MODEL_SOURCES}
    ${WEB_SOURCES}
    ${APP_SOURCES}
    ${CORE_WRAPPER_SOURCES}
    ${PROTOCOL_SOURCES}
    ${UTILS_SOURCES}
)
add_library(idm_app_lib STATIC ${IDM_APP_SOURCES})
target_include_directories(idm_app_lib PUBLIC
    src/core
    src/storage
    src/gui
    src/model
    src/web
    src/app
    src/protocols
    src/cli
    src/utils
)
target_link_libraries(idm_app_lib PUBLIC
    Qt6::Core
    Qt6::Widgets
    Qt6::Network
    Qt6::Charts
    Qt6::Sql
)

# Windows 平台专用库（libcurl / DWM / 多媒体）：应用层源码直接使用，链接到
# idm_app_lib(PUBLIC) 由可执行目标继承；运行期 DLL 拷贝见文件末尾。
if(WIN32)
    if(CURL_FOUND)
        target_link_libraries(idm_app_lib PUBLIC CURL::libcurl)
    else()
        target_include_directories(idm_app_lib PRIVATE ${LIBCURL_INC})
        target_link_libraries(idm_app_lib PUBLIC ${LIBCURL_LIB})
    endif()
    target_link_libraries(idm_app_lib PUBLIC dwmapi winmm)
endif()

# ── 可执行目标 ───────────────────────────────────
# 注意：logger 为 header-only（inline），无需 .cpp；qss 通过 resources.qrc 打包
# WIN32_EXECUTABLE：GUI 子系统，双击不再闪控制台黑窗（#44 修复）。
# CLI 模式（--cli）仍可用：main.cpp 在 --cli 下调用 AllocConsole 分配控制台并
# 重定向标准流；单实例 GUI 已运行时 --cli 命令经 IPC 转发给 GUI。
qt_add_executable(idm-next WIN32
    src/main.cpp
    ${CLI_SOURCES}
    resources/resources.qrc
)
set_target_properties(idm-next PROPERTIES
    QT_WINDOWS_VERSION_MAJOR 0
    QT_WINDOWS_VERSION_MINOR 1
    QT_WINDOWS_VERSION_PATCH 0
)
# include 目录与 Qt 链接由 idm_app_lib(PUBLIC) 继承；平台专用库亦由 idm_app_lib 链接
target_link_libraries(idm-next PRIVATE idm_app_lib)

# ── 原生 Native Messaging Host（浏览器扩展桥接）──────────────
# 仅依赖 Qt6::Core；设为 Win32 子系统（无控制台黑窗），
# stdin/stdout 句柄由浏览器继承，走 Native Messaging 长度前缀协议。
qt_add_executable(idm-next-host WIN32
    src/host/idm_next_host.cpp
)
target_link_libraries(idm-next-host PRIVATE Qt6::Core Qt6::Network)
set_target_properties(idm-next-host PROPERTIES WIN32_EXECUTABLE TRUE)

# ── 构建后自动同步 host 到浏览器扩展目录 ───────────
# 浏览器扩展（chrome://extensions 加载的目录）需要同目录的 idm-next-host.exe
# 及其 Qt/MinGW 运行时，否则 Native Messaging 会报「宿主启动失败」。
# 这里在每次编译后：① 拷贝 exe ② 用 windeployqt 部署运行时，省去手动复制。
set(IDM_NMH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/browser-extension/native-messaging-host")
add_custom_command(TARGET idm-next-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:idm-next-host>"
            "${IDM_NMH_DIR}/idm-next-host.exe"
    COMMAND "$<TARGET_FILE_DIR:Qt6::Core>/windeployqt.exe"
            --release --no-translations --no-system-d3d-compiler --no-opengl-sw
            "${IDM_NMH_DIR}/idm-next-host.exe"
    COMMENT "同步 idm-next-host.exe 及其 Qt 运行时到 browser-extension/native-messaging-host"
)

# ── 安装配置 ─────────────────────────────────────
install(TARGETS idm-next idm-next-host
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)

# ── (可选) UI 离屏截图工具：仅用于无显示环境下评审界面，默认不构建 ──
option(IDM_BUILD_UI_SNAPSHOT "Build offscreen UI snapshot tool" OFF)
if(IDM_BUILD_UI_SNAPSHOT)
    qt_add_executable(ui_snapshot
        tools/ui_snapshot.cpp
        resources/resources.qrc
    )
    target_link_libraries(ui_snapshot PRIVATE idm_app_lib)
endif()

# ── (可选) 下载引擎自测：验证「设置 → 引擎」链路端到端真的生效（默认不构建） ──
# 需要配套的本地认证 HTTP 服务，见 tools/engine_selftest_server.py 顶部说明。
option(IDM_BUILD_ENGINE_SELFTEST "Build engine self-test (needs local auth HTTP server)" OFF)
if(IDM_BUILD_ENGINE_SELFTEST)
    add_executable(engine_selftest
        tools/engine_selftest.c
        ${CORE_SOURCES}
    )
    target_include_directories(engine_selftest PRIVATE src/core src/utils)
    if(WIN32)
        if(CURL_FOUND)
            target_link_libraries(engine_selftest PRIVATE CURL::libcurl)
        else()
            target_include_directories(engine_selftest PRIVATE ${LIBCURL_INC})
            target_link_libraries(engine_selftest PRIVATE ${LIBCURL_LIB})
        endif()
        target_link_libraries(engine_selftest PRIVATE winmm)
    endif()
endif()

# ── 运行期 DLL 拷贝（vendored libcurl）──────────────
# idm_app_lib 已 PUBLIC 链接 libcurl，故运行期 DLL 需落到各可执行目标目录旁。
if(WIN32 AND NOT CURL_FOUND)
    foreach(_t idm-next ui_snapshot)
        foreach(_f libcurl-x64.dll curl-ca-bundle.crt)
            if(EXISTS "${LIBCURL_DIR}/bin/${_f}")
                add_custom_command(TARGET ${_t} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${LIBCURL_DIR}/bin/${_f}" "$<TARGET_FILE_DIR:${_t}>/"
                    COMMENT "拷贝 libcurl 运行期文件: ${_f} -> ${_t}")
            endif()
        endforeach()
    endforeach()
endif()
