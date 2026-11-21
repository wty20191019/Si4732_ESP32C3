# 以下五行样板必须以此顺序出现在工程 CMakeLists 中，否则 CMake 无法正确工作
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(Si4732_Radio)
