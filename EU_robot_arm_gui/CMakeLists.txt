cmake_minimum_required(VERSION 3.5)
project(EU_robot_arm_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# -------- Qt5 --------
find_package(Qt5 COMPONENTS Core Widgets REQUIRED)

# -------- IgH EtherCAT --------
set(ETHERLAB_ROOT_DIR "" CACHE PATH "Optional IgH EtherCAT installation prefix")

set(_etherlab_include_hints
    ${ETHERLAB_ROOT_DIR}/include
    /usr/local/etherlab/include
    /opt/etherlab/include
    /usr/src/ethercat/include
    /usr/local/include
    /usr/include
)

set(_etherlab_lib_hints
    ${ETHERLAB_ROOT_DIR}/lib
    ${ETHERLAB_ROOT_DIR}/lib64
    /usr/local/etherlab/lib
    /opt/etherlab/lib
    /usr/src/ethercat/lib
    /usr/src/ethercat/lib/.libs
    /usr/local/lib
    /usr/lib
    /usr/lib64
)

find_path(ECRT_INCLUDE_DIR
    NAMES ecrt.h
    HINTS ${_etherlab_include_hints}
)

find_library(ECRT_LIB
    NAMES ethercat
    HINTS ${_etherlab_lib_hints}
)

if(NOT ECRT_INCLUDE_DIR)
    message(FATAL_ERROR "ecrt.h not found. Set ETHERLAB_ROOT_DIR or install IgH EtherCAT headers.")
endif()

if(NOT ECRT_LIB)
    message(FATAL_ERROR "libethercat not found. Set ETHERLAB_ROOT_DIR or install IgH EtherCAT library.")
endif()

message(STATUS "Using EtherCAT headers: ${ECRT_INCLUDE_DIR}")
message(STATUS "Using EtherCAT library: ${ECRT_LIB}")

add_executable(EU_robot_arm_gui
    main.cpp
    mainwindow.cpp
    mainwindow.h
    ecworker.cpp
    ecworker.h
)

target_include_directories(EU_robot_arm_gui PRIVATE ${ECRT_INCLUDE_DIR})

target_link_libraries(EU_robot_arm_gui
    Qt5::Widgets
    ${ECRT_LIB}
    pthread
)

add_executable(EU_robot_arm_cli
    cli_main.cpp
    ecworker.cpp
    ecworker.h
)

target_include_directories(EU_robot_arm_cli PRIVATE ${ECRT_INCLUDE_DIR})

target_link_libraries(EU_robot_arm_cli
    Qt5::Core
    ${ECRT_LIB}
    pthread
)
