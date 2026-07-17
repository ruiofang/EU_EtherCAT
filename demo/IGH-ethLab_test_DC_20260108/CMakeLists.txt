cmake_minimum_required(VERSION 3.0)
project(ethercat_example)

# 使用C++11或以上版本
set(CMAKE_CXX_STANDARD 11)

# 包含头文件路径
include_directories(/usr/local/etherlab/include)

# 添加库文件搜索路径
link_directories(/usr/local/etherlab/lib)

# 添加可执行文件
add_executable(ethercat_example main.cpp)

# 查找并链接 ecrt 库
find_library(ECRT_LIB ethercat HINTS /usr/local/etherlab/lib)
if (NOT ECRT_LIB)
    message(FATAL_ERROR "ethercat library not found")
endif()

target_link_libraries(ethercat_example ${ECRT_LIB})
