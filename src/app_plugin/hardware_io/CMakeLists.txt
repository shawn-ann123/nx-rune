add_library(hardware_io_lib SHARED src/SocketCan.cpp)
target_include_directories(hardware_io_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hardware_io_lib PUBLIC pthread)
