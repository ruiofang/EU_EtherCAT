cmake_minimum_required(VERSION 3.5)
project(ethLab_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# -------- Qt5 --------
find_package(Qt5 COMPONENTS Widgets REQUIRED)

# -------- IgH EtherCAT --------
include_directories(/usr/local/etherlab/include)
link_directories(/usr/local/etherlab/lib)
find_library(ECRT_LIB ethercat HINTS /usr/local/etherlab/lib)
if(NOT ECRT_LIB)
    message(FATAL_ERROR "ethercat library (libethercat.so) not found in /usr/local/etherlab/lib")
endif()

add_executable(ethLab_gui
    main.cpp
    mainwindow.cpp
    mainwindow.h
    ecworker.cpp
    ecworker.h
)

target_link_libraries(ethLab_gui
    Qt5::Widgets
    ${ECRT_LIB}
    pthread
)
