cmake_minimum_required(VERSION 3.22)
project(RrSamplePads VERSION 1.0.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(RrSamplePads
    COMPANY_NAME "Rr Audio"
    BUNDLE_ID "com.rraudio.samplepads"
    PLUGIN_MANUFACTURER_CODE RrAu
    PLUGIN_CODE RrSp
    FORMATS VST3
    PRODUCT_NAME "Rr Sample Pads"
    DESCRIPTION "16-pad sample player"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(RrSamplePads)
target_sources(RrSamplePads PRIVATE Plugin.cpp)
target_compile_definitions(RrSamplePads PUBLIC
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(RrSamplePads PRIVATE
    juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags
           juce::juce_recommended_lto_flags
           juce::juce_recommended_warning_flags)
