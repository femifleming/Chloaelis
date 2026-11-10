cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")

project(Chloaeilius VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 9.0.5, as a git submodule (git clone --recursive, or: git submodule update --init)
# Chloaeilius lives in the Chloaelis repository: it shares its JUCE, fonts, recordings and icon
set(CHL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(NOT EXISTS "${CHL_ROOT}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "JUCE is missing: run  git submodule update --init  in the Chloaelis repository's folder")
endif()
add_subdirectory("${CHL_ROOT}/JUCE" "${CMAKE_BINARY_DIR}/JUCE")

set(CHL_DEFS JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0 JUCE_DISPLAY_SPLASH_SCREEN=0)

# plug-in formats per platform: AU only exists on macOS, LV2 is the native Linux format
if(APPLE)
    set(CHL_FORMATS AU VST3 Standalone)
elseif(UNIX)
    set(CHL_FORMATS VST3 LV2 Standalone)
else()
    set(CHL_FORMATS VST3 Standalone)
endif()

# copying the built plug-ins into the system plug-in folders is handy on your own machine,
# but not on build servers
option(CHLOAEILIUS_COPY_AFTER_BUILD "Install the plug-ins into the user plug-in folders after building" OFF)

juce_add_plugin(Chloaeilius
    VERSION 0.1.0
    COMPANY_NAME "Chloaelis"
    BUNDLE_ID "com.chloaelis.chloaeilius"
    PLUGIN_MANUFACTURER_CODE Chls
    PLUGIN_CODE Chsp
    FORMATS ${CHL_FORMATS}
    PRODUCT_NAME "Chloaeilius"
    ICON_BIG "${CHL_ROOT}/Resources/icon/icon_1024.png"
    ICON_SMALL "${CHL_ROOT}/Resources/icon/icon_1024.png"
    DESCRIPTION "Chloaelis in 3rd-order Ambisonics: filter > tube > delay whose echoes fly round you on a flock and a Lorenz attractor"
    LV2URI "https://github.com/femifleming/Chloaelis/Chloaeilius"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Spatial
    COPY_PLUGIN_AFTER_BUILD ${CHLOAEILIUS_COPY_AFTER_BUILD})

# Nunito Bold (SIL Open Font License), embedded so the UI looks the same on any machine
juce_add_binary_data(ChloaelisFonts SOURCES
    ${CHL_ROOT}/Resources/fonts/NunitoBold.ttf)

# the built-in source recordings (Ogg Vorbis, ~16 MB), compiled into the plug-in
juce_add_binary_data(ChloaelisAudio HEADER_NAME ChlAudio.h NAMESPACE ChlAudio SOURCES
    ${CHL_ROOT}/Resources/audio/baseloop.ogg
    ${CHL_ROOT}/Resources/audio/hydrophone3.ogg)

target_sources(Chloaeilius PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Chloaeilius PUBLIC ${CHL_DEFS})

if(MSVC)
    target_compile_options(Chloaeilius PRIVATE /bigobj)
endif()

target_link_libraries(Chloaeilius
    PRIVATE
        ChloaelisFonts
        ChloaelisAudio
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

option(CHLOAEILIUS_BUILD_TESTS "Build offline DSP + host tests" ON)
if(CHLOAEILIUS_BUILD_TESTS)
    juce_add_console_app(ChloaeiliusTest PRODUCT_NAME "ChloaeiliusTest")
    target_sources(ChloaeiliusTest PRIVATE tests/OfflineTest.cpp)
    target_compile_definitions(ChloaeiliusTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ChloaeiliusTest PRIVATE juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags)

    juce_add_console_app(ChloaeiliusSpatialTest PRODUCT_NAME "ChloaeiliusSpatialTest")
    target_sources(ChloaeiliusSpatialTest PRIVATE tests/SpatialTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(ChloaeiliusSpatialTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ChloaeiliusSpatialTest PRIVATE ChloaelisFonts ChloaelisAudio juce::juce_audio_utils juce::juce_dsp juce::juce_opengl
        PUBLIC juce::juce_recommended_config_flags)

    juce_add_gui_app(ChloaeiliusHostTest PRODUCT_NAME "ChloaeiliusHostTest")
    target_sources(ChloaeiliusHostTest PRIVATE tests/HostTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(ChloaeiliusHostTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ChloaeiliusHostTest PRIVATE ChloaelisFonts ChloaelisAudio juce::juce_audio_utils juce::juce_dsp juce::juce_opengl
        PUBLIC juce::juce_recommended_config_flags)
    if(APPLE)
        set_target_properties(ChloaeiliusTest ChloaeiliusHostTest ChloaeiliusSpatialTest PROPERTIES OSX_ARCHITECTURES "arm64")
    endif()
    # (Windows: M_PI for the tests, and a bigger stack: they make whole plug-ins as local variables)
    if(MSVC)
        foreach(t ChloaeiliusTest ChloaeiliusSpatialTest ChloaeiliusHostTest)
            target_compile_definitions(${t} PRIVATE _USE_MATH_DEFINES)
            target_link_options(${t} PRIVATE /STACK:16777216)
            target_compile_options(${t} PRIVATE /bigobj)
        endforeach()
    endif()
endif()
