cmake_minimum_required(VERSION 3.25)
project(AIDecorator VERSION 0.2.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Set GEODE_SDK to your Geode SDK directory")
endif()
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
add_library(${PROJECT_NAME} SHARED src/main.cpp)
target_include_directories(${PROJECT_NAME} PRIVATE vendor)
setup_geode_mod(${PROJECT_NAME})

# TEMP CI diagnostic (revert after use): replay the real compile command
# for src/main.cpp and surface its errors as workflow annotations.
if(DEFINED ENV{GITHUB_STEP_SUMMARY})
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)
    add_custom_target(diagcheck ALL
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/diag-check.ps1
            -BuildDir ${CMAKE_BINARY_DIR}
        COMMENT "diagcheck"
        VERBATIM)
    add_dependencies(diagcheck GeodeBindings)
    add_dependencies(${PROJECT_NAME} diagcheck)
endif()
