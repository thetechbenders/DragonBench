cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS firmware/common firmware/targets/esp32s3/main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dragonbench)
