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

# 桌面服务端：窗口管理、remote tiles 切分与 SSE 服务，全部在 guest 侧。
add_executable(demo_wm
    demo_wm.c
    clay_frame_adapter.c
    vm_display.c
    vm_serialize_json.c
    vm_tiles.c
    vm_wm.c
    vm_server.c
    vm_font.c
    vm_theme.c
    vm_shell.c
    vm_clipboard.c
)
target_include_directories(demo_wm PRIVATE "${CLAY_DIR}" "${MSTB_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    target_link_libraries(demo_wm PRIVATE ws2_32)
endif()

# JPX 宿主示例：应用代码只 include jpx.h，Clay 实现单独放一个 TU。
add_executable(demo_jpx
    demo_jpx.c
    jpx/jpx_clay_impl.c
    jpx/jpx_app.c
    jpx/jpx_window.c
    jpx/jpx_theme.c
    jpx/jpx_widget.c
    jpx/jpx_text_edit.c
    clay_frame_adapter.c
    vm_display.c
    vm_serialize_json.c
    vm_tiles.c
    vm_wm.c
    vm_server.c
    vm_font.c
    vm_theme.c
    vm_shell.c
    vm_clipboard.c
    vm_sys_win32.c
    vm_sys_stub.c
    vm_fs_win32.c
    vm_fs_stub.c
)
target_include_directories(demo_jpx PRIVATE "${CLAY_DIR}" "${MSTB_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/jpx")
if(WIN32)
    target_link_libraries(demo_jpx PRIVATE ws2_32 psapi)
endif()

# 帧断言测试：不开服务、不开浏览器，直接断言度量/装饰/裁剪/脏标记/输入路由。
add_executable(test_frame
    test_frame.c
    clay_frame_adapter.c
    vm_display.c
    vm_wm.c
    vm_font.c
    vm_theme.c
    vm_shell.c
    vm_clipboard.c
)
target_include_directories(test_frame PRIVATE "${CLAY_DIR}" "${MSTB_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")

# 文本编辑状态机断言：纯逻辑，不链接 Clay / WM / 网络，毫秒级。
add_executable(test_text_edit
    test_text_edit.c
    jpx/jpx_text_edit.c
)

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

# 文件抽象层断言：路径 UTF-8 往返、元数据、错误分类。与 test_sys 同理，两个实现互斥。
add_executable(test_fs
    test_fs.c
    vm_fs_win32.c
    vm_fs_stub.c
)
target_include_directories(test_fs PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# C 版渲染宿主：复用 canvas-cpp / ant 引擎执行前端脚本。
add_executable(vm_renderer renderer/main.c)
target_link_libraries(vm_renderer PRIVATE canvas)

set_target_properties(demo_wm demo_jpx vm_renderer test_frame test_text_edit test_sys test_fs PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)
