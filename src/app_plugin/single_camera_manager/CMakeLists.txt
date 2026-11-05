set(CAMERA_MANAGER_PLUGIN_TARGET single_camera_manager_plugin_lib)

find_package(OpenCV REQUIRED COMPONENTS core imgproc videoio)

find_path(HIKROBOT_MVS_INCLUDE_DIR MvCameraControl.h
    PATHS /opt/MVS/include
    NO_DEFAULT_PATH
)
find_library(HIKROBOT_MVS_LIBRARY MvCameraControl
    PATHS /opt/MVS/lib/aarch64 /opt/MVS/lib/64
    NO_DEFAULT_PATH
)

add_library(${CAMERA_MANAGER_PLUGIN_TARGET} SHARED
    src/HikRobotCamera.cpp
    src/RealCameraManager.cpp
)

target_include_directories(${CAMERA_MANAGER_PLUGIN_TARGET} PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/config
)

target_link_libraries(${CAMERA_MANAGER_PLUGIN_TARGET} PUBLIC
    app_interface_lib
    class_loader_lib
    config_loader_lib
    google_logger_lib
    opencv_videoio
    opencv_imgproc
)

if(HIKROBOT_MVS_INCLUDE_DIR AND HIKROBOT_MVS_LIBRARY)
    target_compile_definitions(${CAMERA_MANAGER_PLUGIN_TARGET} PRIVATE RP_HAS_HIKROBOT_MVS=1)
    target_include_directories(${CAMERA_MANAGER_PLUGIN_TARGET} PRIVATE ${HIKROBOT_MVS_INCLUDE_DIR})
    target_link_libraries(${CAMERA_MANAGER_PLUGIN_TARGET} PRIVATE ${HIKROBOT_MVS_LIBRARY})
    message(STATUS "HikRobot MVS camera support enabled: ${HIKROBOT_MVS_LIBRARY}")
else()
    message(WARNING "HikRobot MVS SDK not found under /opt/MVS; building fallback camera backends only")
endif()
