cmake_minimum_required(VERSION 3.16)

# 共用组件（telemetry.h）放在工程外面，两个工程共享同一份
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common_components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(symfiddle_sender)
