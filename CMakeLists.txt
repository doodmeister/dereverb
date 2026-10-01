cmake_minimum_required(VERSION 3.24)

set(CMAKE_OSX_DEPLOYMENT_TARGET 10.13 CACHE STRING "")

project(VocalDeVerb
    VERSION 0.1.5.0
    DESCRIPTION "Real-time vocal dereverb VST3 plug-in"
    LANGUAGES CXX
)

set(DEREVERB_VST3SDK_SOURCE_DIR "" CACHE PATH "Path to a local Steinberg VST3 SDK checkout")
set(DEREVERB_VST3SDK_TAG "v3.8.0_build_66" CACHE STRING "Steinberg VST3 SDK tag used when fetching the SDK")

set(SMTG_ENABLE_VST3_HOSTING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VST3_PLUGIN_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SMTG_ENABLE_VSTGUI_SUPPORT ON CACHE BOOL "" FORCE)

if(vst3sdk_SOURCE_DIR AND NOT DEREVERB_VST3SDK_SOURCE_DIR)
    set(DEREVERB_VST3SDK_SOURCE_DIR "${vst3sdk_SOURCE_DIR}")
endif()

if(DEREVERB_VST3SDK_SOURCE_DIR)
    set(dereverb_sdk_dir "${DEREVERB_VST3SDK_SOURCE_DIR}")
    add_subdirectory("${DEREVERB_VST3SDK_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/vst3sdk")
else()
    include(FetchContent)
    FetchContent_Declare(vst3sdk
        GIT_REPOSITORY https://github.com/steinbergmedia/vst3sdk.git
        GIT_TAG        ${DEREVERB_VST3SDK_TAG}
        GIT_SHALLOW    TRUE
        GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_Populate(vst3sdk)
    set(dereverb_sdk_dir "${vst3sdk_SOURCE_DIR}")
    add_subdirectory("${vst3sdk_SOURCE_DIR}" "${vst3sdk_BINARY_DIR}")
endif()

smtg_enable_vst3_sdk()

set(dereverb_sources
    README.md
    source/cids.h
    source/controller.cpp
    source/controller.h
    source/factory.cpp
    source/parameters.h
    source/processor.cpp
    source/processor.h
    resource/vocaldeverb.uidesc
    source/state.cpp
    source/state.h
    source/version.h
)

smtg_add_vst3plugin(VocalDeVerb ${dereverb_sources})

target_compile_features(VocalDeVerb
    PUBLIC
        cxx_std_17
)

target_link_libraries(VocalDeVerb
    PRIVATE
        sdk
        vstgui_support
)

smtg_target_configure_version_file(VocalDeVerb)
smtg_target_add_plugin_resources(VocalDeVerb
    RESOURCES
        resource/vocaldeverb.uidesc
        resource/background_2x.png
)

if(SMTG_WIN)
    target_sources(VocalDeVerb PRIVATE resource/win32resource.rc)
    if(MSVC)
        set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT VocalDeVerb)
    endif()
elseif(SMTG_MAC)
    smtg_target_set_bundle(VocalDeVerb
        BUNDLE_IDENTIFIER com.local.vocaldeverb
        COMPANY_NAME "Local Audio"
    )
endif()

# --- Offline DSP test harness -------------------------------------------------
# Builds a small console executable that drives the real processor through the
# SDK's audio-processing path (no plugin host needed). Reuses the SDK targets
# configured above, so it links the prebuilt static libs.
option(DEREVERB_BUILD_TESTS "Build the offline DSP test harness" OFF)
if(DEREVERB_BUILD_TESTS)
    add_executable(dsp_tests
        tests/dsp_tests.cpp
        source/processor.cpp
        source/state.cpp
        "${dereverb_sdk_dir}/public.sdk/source/common/memorystream.cpp"
    )
    target_compile_features(dsp_tests PRIVATE cxx_std_17)
    target_include_directories(dsp_tests PRIVATE source)
    target_compile_definitions(dsp_tests PRIVATE
        $<$<CONFIG:Debug>:DEVELOPMENT=1>
        $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>
    )
    target_link_libraries(dsp_tests
        PRIVATE
            sdk
            sdk_common
            sdk_hosting
            base
            pluginterfaces
    )

    enable_testing()
    add_test(NAME dsp_tests COMMAND dsp_tests)
endif()
