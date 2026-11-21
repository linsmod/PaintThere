cmake_minimum_required(VERSION 3.16)
project(vm_renderer LANGUAGES C CXX)

set(CANVAS_JS_ENGINE "ant" CACHE STRING "JS engine: ant")
set(ANT_ROOT "H:/ant-v16" CACHE PATH "Ant engine checkout")
set(CANVAS_SOURCE "E:/canvas-cpp/recovery/out/recovered" CACHE PATH "canvas-cpp source tree")

add_subdirectory("${CANVAS_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/canvas-cpp")
add_executable(vm_renderer main.c)
target_link_libraries(vm_renderer PRIVATE canvas)
