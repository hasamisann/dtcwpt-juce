cmake_minimum_required(VERSION 3.22)
project(dtcwpt_tests VERSION 0.1.0 LANGUAGES CXX)

# C++20 required
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find JUCE (in order of priority)
# 1. System-installed JUCE (via find_package)
# 2. Local submodule at ../JUCE
# 3. Auto-download via FetchContent
if(NOT DEFINED JUCE_FOUND)
    find_package(JUCE QUIET)
endif()

if(NOT JUCE_FOUND)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../JUCE/CMakeLists.txt)
        message(STATUS "Using JUCE from local submodule at ../JUCE")
        add_subdirectory(../JUCE JUCE_build)
    else()
        message(STATUS "JUCE not found locally. Downloading via FetchContent...")
        include(FetchContent)
        FetchContent_Declare(
            JUCE
            GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
            GIT_TAG 8.0.0
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(JUCE)
    endif()
else()
    message(STATUS "Using system-installed JUCE")
endif()

# Add dtcwpt module from parent directory
juce_add_module(../dtcwpt)

# Test executable
juce_add_console_app(dtcwpt_tests
    PRODUCT_NAME "DT-CWPT Tests"
)

target_sources(dtcwpt_tests
    PRIVATE
        main.cpp
        suites/AnalysisSynthesisTests.cpp
        suites/ComplexUtilsTests.cpp
        suites/DelayBufferTests.cpp
        suites/DTCWPTProcessorTests.cpp
        suites/MorphBandProcessorTests.cpp
        suites/StatefulFilterTests.cpp
        suites/TopologyPlannerTests.cpp
        suites/TopologyStateTests.cpp
        suites/TopologySplitOrderTests.cpp
        suites/GraphicEqDragPaintTests.cpp
        suites/GateEditorSampleRateTests.cpp
        suites/GateGuiFrequencyMappingTests.cpp
        suites/SpectrumDataBridgeTests.cpp
        suites/BandLevelBridgeTests.cpp
        suites/GateBandProcessorTests.cpp
        util/TestUtils.cpp
        stubs/MorphAudioProcessorStubs.cpp
        ../sample_plugins/dtcwpt-morph/MorphBandProcessor.cpp
        ../sample_plugins/dtcwpt-morph/SpectrumDataBridge.cpp
        ../sample_plugins/dtcwpt-gate/GateBandProcessor.cpp
        ../sample_plugins/dtcwpt-gate/gui/GateGuiFrequencyMapping.cpp
)

target_include_directories(dtcwpt_tests
    PRIVATE
        .
        ../sample_plugins/dtcwpt-morph
        ../sample_plugins/dtcwpt-morph/gui
        ../sample_plugins/dtcwpt-gate
)

target_compile_definitions(dtcwpt_tests
    PRIVATE
        JucePlugin_Name="DT-CWPT Morph"
        JucePlugin_IsSynth=0
        JucePlugin_WantsMidiInput=0
        JucePlugin_ProducesMidiOutput=0
        JucePlugin_IsMidiEffect=0
)

target_link_libraries(dtcwpt_tests
    PRIVATE
        dtcwpt
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_processors
        juce::juce_dsp
        juce::juce_gui_basics
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# Enable testing
enable_testing()
add_test(NAME dtcwpt_unit_tests COMMAND dtcwpt_tests)

# Print configuration summary
message(STATUS "")
message(STATUS "DT-CWPT Tests Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "")
message(STATUS "To build:")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")
message(STATUS "To run tests:")
message(STATUS "  ctest --output-on-failure -C Release")
message(STATUS "")
