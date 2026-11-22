cmake_minimum_required(VERSION 3.16)
project(vm_desktop LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CANVAS_JS_ENGINE "ant" CACHE STRING "JS engine: ant")
set(ANT_ROOT "H:/ant-v16" CACHE PATH "Ant engine checkout")
set(CANVAS_SOURCE "E:/canvas-cpp/recovery/out/recovered" CACHE PATH "canvas-cpp source tree")
set(CLAY_DIR "H:/github_repos/clay" CACHE PATH "Clay checkout containing clay.h")

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
)
target_include_directories(demo_wm PRIVATE "${CLAY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
if(WIN32)
    target_link_libraries(demo_wm PRIVATE ws2_32)
endif()

# C 版渲染宿主：复用 canvas-cpp / ant 引擎执行前端脚本。
add_executable(vm_renderer renderer/main.c)
target_link_libraries(vm_renderer PRIVATE canvas)

set_target_properties(demo_wm vm_renderer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}"
)
