cmake_minimum_required(VERSION 3.22)

# Must be set BEFORE project(): otherwise project() creates an empty cache entry
# and the binary gets minos = the build machine's macOS (it was 26.0!).
# 11.0 = minimum for std::atomic::wait (libc++) and CoreAudio workgroups.
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
# Universal binary: runs natively on Apple Silicon and Intel Macs.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Target architectures")

project(StageRack VERSION 0.4.0 LANGUAGES C CXX OBJC OBJCXX)

# Pre-release label ("beta", "" for a final release). The full version ("0.2.0-beta") must match
# the GitHub release tag ("v0.2.0-beta"): the built-in updater compares them.
set(STAGERACK_VERSION_LABEL "beta")
if(STAGERACK_VERSION_LABEL)
    set(STAGERACK_FULL_VERSION "${PROJECT_VERSION}-${STAGERACK_VERSION_LABEL}")
else()
    set(STAGERACK_FULL_VERSION "${PROJECT_VERSION}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Optional: path to a folder containing pluginterfaces/vst2.x/aeffect.h + aeffectx.h
# (Steinberg no longer distributes the VST2 SDK, so it cannot be bundled here.)
set(STAGERACK_VST2_SDK_PATH "" CACHE PATH "Folder containing pluginterfaces/vst2.x (enables VST2 hosting)")

add_subdirectory(libs/JUCE)

juce_add_gui_app(StageRack
    PRODUCT_NAME "StageRack"
    BUNDLE_ID "com.bio.stagerack"
    COMPANY_NAME "bio"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "StageRack needs audio input access to process live audio."
    # Lets Finder show "requires macOS 11" instead of a launch failure on older systems.
    PLIST_TO_MERGE "<plist><dict><key>LSMinimumSystemVersion</key><string>${CMAKE_OSX_DEPLOYMENT_TARGET}</string></dict></plist>")

juce_generate_juce_header(StageRack)

target_sources(StageRack PRIVATE
    Source/Main.cpp
    Source/Engine/WorkerPool.cpp
    Source/Engine/Rack.cpp
    Source/Engine/AudioEngine.cpp
    Source/Engine/Session.cpp
    Source/Engine/Autosave.cpp
    Source/Engine/HopAligner.cpp
    Source/Engine/CoreWakeAssist.cpp
    Source/Plugins/PluginManager.cpp
    Source/Plugins/CLAPPluginFormat.mm
    Source/UI/MainComponent.cpp
    Source/UI/RackComponent.cpp
    Source/UI/EditController.cpp
    Source/UI/SceneListComponent.cpp
    Source/UI/TopBar.cpp
    Source/UI/PluginWindow.cpp
    Source/UI/PluginThumbnail.mm
    Source/UI/PluginMeters.mm
    Source/UI/PluginBrowser.cpp
    Source/UI/PluginManagerWindow.cpp
    Source/Update/Updater.cpp)

target_include_directories(StageRack PRIVATE Source libs/clap/include
    libs/JUCE/modules/juce_audio_processors_headless/format_types/VST3_SDK)   # VST3 interfaces only (meter flags)

set(STAGERACK_HOST_VST 0)
if(STAGERACK_VST2_SDK_PATH)
    if(EXISTS "${STAGERACK_VST2_SDK_PATH}/pluginterfaces/vst2.x/aeffectx.h")
        set(STAGERACK_HOST_VST 1)
        target_include_directories(StageRack PRIVATE "${STAGERACK_VST2_SDK_PATH}")
        message(STATUS "StageRack: VST2 hosting enabled")
    else()
        message(WARNING "STAGERACK_VST2_SDK_PATH set but aeffectx.h not found, VST2 disabled")
    endif()
endif()

target_compile_definitions(StageRack PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:StageRack,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:StageRack,JUCE_VERSION>"
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_LV2=1
    JUCE_PLUGINHOST_VST=${STAGERACK_HOST_VST}
    JUCE_DISPLAY_SPLASH_SCREEN=0
    DONT_SET_USING_JUCE_NAMESPACE=1
    STAGERACK_VERSION="${STAGERACK_FULL_VERSION}")

# CPU tuning only for the arm64 slice (every Apple Silicon Mac is >= M1).
# The x86_64 slice keeps the default baseline so it runs on every Intel Mac with macOS 11+.
if("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _stagerack_num_archs)
    if(_stagerack_num_archs GREATER 1)
        target_compile_options(StageRack PRIVATE "$<$<CONFIG:Release>:SHELL:-Xarch_arm64 -mcpu=apple-m1>")
    else()
        target_compile_options(StageRack PRIVATE $<$<CONFIG:Release>:-mcpu=apple-m1>)
    endif()
endif()

target_link_libraries(StageRack PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
