cmake_minimum_required(VERSION 3.22)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum OS X version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal architectures")
endif()

project(CASSETTE_PLUGIN VERSION 1.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Runtime estático en Windows (igual que Omni): el .vst3 no depende de vcruntime
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# JUCE 8.0.0: reutiliza la copia ya descargada por Omni si existe; si no, la baja
set(CASSETTE_JUCE_DIR "${CMAKE_SOURCE_DIR}/../Omni Plugin/build-dev/_deps/juce-src" CACHE PATH "Carpeta de JUCE")
if(EXISTS "${CASSETTE_JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${CASSETTE_JUCE_DIR}" juce)
else()
    include(FetchContent)
    FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 8.0.0)
    FetchContent_MakeAvailable(juce)
endif()

set(CASSETTE_FORMATS VST3 Standalone)
set(CASSETTE_EXTRA "")
if(WIN32)
    set(CASSETTE_EXTRA NEEDS_WEBVIEW2 TRUE)
endif()
if(APPLE)
    list(APPEND CASSETTE_FORMATS AU)
endif()

juce_add_plugin(CassettePlugin
    COMPANY_NAME "SVNDALPHON"
    BUNDLE_ID "com.svndalphon.cassette"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PLUGIN_MANUFACTURER_CODE Svnd
    PLUGIN_CODE Cass
    FORMATS ${CASSETTE_FORMATS}
    PRODUCT_NAME "Cassette"
    ${CASSETTE_EXTRA})

# La interfaz (un solo HTML) va dentro del binario
juce_add_binary_data(CassetteUI SOURCES ui/index.html)

target_sources(CassettePlugin PRIVATE Source/PluginProcessor.cpp Source/PluginEditor.cpp
    Source/licensing/Ed25519.cpp Source/licensing/Ed25519.h
    Source/licensing/LicenseManager.cpp Source/licensing/LicenseManager.h)
target_include_directories(CassettePlugin PRIVATE Source Source/licensing)

# Solo para pruebas E2E con un servidor simulado (licencia aislada). NUNCA distribuir ese binario.
set(CASSETTE_API_BASE_OVERRIDE "" CACHE STRING "URL del backend para pruebas (vacío = https://offszn.lat)")
if(CASSETTE_API_BASE_OVERRIDE)
    target_compile_definitions(CassettePlugin PRIVATE CASSETTE_API_BASE="${CASSETTE_API_BASE_OVERRIDE}" CASSETTE_STORE_TAG="_e2e")
endif()
target_compile_definitions(CassettePlugin PUBLIC
    JUCE_WEB_BROWSER=1
    $<$<BOOL:${WIN32}>:JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1>
    CASSETTE_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(CassettePlugin
    PRIVATE CassetteUI juce::juce_audio_utils juce::juce_gui_extra juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_lto_flags juce::juce_recommended_warning_flags)

if(MSVC)
    target_compile_options(CassettePlugin PRIVATE /utf-8)
endif()

# Pruebas offline + render de audio real
juce_add_console_app(CassetteTest PRODUCT_NAME "CassetteTest")
target_sources(CassetteTest PRIVATE tools/CassetteTest.cpp Source/PluginProcessor.cpp)
target_compile_definitions(CassetteTest PRIVATE
    CASSETTE_TESTING=1 JucePlugin_Name="Cassette" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1 JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(CassetteTest PRIVATE
    juce::juce_audio_processors juce::juce_audio_formats juce::juce_audio_utils juce::juce_dsp juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(CassetteTest PRIVATE /utf-8)
endif()

# Licencias / anti-abuso (reloj, red y disco simulados; firmas con tools/sign_test.mjs, necesita Node)
juce_add_console_app(CassetteLicenseTests PRODUCT_NAME "CassetteLicenseTests")
target_sources(CassetteLicenseTests PRIVATE tests/LicenseTests.cpp Source/licensing/Ed25519.cpp Source/licensing/LicenseManager.cpp)
target_include_directories(CassetteLicenseTests PRIVATE Source Source/licensing)
target_compile_definitions(CassetteLicenseTests PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=0
    CASSETTE_TOOLS_DIR="${CMAKE_SOURCE_DIR}/tools")
target_link_libraries(CassetteLicenseTests PRIVATE
    juce::juce_core juce::juce_events juce::juce_audio_basics juce::juce_recommended_config_flags)
if(MSVC)
    target_compile_options(CassetteLicenseTests PRIVATE /utf-8)
endif()
