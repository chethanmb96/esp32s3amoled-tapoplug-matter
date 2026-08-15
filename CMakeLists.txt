# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{ESP_MATTER_PATH})
    set(ENV{ESP_MATTER_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/managed_components/espressif__esp_matter")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tapo-matter-display)
