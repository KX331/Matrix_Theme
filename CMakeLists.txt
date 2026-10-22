cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 的核心编译脚本
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 🚀 必须有这一行，声明你的项目名称（解决日志中的 No project() 警告）
project(Matrix_Theme)

# 确保根目录 CMake 知道我们用了自定义分区
set(CONFIG_PARTITION_TABLE_CUSTOM y)
set(CONFIG_PARTITION_TABLE_FILENAME "partitions.csv")
