cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS")

project(Chloaelis VERSION 1.0.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE 9.0.5, as a git submodule (git clone --recursive, or: git submodule update --init)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    message(FATAL_ERROR "JUCE is missing: run  git submodule update --init  in the Chloaelis folder")
endif()
add_subdirectory(JUCE)

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
option(CHLOAELIS_COPY_AFTER_BUILD "Install the plug-ins into the user plug-in folders after building" OFF)

juce_add_plugin(Chloaelis
    VERSION 1.0.0
    COMPANY_NAME "Chloaelis"
    BUNDLE_ID "com.chloaelis.chloaelis"
    PLUGIN_MANUFACTURER_CODE Chls
    PLUGIN_CODE Chl2
    FORMATS ${CHL_FORMATS}
    PRODUCT_NAME "Chloaelis"
    ICON_BIG "Resources/icon/icon_1024.png"
    ICON_SMALL "Resources/icon/icon_1024.png"
    DESCRIPTION "dual multimode filter > tube > tape / PT2399 / BBD delay, with a Lorenz attractor"
    LV2URI "https://github.com/femifleming/Chloaelis"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx
    COPY_PLUGIN_AFTER_BUILD ${CHLOAELIS_COPY_AFTER_BUILD})

# Nunito Bold (SIL Open Font License), embedded so the UI looks the same on any machine
juce_add_binary_data(ChloaelisFonts SOURCES
    Resources/fonts/NunitoBold.ttf)

# the built-in source recordings (Ogg Vorbis, ~16 MB), compiled into the plug-in
juce_add_binary_data(ChloaelisAudio HEADER_NAME ChlAudio.h NAMESPACE ChlAudio SOURCES
    Resources/audio/baseloop.ogg
    Resources/audio/hydrophone3.ogg)

target_sources(Chloaelis PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(Chloaelis PUBLIC ${CHL_DEFS})

if(MSVC)
    target_compile_options(Chloaelis PRIVATE /bigobj)
endif()

target_link_libraries(Chloaelis
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

option(CHLOAELIS_BUILD_TESTS "Build offline DSP + host tests" ON)
if(CHLOAELIS_BUILD_TESTS)
    juce_add_console_app(ChloaelisTest PRODUCT_NAME "ChloaelisTest")
    target_sources(ChloaelisTest PRIVATE tests/OfflineTest.cpp)
    target_compile_definitions(ChloaelisTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
    target_link_libraries(ChloaelisTest PRIVATE juce::juce_dsp juce::juce_audio_formats
        PUBLIC juce::juce_recommended_config_flags)

    juce_add_gui_app(ChloaelisHostTest PRODUCT_NAME "ChloaelisHostTest")
    target_sources(ChloaelisHostTest PRIVATE tests/HostTest.cpp Source/PluginProcessor.cpp Source/PluginEditor.cpp)
    target_compile_definitions(ChloaelisHostTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ChloaelisHostTest PRIVATE ChloaelisFonts ChloaelisAudio juce::juce_audio_utils juce::juce_dsp juce::juce_opengl
        PUBLIC juce::juce_recommended_config_flags)
    if(APPLE)
        set_target_properties(ChloaelisTest ChloaelisHostTest PROPERTIES OSX_ARCHITECTURES "arm64")
    endif()
    # (Windows: M_PI for the tests, and a bigger stack: they make whole plug-ins as local variables)
    if(MSVC)
        foreach(t ChloaelisTest ChloaelisHostTest)
            target_compile_definitions(${t} PRIVATE _USE_MATH_DEFINES)
            target_link_options(${t} PRIVATE /STACK:16777216)
            target_compile_options(${t} PRIVATE /bigobj)
        endforeach()
    endif()
endif()
