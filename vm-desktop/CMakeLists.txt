cmake_minimum_required(VERSION 3.16)
project(vm_desktop LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CANVAS_JS_ENGINE "ant" CACHE STRING "JS engine: ant")
set(ANT_ROOT "H:/ant-v16" CACHE PATH "Ant engine checkout")
set(CANVAS_SOURCE "E:/canvas-cpp/recovery/out/recovered" CACHE PATH "canvas-cpp source tree")
set(CLAY_DIR "H:/github_repos/clay" CACHE PATH "Clay checkout containing clay.h")
set(MSTB_DIR "H:/github_repos/mystbfont_c" CACHE PATH "mystbfont_c checkout containing stb_truetype.h")

add_subdirectory("${CANVAS_SOURCE}" "${CMAKE_BINARY_DIR}/canvas-cpp" EXCLUDE_FROM_ALL)

# 崩溃钩子静态库：exe 链接后，在 main 第一行调用 CrashReport_Install(NULL)。
# 崩溃时向 stderr 打印符号化堆栈，并写 minidump / .rpt（见 crash_report.h）。
add_library(crash_report STATIC crash_report.c)
if(WIN32)
    target_link_libraries(crash_report PUBLIC dbghelp)
endif()

# 崩溃钩子探针：运行即打印当前堆栈并触发一次真实崩溃，验证钩子本身可用。
add_executable(crash_probe crash_probe.c)
target_include_directories(crash_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(crash_probe PRIVATE crash_report)

# 帧断言测试：不开服务、不开浏览器，直接断言度量/装饰/裁剪/脏标记/输入路由。
add_executable(test_frame
    test_frame.c
    clay_frame_adapter.c
    vm_display.c
    vm_wm.c
    vm_font.c
    vm_theme.c
    vm_shell.c
    vm_shell_local.c
    vm_template.c
    vm_menu.c
    vm_clipboard.c
)
target_include_directories(test_frame PRIVATE "${CLAY_DIR}" "${MSTB_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/wms")

# 菜单控件断言：右键路由、popup 外点关闭、菜单命中与自动关闭（不开服务/浏览器）。
add_executable(test_menu
    test_menu.c
    clay_frame_adapter.c
    vm_display.c
    vm_wm.c
    vm_font.c
    vm_theme.c
    vm_shell.c
    vm_shell_local.c
    vm_template.c
    vm_menu.c
    vm_clipboard.c
)
target_include_directories(test_menu PRIVATE "${CLAY_DIR}" "${MSTB_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

# 文本编辑状态机断言：纯逻辑，不链接 Clay / WM / 网络，毫秒级。
add_executable(test_text_edit
    test_text_edit.c
    ui/ui_text_edit.c
)
target_include_directories(test_text_edit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/ui")

# 平台能力抽象层断言：只验证内核事实（进程表、内存、CPU 差分、错误分类）。
# 两个实现文件互斥：win32 用 #ifdef _WIN32，stub 用 #ifndef _WIN32。
add_executable(test_sys
    test_sys.c
    vm_sys_win32.c
    vm_sys_stub.c
)
target_include_directories(test_sys PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    target_link_libraries(test_sys PRIVATE psapi)
endif()
target_include_directories(test_text_edit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# jp_xaml 断言：XAML 子集解析、绑定与 handler 表（不链接 Clay / WM / 网络）。
add_executable(test_ui
    test_ui.c)
target_link_libraries(test_ui PRIVATE vm_ui_core)

add_executable(test_jpx_xaml
    test_jpx_xaml.c
    jpx_xaml/jpx_xaml_parser.c
    jpx_xaml/jpx_xaml_state.c
    jpx_xaml/jpx_xaml_render_direct.c
    jpx_xaml/jpx_xaml_direct_ui.c
    jpx_xaml/jpx_xaml_theme.c
    vm_display.c
    vm_theme.c
)
target_include_directories(test_jpx_xaml PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/jpx_xaml"
    "${CMAKE_CURRENT_SOURCE_DIR}/jpx")

# 跨 app 的控件状态机（M7.3）：焦点 / 文本编辑 / 命中，零布局引擎依赖。
# retain 与 imm 两种写法共用；第三方 app 可直接链接。
add_library(vm_ui_core STATIC
    ui/ui_text_edit.c
    ui/ui_focus.c
    ui/ui_hit.c)
target_include_directories(vm_ui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ui")

# Clay 后端的控件层（M7.3b）：布局交给 Clay，状态/焦点来自 vm_ui_core。
add_library(vm_ui_clay STATIC
    ui/clay/ui_clay.c
    ui/clay/clay_impl.c
    clay_frame_adapter.c)
target_include_directories(vm_ui_clay PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui"
    "${CMAKE_CURRENT_SOURCE_DIR}/ui/clay"
    "${CLAY_DIR}" "${MSTB_DIR}")
target_link_libraries(vm_ui_clay PUBLIC vm_ui_core)

# jp_xaml 的 Direct 渲染与命令缓冲后端（M7.1）：shell / app 共用，不依赖 Clay。
add_library(jpx_xaml_direct STATIC
    jpx_xaml/jpx_xaml_parser.c
    jpx_xaml/jpx_xaml_state.c
    jpx_xaml/jpx_xaml_render_direct.c
    jpx_xaml/jpx_xaml_direct_ui.c
    jpx_xaml/jpx_xaml_backend_cmdbuf.c
    jpx_xaml/jpx_xaml_theme.c
    vm_display.c
    vm_font.c
    vm_theme.c)
target_include_directories(jpx_xaml_direct PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/jpx_xaml"
    "${MSTB_DIR}")

# jp_xaml 主题断言：ResourceDictionary 解析/查找/渐变/派生（不链接 Clay/WM/网络）。
add_executable(test_jpx_xaml_theme
    test_jpx_xaml_theme.c
    jpx_xaml/jpx_xaml_parser.c
    jpx_xaml/jpx_xaml_theme.c
    jpx_xaml/jpx_html_theme.c
    vm_display.c
    vm_serialize_json.c
    vm_frame_bin.c
    vm_theme.c
)
target_include_directories(test_jpx_xaml_theme PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/jpx_xaml")

# 文件抽象层断言：路径 UTF-8 往返、元数据、错误分类。与 test_sys 同理，两个实现互斥。
add_executable(test_fs
    test_fs.c
    vm_fs_win32.c
    vm_fs_stub.c
)
target_include_directories(test_fs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# C 版渲染宿主：复用 canvas-cpp / ant 引擎执行前端脚本。
add_executable(vm_renderer renderer/main.c)
target_include_directories(vm_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vm_renderer PRIVATE canvas crash_report)

# ---------------------------------------------------------------
# WMS 进程层：来自 wmwms（协议 v2，命名管道 + libuv）。
# 只提供窗口元数据、IPC 传输与进程身份；合成/瓦片/序列化仍在本项目，
# 由宿主回调接进去（见 docs/task-vm-wms-merge.md §6）。
# ---------------------------------------------------------------
set(WMS_UV_TARGET "")
find_package(libuv CONFIG QUIET)
# 静态优先（MSYS2 的 libuv::uv 指向 libuv-1.dll，uv_a 指向 libuv.a）：
# 产物自包含，e2e 不需要把 mingw64/bin 加进 PATH 才能跑。
if(TARGET libuv::uv_a)
    set(WMS_UV_TARGET libuv::uv_a)
elseif(TARGET libuv::uv)
    set(WMS_UV_TARGET libuv::uv)
elseif(TARGET uv_a)
    set(WMS_UV_TARGET uv_a)
elseif(TARGET uv)
    set(WMS_UV_TARGET uv)
endif()

if(NOT WMS_UV_TARGET)
    find_path(WMS_UV_INCLUDE_DIR NAMES uv.h
              HINTS "C:/msys64/mingw64/include" /usr/include /usr/local/include)
    find_library(WMS_UV_LIBRARY NAMES uv libuv uv_a libuv_a
                 HINTS "C:/msys64/mingw64/lib" /usr/lib /usr/local/lib)
    if(NOT WMS_UV_INCLUDE_DIR OR NOT WMS_UV_LIBRARY)
        message(FATAL_ERROR
            "libuv not found.\n"
            "  指定头文件与库：-DWMS_UV_INCLUDE_DIR=<dir> -DWMS_UV_LIBRARY=<file>\n"
            "  或装 MSYS2 的 mingw-w64-x86_64-libuv")
    endif()
    add_library(wms_uv UNKNOWN IMPORTED)
    set_target_properties(wms_uv PROPERTIES
        IMPORTED_LOCATION "${WMS_UV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${WMS_UV_INCLUDE_DIR}")
    set(WMS_UV_TARGET wms_uv)
endif()
message(STATUS "WMS libuv target: ${WMS_UV_TARGET}")

# 静态 libuv 在 Windows 上要显式连带一批系统库
set(WMS_PLATFORM_LIBS "")
if(WIN32)
    set(WMS_PLATFORM_LIBS ws2_32 iphlpapi psapi userenv advapi32 dbghelp
                          ole32 shell32 winmm)
endif()

# 协议编解码：纯函数，不依赖 socket，单测直接链它
add_library(wms_core STATIC wms/wms_codec.c)
target_include_directories(wms_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/wms")

# 进程身份抽象：Windows 管道句柄 / POSIX pidfd / mock
add_library(wms_peer STATIC wms/wms_peer.c)
target_include_directories(wms_peer PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/wms")
if(WIN32)
    target_link_libraries(wms_peer PRIVATE kernel32)
endif()

# 客户端 SDK：APP / SHELL / RDP 共用，句柄式
add_library(wms_client STATIC wms/wms_client.c)
target_link_libraries(wms_client
    PUBLIC  wms_core ${WMS_UV_TARGET}
    PRIVATE ${WMS_PLATFORM_LIBS})

add_executable(test_wms_ipc wms/test_wms_ipc.c)
target_link_libraries(test_wms_ipc PRIVATE wms_core)

add_executable(test_wms_peer wms/test_wms_peer.c)
target_link_libraries(test_wms_peer PRIVATE wms_peer)

# 窗口管理服务。M3 起默认挂真实合成宿主（wms/vm_host.c），
# 因此带上本项目的合成链：vm_wm / vm_display / vm_tiles / 序列化 / 字体 / 主题。
# M5.2 起 vm_wm 不再认识 Clay：窗口内容契约是 VmCmdBuffer，Clay 只在 producer 侧。
add_executable(vm_wms
    wms/main.c
    wms/wms_server.c
    wms/wms_mock_host.c
    wms/vm_host.c
    wms/vm_shm.c
    vm_wm.c
    vm_display.c
    vm_serialize_json.c
    vm_frame_bin.c
    vm_tiles.c
    vm_font.c
    vm_theme.c
)
target_include_directories(vm_wms PRIVATE
    "${MSTB_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/wms"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vm_wms PRIVATE
    wms_core wms_peer ${WMS_UV_TARGET} ${WMS_PLATFORM_LIBS})
if(WIN32)
    target_link_libraries(vm_wms PRIVATE kernel32)
endif()

# 协议级客户端演示：APP / SHELL / RDP 三角色，e2e 脚本直接驱动它们
add_executable(test_wms_app wms/test_wms_app.c)
target_link_libraries(test_wms_app PRIVATE wms_client)
# 推真实 VmDrawCommand，需要 vm_display.h 的类型定义
target_include_directories(test_wms_app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(test_wms_shell wms/test_wms_shell.c)
target_link_libraries(test_wms_shell PRIVATE wms_client)

add_executable(test_wms_rdp wms/test_wms_rdp.c)
target_link_libraries(test_wms_rdp PRIVATE wms_client)

# 二进制 tile 帧往返测试
add_executable(test_frame_bin
    test_frame_bin.c
    vm_frame_bin.c
    vm_serialize_json.c
    vm_display.c)
target_include_directories(test_frame_bin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# 共享内存 ring 单测
add_executable(test_shm wms/test_shm.c wms/vm_shm.c)
target_include_directories(test_shm PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/wms")

# 呈现端服务器：HTTP / SSE / WebSocket / 静态资源 / respack。
# 帧与资源全部来自 vm_wms（EVT_FRAME / EVT_FULLFRAME / EVT_RESOURCES），
# 所以它不链 vm_wm / Clay：rdp 不懂桌面语义。
add_executable(vm_rdp
    rdp/main.c
    rdp/rdp_server.c
    wms/vm_shm.c
    vm_frame_bin.c
    vm_serialize_json.c
    vm_display.c)
target_include_directories(vm_rdp PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/rdp"
    "${CMAKE_CURRENT_SOURCE_DIR}/wms"
    "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vm_rdp PRIVATE wms_client ${WMS_PLATFORM_LIBS})

# 桌面外壳：任务栏内容 + 输入命中。只链纯绘制层（vm_shell.c）与字体/主题，
# 不链 vm_wm / Clay；窗口列表与动作通过 wms_client 走 vm_wms。
add_executable(vm_shell
    shell/main.c
    vm_shell.c
    vm_template.c
    vm_menu_view.c)
target_include_directories(vm_shell PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/wms"
    "${MSTB_DIR}")
# 开始菜单用 XAML（Direct 后端）；jpx_xaml_direct 已带 vm_display / vm_font。
target_link_libraries(vm_shell PRIVATE wms_client jpx_xaml_direct ${WMS_PLATFORM_LIBS})

# 新架构应用（M6）：只依赖 VmCmdBuffer 与 wms 协议。
# 不链 vm_wm / vm_server / vm_shell / Clay；A1 的界面是手写命令。
add_executable(vm_app
    app/main.c
    app/ui_hello.c
    app/ui_xaml.c
    app/ui_clay_demo.c)
target_include_directories(vm_app PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/wms"
    "${MSTB_DIR}")
# --ui xaml 复用 jp_xaml 的 Direct 后端；--ui clay 用 vm_ui_clay。
# 两个库都带 vm_display / vm_font，静态链接时链接器按需取一份。
target_link_libraries(vm_app PRIVATE wms_client jpx_xaml_direct vm_ui_clay ${WMS_PLATFORM_LIBS})

set_target_properties(vm_renderer test_frame test_text_edit test_jpx_xaml test_jpx_xaml_theme test_sys test_fs test_wms_ipc test_wms_peer test_frame_bin test_shm vm_wms test_wms_app test_wms_shell test_wms_rdp vm_rdp vm_shell vm_app test_ui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)
