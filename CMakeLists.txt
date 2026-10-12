cmake_minimum_required(VERSION 3.22)

project(TC VERSION 0.1.0 LANGUAGES C CXX)

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
    Source/Stretch.cpp)

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
    JUCE_ASIO=0
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
