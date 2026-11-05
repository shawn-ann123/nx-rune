cmake_minimum_required(VERSION 3.22.0)

set(lib_name NNdetect)

find_package(CUDAToolkit REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc)
find_path(TENSORRT_INCLUDE_DIR NvInfer.h
    HINTS /usr/include /usr/local/include
    PATH_SUFFIXES aarch64-linux-gnu x86_64-linux-gnu)
find_library(TENSORRT_LIBRARY nvinfer REQUIRED)
find_library(TENSORRT_ONNX_PARSER_LIBRARY nvonnxparser REQUIRED)
if(NOT TENSORRT_INCLUDE_DIR)
    message(FATAL_ERROR "TensorRT headers not found (NvInfer.h). Install libnvinfer-dev.")
endif()

add_library(${lib_name}_lib SHARED src/NNDetector.cpp)
target_compile_features(${lib_name}_lib PRIVATE cxx_std_20)
target_include_directories(${lib_name}_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)
target_include_directories(${lib_name}_lib PRIVATE ${TENSORRT_INCLUDE_DIR})
target_link_libraries(
    ${lib_name}_lib
    PRIVATE
        app_interface_lib
        class_loader_lib
        config_loader_lib
        google_logger_lib
        img_viz_lib
        CUDA::cudart
        ${TENSORRT_LIBRARY}
        ${TENSORRT_ONNX_PARSER_LIBRARY}
        opencv_core
        opencv_imgproc
)
