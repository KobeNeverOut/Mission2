cmake_minimum_required(VERSION 3.16)
project(daheng_camera_rmlog LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# 依赖 1: OpenCV（取流显示、图像转换、存图）
#   优先用 find_package 的 OpenCVConfig.cmake；找不到就退回 pkg-config。
#   源码编译安装的 OpenCV 经常只有 opencv4.pc 而没有 cmake config，
#   这里两条路都留着，省得为环境问题改工程。
# ---------------------------------------------------------------------------
find_package(OpenCV QUIET COMPONENTS core imgproc highgui imgcodecs)
if(OpenCV_FOUND)
    message(STATUS "OpenCV ${OpenCV_VERSION} —— 来自 find_package(OpenCVConfig.cmake)")
    set(OPENCV_TARGET OpenCV::OpenCV)
    if(NOT TARGET OpenCV::OpenCV)
        # 老版本 OpenCV（< 4.5.2）只给变量，不给 imported target
        add_library(opencv_shim INTERFACE)
        target_include_directories(opencv_shim INTERFACE ${OpenCV_INCLUDE_DIRS})
        target_link_libraries(opencv_shim INTERFACE ${OpenCV_LIBS})
        set(OPENCV_TARGET opencv_shim)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PC_OPENCV4 REQUIRED IMPORTED_TARGET opencv4)
    message(STATUS "OpenCV ${PC_OPENCV4_VERSION} —— 来自 pkg-config(opencv4.pc)")
    set(OPENCV_TARGET PkgConfig::PC_OPENCV4)
endif()

# ---------------------------------------------------------------------------
# 依赖 2: 线程（spdlog 异步日志线程池需要）
# ---------------------------------------------------------------------------
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# 依赖 3: 大恒 GalaxySDK
#   头文件用仓库自带的 include/sdk（版本固定，保证复现）；
#   动态库 libgxiapi.so 必须来自本机安装，找不到就明确报错并给出排查提示。
#   如果相机型号需要 DxImageProc 单独成库，这里会自动带上。
# ---------------------------------------------------------------------------
set(GALAXY_SDK_ROOT "" CACHE PATH "大恒 GalaxySDK 安装根目录（含 lib/libgxiapi.so）")

find_library(GXIAPI_LIBRARY
    NAMES gxiapi
    HINTS ${GALAXY_SDK_ROOT}/lib ${GALAXY_SDK_ROOT}/lib/x64 ${GALAXY_SDK_ROOT}/lib/x86_64
    PATHS /usr/lib /usr/local/lib /usr/lib/x86_64-linux-gnu /opt/GalaxySDK/lib
)
if(NOT GXIAPI_LIBRARY)
    message(FATAL_ERROR
        "找不到大恒 GalaxySDK 的 libgxiapi.so。\n"
        "  1) 确认已安装 GalaxySDK Linux 版（默认会装到 /usr/lib/libgxiapi.so）\n"
        "  2) 或显式指定: cmake -DGALAXY_SDK_ROOT=/path/to/GalaxySDK ..\n"
        "  3) 只想编译不链接相机库时，见 README 的「没有相机也能编译」一节")
endif()
message(STATUS "GalaxySDK: ${GXIAPI_LIBRARY}")

find_library(DXIMAGEPROC_LIBRARY NAMES DxImageProc HINTS ${GALAXY_SDK_ROOT}/lib)
if(DXIMAGEPROC_LIBRARY)
    message(STATUS "DxImageProc: ${DXIMAGEPROC_LIBRARY}")
endif()

# ---------------------------------------------------------------------------
# 目标
#   一个大恒相机封装 + 一个取流程序；rm_log 与 spdlog 都随仓库自带。
# ---------------------------------------------------------------------------
set(APP_SOURCES
    src/main.cpp
    src/daheng_camera.cpp
    util/rm_log/src/rm_log.cpp
)

add_executable(daheng_camera_demo ${APP_SOURCES})

target_include_directories(daheng_camera_demo PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include          # camera/*.h 与 sdk/*.h（大恒头文件）
    ${CMAKE_CURRENT_LIST_DIR}/util             # rm_log.h 里 #include "spdlog/spdlog.h"
    ${CMAKE_CURRENT_LIST_DIR}/util/rm_log/include
)

target_link_libraries(daheng_camera_demo PRIVATE
    ${OPENCV_TARGET}
    ${GXIAPI_LIBRARY}
    Threads::Threads
    ${CMAKE_DL_LIBS}
)

if(DXIMAGEPROC_LIBRARY)
    target_link_libraries(daheng_camera_demo PRIVATE ${DXIMAGEPROC_LIBRARY})
endif()

# 库在自定义目录时，让可执行文件能直接跑起来（不必额外设 LD_LIBRARY_PATH）
set_target_properties(daheng_camera_demo PROPERTIES
    BUILD_RPATH "${GALAXY_SDK_ROOT}/lib"
    INSTALL_RPATH "${GALAXY_SDK_ROOT}/lib"
)

install(TARGETS daheng_camera_demo RUNTIME DESTINATION bin)
