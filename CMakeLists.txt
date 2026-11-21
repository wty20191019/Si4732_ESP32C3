# 以下五行样板必须以此顺序出现在工程 CMakeLists 中，否则 CMake 无法正确工作
cmake_minimum_required(VERSION 3.22)

# 本工程不用 WiFi/BT：只构建 main 及其依赖，WiFi/BT 组件完全不编译
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Si4732_Radio)
