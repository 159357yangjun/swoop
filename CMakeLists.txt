cmake_minimum_required(VERSION 3.16)

project(idm-next
    VERSION 0.1.0
    DESCRIPTION "全协议下载平台 - Qt 6 + libcurl + aria2"
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
# 优先 find_package(CURL)，否则回退 vendored third_party/libcurl。
# 结果写入 CURL_FOUND / LIBCURL_INC / LIBCURL_LIB，供下方目标复用。
if(WIN32)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "network: 使用 CMake 可发现的 libcurl")
    else()
        set(LIBCURL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libcurl")
        find_path(LIBCURL_INC NAMES curl/curl.h REQUIRED
                  PATHS "${LIBCURL_DIR}/include" NO_DEFAULT_PATH)
        find_library(LIBCURL_LIB NAMES libcurl libcurl.dll.a REQUIRED
                     PATHS "${LIBCURL_DIR}/lib" NO_DEFAULT_PATH)
        message(STATUS "network: 使用 vendored libcurl @ ${LIBCURL_DIR}")
    endif()
endif()

# NOTE: 网络层使用 libcurl（src/core/network_curl.c，API 与 network.h 保持一致）。
# libcurl 可由系统/vcpkg/MSYS2 提供；找不到时回退 third_party/libcurl。
# FTP / BT / 磁力由 aria2 后端经 JSON-RPC 托管；Sql 用于 SQLite 历史持久化。

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

set(MODEL_SOURCES
    src/model/task_list_model.cpp
    src/model/task_list_model.h
    src/model/category_filter_proxy.cpp
    src/model/category_filter_proxy.h
    src/model/task_group_proxy.cpp
    src/model/task_group_proxy.h
)

set(WEB_SOURCES
    src/web/web_server.cpp
    src/web/web_server.h
)

set(APP_SOURCES
    src/app/auto_power.cpp
    src/app/auto_power.h
    src/app/cli_forward.cpp
    src/app/cli_forward.h
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
    src/utils/tool_probe.cpp
    src/utils/off_thread.cpp
    src/utils/off_thread.h
    src/utils/socket_drain.cpp
    src/utils/socket_drain.h
)

# ── 共享静态库：聚合应用层源码，供 idm-next 与可选 ui_snapshot 复用 ──
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

if(WIN32)
    if(CURL_FOUND)
        target_link_libraries(idm_app_lib PUBLIC CURL::libcurl)
    else()
        target_include_directories(idm_app_lib PRIVATE ${LIBCURL_INC})
        target_link_libraries(idm_app_lib PUBLIC ${LIBCURL_LIB})
    endif()
    target_link_libraries(idm_app_lib PUBLIC dwmapi winmm)
endif()

# ── 主程序 ───────────────────────────────────────
# WIN32 子系统避免双击时弹出控制台窗口；--cli 模式由程序按需分配控制台。
qt_add_executable(idm-next WIN32
    src/main.cpp
    ${CLI_SOURCES}
    resources/resources.qrc
)
set_target_properties(idm-next PROPERTIES
    QT_WINDOWS_VERSION_MAJOR ${PROJECT_VERSION_MAJOR}
    QT_WINDOWS_VERSION_MINOR ${PROJECT_VERSION_MINOR}
    QT_WINDOWS_VERSION_PATCH ${PROJECT_VERSION_PATCH}
)
target_compile_definitions(idm-next PRIVATE IDM_NEXT_VERSION="${PROJECT_VERSION}")
target_link_libraries(idm-next PRIVATE idm_app_lib)

# ── Native Messaging Host ────────────────────────
qt_add_executable(idm-next-host WIN32
    src/host/idm_next_host.cpp
)
target_link_libraries(idm-next-host PRIVATE Qt6::Core Qt6::Network)
set_target_properties(idm-next-host PROPERTIES WIN32_EXECUTABLE TRUE)

# 开发构建后同步 host 与其 Qt 运行时到浏览器扩展目录，方便直接加载扩展调试。
set(IDM_NMH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/browser-extension/native-messaging-host")
add_custom_command(TARGET idm-next-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${IDM_NMH_DIR}"
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

# ── 可选 UI 离屏截图工具 ─────────────────────────
option(IDM_BUILD_UI_SNAPSHOT "Build offscreen UI snapshot tool" OFF)
if(IDM_BUILD_UI_SNAPSHOT)
    qt_add_executable(ui_snapshot
        tools/ui_snapshot.cpp
        resources/resources.qrc
    )
    target_link_libraries(ui_snapshot PRIVATE idm_app_lib)
endif()

# ── 可选下载引擎自测 ─────────────────────────────
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

# ── vendored libcurl 运行期文件 ──────────────────
# 只向“实际存在”的 executable target 添加 POST_BUILD，避免默认关闭 ui_snapshot
# 时仍引用不存在的 target，导致本地 vendored-libcurl 配置失败。
if(WIN32 AND NOT CURL_FOUND)
    set(_curl_runtime_targets idm-next)
    if(TARGET ui_snapshot)
        list(APPEND _curl_runtime_targets ui_snapshot)
    endif()

    foreach(_t IN LISTS _curl_runtime_targets)
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
