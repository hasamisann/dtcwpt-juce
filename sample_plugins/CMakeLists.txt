cmake_minimum_required(VERSION 3.22)
project(dtcwpt_morph_plugin VERSION 0.1.0 LANGUAGES CXX)

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

# DT-CWPT Morph Plugin target
juce_add_plugin(DtcwptMorph
    COMPANY_NAME                "hasamisann"
    PLUGIN_MANUFACTURER_CODE    "Hmsn"
    PLUGIN_CODE                 "DtMo"
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "DT-CWPT Morph"
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_SYNTH                    FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
)

target_sources(DtcwptMorph
    PRIVATE
        MorphAudioProcessor.h
        MorphAudioProcessor.cpp
)

target_include_directories(DtcwptMorph
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(DtcwptMorph
    PUBLIC
        # New plugin — no VST2 version has ever been released, so suppress the
        # VST3/VST2 parameter-ID mismatch warning that JUCE emits by default.
        JUCE_IGNORE_VST3_MISMATCHED_PARAMETER_ID_WARNING=1
)

target_link_libraries(DtcwptMorph
    PRIVATE
        dtcwpt
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_dsp
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Print configuration summary
message(STATUS "")
message(STATUS "DT-CWPT Morph Plugin Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "")
message(STATUS "To build:")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")
