cmake_minimum_required(VERSION 3.16)

# 收发两端共用 telemetry.h，组件目录在工程外面。
# 少了这行，main 里 REQUIRES shared 会报 "Failed to resolve component 'shared'"。
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common_components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fiddle5_receiver)
