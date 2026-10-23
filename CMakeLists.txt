cmake_minimum_required(VERSION 3.16)

# 引入 ESP-IDF 的核心编译脚本
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 🚀 必须有这一行，声明你的项目名称（解决日志中的 No project() 警告）
project(Matrix_Theme)

# 注意：分区表配置不能在这里用 set(CONFIG_...) 设置（project() 之后设置无效，
# 且 CONFIG_PARTITION_TABLE_FILENAME 不是有效的 Kconfig 名称）。
# 自定义分区表统一在 sdkconfig.defaults 中声明：
#   CONFIG_PARTITION_TABLE_CUSTOM=y
#   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
