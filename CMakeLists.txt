#顶层cmakelists
cmake_minimum_required(VERSION 3.22.0)
project(26AutoAim)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PROJECT_OUTPUT_DIR ${CMAKE_SOURCE_DIR}/output)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_OUTPUT_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_OUTPUT_DIR})

foreach(CONFIG_TYPE Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER ${CONFIG_TYPE} CONFIG_TYPE_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} ${PROJECT_OUTPUT_DIR})
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} ${PROJECT_OUTPUT_DIR})
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_TYPE_UPPER} ${PROJECT_OUTPUT_DIR})
endforeach()

# Jetson's OpenCV/TensorRT packages require GLIBCXX_3.4.30, while the GCC 11
# driver otherwise selects its older linker copy first.  Match the proven
# workaround used by the source vehicle project and link the system runtime
# explicitly for every target created below.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    find_library(RP26_SYSTEM_LIBSTDCXX
        NAMES libstdc++.so.6
        PATHS /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu
        NO_DEFAULT_PATH
        REQUIRED
    )
    link_libraries(${RP26_SYSTEM_LIBSTDCXX})
    message(STATUS "Using Jetson system libstdc++: ${RP26_SYSTEM_LIBSTDCXX}")
endif()


add_subdirectory(src/core)
add_subdirectory(src/app_plugin)
