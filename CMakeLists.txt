cmake_minimum_required(VERSION 3.22)

project(OrchMerge VERSION 0.1.0)

# Path to your JUCE folder (same as the rest of the Orch family).
add_subdirectory("C:/JUCE/JUCE" JUCE)

juce_add_plugin(OrchMerge
    COMPANY_NAME "JohnPascu"
    PLUGIN_MANUFACTURER_CODE Jpas
    PLUGIN_CODE Omrg

    FORMATS VST3 Standalone

    PRODUCT_NAME "OrchMerge"
    PLUGIN_NAME "OrchMerge"
    BUNDLE_ID "com.johnpascu.OrchMerge"

    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT TRUE
    IS_MIDI_EFFECT TRUE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_REPLACES_VST2 FALSE

    # Goes on ~25 source tracks plus one Hub - convenient to have it land in the
    # shared VST3 folder automatically. Flip to FALSE if your build shell is not
    # elevated (the post-build copy into C:/Program Files/Common Files/VST3
    # needs write permission there) and copy the .vst3 by hand instead.
    COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(OrchMerge)

target_sources(OrchMerge
    PRIVATE
        Source/OrchMergeProcessor.cpp
        Source/OrchMergeEditor.cpp
        Source/OrchMergeLink.cpp
)

# Regenerated on EVERY build (add_custom_target has no file-based up-to-date
# check, unlike add_custom_command(OUTPUT ...)) so the editor's build label
# always reflects the actual latest build, even when only a different .cpp
# changed and this one wasn't recompiled for its own sake.
set(ORCHMERGE_BUILD_TIMESTAMP_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/OrchMergeBuildTimestamp.h")
add_custom_target(OrchMergeStampBuildTime ALL
    COMMAND ${CMAKE_COMMAND}
            -DHEADER_PATH=${ORCHMERGE_BUILD_TIMESTAMP_HEADER}
            -DMACRO_NAME=ORCHMERGE_BUILD_TIMESTAMP
            -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateBuildTimestamp.cmake"
    BYPRODUCTS "${ORCHMERGE_BUILD_TIMESTAMP_HEADER}"
    COMMENT "Stamping OrchMerge build timestamp"
)
add_dependencies(OrchMerge OrchMergeStampBuildTime)
target_include_directories(OrchMerge PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

target_compile_definitions(OrchMerge
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(OrchMerge
    PRIVATE
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_gui_extra

    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if (TARGET OrchMerge_VST3)
    target_compile_definitions(OrchMerge_VST3
        PRIVATE
            JUCE_VST3_CAN_REPLACE_VST2=0
    )
endif()
