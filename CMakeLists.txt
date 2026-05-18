cmake_minimum_required(VERSION 3.10)

project(RK3568_Yolo_YOLOv8 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "System processor: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Install prefix: ${CMAKE_INSTALL_PREFIX}")

add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_options(-O2)
else()
    add_compile_options(-O0 -g)
endif()

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED libmosquitto)

find_package(OpenCV REQUIRED)

find_library(TURBOJPEG_LIBRARY turbojpeg REQUIRED)
message(STATUS "TurboJPEG library: ${TURBOJPEG_LIBRARY}")

message(STATUS "Mosquitto libraries: ${MOSQUITTO_LIBRARIES}")
message(STATUS "OpenCV version: ${OpenCV_VERSION}")
message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")

set(RKNN_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknn/include)
set(RGA_INCLUDE_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rga/include)
set(YOLOV8_DIR       ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknn_yolov8)


set(RKNN_LIB ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rknn/lib/librknnrt.so)
set(RGA_LIB  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rga/lib/librga.so)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${YOLOV8_DIR}
    ${RKNN_INCLUDE_DIR}
    ${RGA_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

if(NOT EXISTS ${RKNN_LIB})
    message(FATAL_ERROR "librknnrt.so not found: ${RKNN_LIB}")
endif()

if(NOT EXISTS ${RGA_LIB})
    message(FATAL_ERROR "librga.so not found: ${RGA_LIB}")
endif()

set(YOLOV8_SOURCES
    third_party/rknn_yolov8/yolov8.cc
    third_party/rknn_yolov8/postprocess.cc
    third_party/rknn_yolov8/file_utils.c
    third_party/rknn_yolov8/image_utils.c
)

add_executable(edge_ai_pipeline
    src/main.cpp
    src/capture.cpp
    src/inference.cpp
    src/alarm.cpp
    src/config.cpp
    src/rule_engine.cpp
    ${YOLOV8_SOURCES}
)

target_include_directories(edge_ai_pipeline PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${YOLOV8_DIR}
    ${RKNN_INCLUDE_DIR}
    ${RGA_INCLUDE_DIR}
    ${MOSQUITTO_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_directories(edge_ai_pipeline PRIVATE
    ${MOSQUITTO_LIBRARY_DIRS}
)

target_link_libraries(edge_ai_pipeline PRIVATE
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${OpenCV_LIBS}
    ${RKNN_LIB}
    ${RGA_LIB}
    ${TURBOJPEG_LIBRARY}
    rt
    dl
    m
    jpeg
    png
    z
)

add_executable(test_rule_engine
    tests/test_rule_engine.cpp
    src/rule_engine.cpp
)

target_include_directories(test_rule_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
add_executable(web_viewer
    src/web_viewer.cpp
)

target_include_directories(web_viewer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MOSQUITTO_INCLUDE_DIRS}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_directories(web_viewer PRIVATE
    ${MOSQUITTO_LIBRARY_DIRS}
)

target_link_libraries(web_viewer PRIVATE
    Threads::Threads
    ${MOSQUITTO_LIBRARIES}
    ${OpenCV_LIBS}
    rt
)

install(TARGETS edge_ai_pipeline web_viewer
    RUNTIME DESTINATION .
)

install(FILES
    ${RKNN_LIB}
    ${RGA_LIB}
    DESTINATION lib
)

install(DIRECTORY model/
    DESTINATION model
    FILES_MATCHING
    PATTERN "*.rknn"
    PATTERN "*.txt"
)

message(STATUS "Configuration done.")
