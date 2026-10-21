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

# TEMP CI diagnostic (revert after use): surface compiler errors as
# workflow annotations so they are readable without log download.
if(DEFINED ENV{GITHUB_STEP_SUMMARY})
    file(APPEND "$ENV{GITHUB_STEP_SUMMARY}" "DIAG-CMAKE-REACHED\n")
    set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_COMPILER_LAUNCHER "powershell;-NoProfile;-ExecutionPolicy;Bypass;-File;${CMAKE_CURRENT_SOURCE_DIR}/tools/diag-wrap.ps1")
endif()
