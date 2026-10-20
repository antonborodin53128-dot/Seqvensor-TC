cmake_minimum_required(VERSION 3.22)

project(TC VERSION 0.1.0 LANGUAGES C CXX)

if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static runtime: the exe runs on a clean Windows without extra redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.12
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Time-stretch without changing the pitch: Signalsmith Stretch + Signalsmith Linear (both MIT licensed).
# Header-only: only fetched, their own CMake files are not used.
FetchContent_Declare(signalsmith_linear
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/linear.git
    GIT_TAG 0.6.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake-here)
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG 1.4.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake-here)
FetchContent_MakeAvailable(signalsmith_linear signalsmith_stretch)

# ONNX Runtime (MIT): runs the key-detection network. The prebuilt DLL is embedded in the exe as a resource,
# unpacked into the settings folder on first use and loaded from there (the import is delay-loaded).
if(WIN32)
    if(CMAKE_GENERATOR_PLATFORM MATCHES "ARM64|arm64" OR CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
        set(ORT_ARCH arm64)
    else()
        set(ORT_ARCH x64)
    endif()
    set(ORT_VERSION 1.22.0)
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-win-${ORT_ARCH}-${ORT_VERSION}.zip
        SOURCE_SUBDIR no-cmake-here)
    FetchContent_MakeAvailable(onnxruntime)
    set(ORT_DLL_PATH "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
    set(MODEL_PATH "${CMAKE_SOURCE_DIR}/Resources/nmp.onnx")
    configure_file(Resources/embedded.rc.in ${CMAKE_BINARY_DIR}/embedded.rc @ONLY)
endif()

# Steinberg ASIO SDK 2.3.4 (GPLv3 / proprietary dual licence): only its headers are needed, so that TC can open ASIO drivers
# (Behringer WING, RME, Focusrite, MOTU ... with all their channels, not only one stereo pair as through Windows Audio).
if(WIN32)
    FetchContent_Declare(asiosdk
        URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
        URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
        SOURCE_SUBDIR no-cmake-here)
    FetchContent_MakeAvailable(asiosdk)
    file(GLOB_RECURSE ASIO_DRV_HEADER "${asiosdk_SOURCE_DIR}/*iasiodrv.h")
    list(GET ASIO_DRV_HEADER 0 ASIO_DRV_HEADER)
    get_filename_component(ASIO_INCLUDE_DIR "${ASIO_DRV_HEADER}" DIRECTORY)
    message(STATUS "ASIO SDK headers: ${ASIO_INCLUDE_DIR}")
endif()

juce_add_gui_app(TC
    PRODUCT_NAME "TC"
    COMPANY_NAME "TC"
    VERSION ${PROJECT_VERSION})

target_sources(TC PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/Track.cpp
    Source/MainComponent.cpp
    Source/TrackComponent.cpp
    Source/Timeline.cpp
    Source/PlayerBar.cpp
    Source/Clip.cpp
    Source/TempoDetect.cpp
    Source/Stretch.cpp
    Source/LiveStretch.cpp
    Source/KeyDetect.cpp
    Source/LiveKey.cpp
    Source/Lyrics.cpp
    Source/Builtin.cpp
    Source/Realtime.cpp)

if(WIN32)
    target_sources(TC PRIVATE ${CMAKE_BINARY_DIR}/embedded.rc)
    target_include_directories(TC PRIVATE ${onnxruntime_SOURCE_DIR}/include)
    target_link_libraries(TC PRIVATE ${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib delayimp)
    target_link_options(TC PRIVATE /DELAYLOAD:onnxruntime.dll)
    target_include_directories(TC PRIVATE ${ASIO_INCLUDE_DIR})
endif()

target_compile_definitions(TC PRIVATE
    JUCE_APPLICATION_NAME_STRING="TC"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}"
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_AU=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ASIO=$<IF:$<PLATFORM_ID:Windows>,1,0>
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_include_directories(TC PRIVATE
    ${signalsmith_stretch_SOURCE_DIR}
    ${signalsmith_linear_SOURCE_DIR}/include)

# Source files are UTF-8 (Russian UI text); /bigobj for the template-heavy Signalsmith headers.
target_compile_options(TC PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>" "$<$<CXX_COMPILER_ID:MSVC>:/bigobj>")

target_link_libraries(TC PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_dsp
    juce::juce_gui_basics
    juce::juce_gui_extra
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)
