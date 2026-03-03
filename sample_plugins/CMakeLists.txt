cmake_minimum_required(VERSION 3.22)
project(dtcwpt_sample_plugins VERSION 0.1.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find JUCE (in order of priority)
# 1. System-installed JUCE (via find_package)
# 2. Local submodule at ../JUCE
if(NOT DEFINED JUCE_FOUND)
    find_package(JUCE QUIET)
endif()

if(NOT JUCE_FOUND)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt)
        message(STATUS "Using JUCE from local submodule at ../JUCE")
        add_subdirectory(../JUCE JUCE_build)
    else()
        message(FATAL_ERROR "JUCE not found. Please provide JUCE at ../JUCE")
    endif()
else()
    message(STATUS "Using system-installed JUCE")
endif()

# Add dtcwpt module from parent directory
juce_add_module(../dtcwpt)

# Add plugin subdirectories
add_subdirectory(dtcwpt-morph)
add_subdirectory(dtcwpt-gate)
