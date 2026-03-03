project(dtcwpt_gate_plugin VERSION 0.1.0 LANGUAGES CXX)

# Embedded font resources (Hanken Grotesk, SIL OFL v1.1)
juce_add_binary_data(DtcwptGateResources
    SOURCES
        resources/HankenGrotesk-Regular.ttf
        resources/HankenGrotesk-Bold.ttf
        resources/OFL.txt
)

# DT-CWPT Gate Plugin target
juce_add_plugin(DtcwptGate
    COMPANY_NAME                "LTSU"
    PLUGIN_MANUFACTURER_CODE    "LTSU"
    PLUGIN_CODE                 "Dtcg"
    FORMATS                     VST3 Standalone
    PRODUCT_NAME                "DT-CWPT Gate"
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_SYNTH                    FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
)

target_sources(DtcwptGate
    PRIVATE
        TopologyState.h
        SpectrumDataBridge.h
        SpectrumDataBridge.cpp
        BandLevelBridge.h
        GateBandProcessor.h
        GateBandProcessor.cpp
        GateAudioProcessor.h
        GateAudioProcessor.cpp
        GateAudioProcessorEditor.cpp
        gui/FontHelper.h
        gui/GateEditor.h
        gui/GateEditor.cpp
        gui/GateMainView.h
        gui/GateMainView.cpp
        gui/GraphicEqWidget.h
        gui/GraphicEqWidget.cpp
        gui/SpectrumAnalyzerWidget.h
        gui/SpectrumAnalyzerWidget.cpp
        gui/TopologyEditorView.h
        gui/TopologyEditorView.cpp
        gui/AboutView.h
        gui/AboutView.cpp
)

target_include_directories(DtcwptGate
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/gui
)

target_compile_definitions(DtcwptGate
    PUBLIC
        # New plugin — no VST2 version has ever been released, so suppress the
        # VST3/VST2 parameter-ID mismatch warning that JUCE emits by default.
        JUCE_IGNORE_VST3_MISMATCHED_PARAMETER_ID_WARNING=1
)

# Suppress C4819 (file encoding / code-page mismatch) on MSVC.
# This warning is a Japanese-locale artefact (code page 932) and is not a code
# quality issue.  It is pre-existing across all source files in the project.
if(MSVC)
    target_compile_options(DtcwptGate PRIVATE /wd4819)
endif()

target_link_libraries(DtcwptGate
    PRIVATE
        DtcwptGateResources
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
message(STATUS "DT-CWPT Gate Plugin Configuration:")
message(STATUS "  Version: ${PROJECT_VERSION}")
message(STATUS "  C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  Generator: ${CMAKE_GENERATOR}")
message(STATUS "")
message(STATUS "To build:")
message(STATUS "  cmake --build . --config Release")
message(STATUS "")
