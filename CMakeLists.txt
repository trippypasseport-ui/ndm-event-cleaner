cmake_minimum_required(VERSION 3.22)

project(NdmEventCleaner VERSION 0.1.0)

# --- JUCE : même version que PopVocal (compilation validée sur ton workflow) ---
include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(NdmEventCleaner
    COMPANY_NAME        "DemStudio"
    PLUGIN_MANUFACTURER_CODE Dstd            # même fabricant que PopVocal
    PLUGIN_CODE         Nevc                 # unique (PopVocal = Pvoc)
    FORMATS              AU                  # Logic Pro X 10.4 / High Sierra : AU v2 uniquement
    PRODUCT_NAME         "NDM Event Cleaner" # NOM DE TRAVAIL, à changer quand tu auras choisi le nom définitif
    IS_SYNTH              FALSE
    NEEDS_MIDI_INPUT      FALSE
    NEEDS_MIDI_OUTPUT     FALSE
    IS_MIDI_EFFECT        FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE
)

target_sources(NdmEventCleaner
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(NdmEventCleaner
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(NdmEventCleaner
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# --- Tests du cœur DSP (sans JUCE) : cmake -DNDM_BUILD_TESTS=ON ---
option(NDM_BUILD_TESTS "Compiler les tests du moteur DSP" OFF)
if(NDM_BUILD_TESTS)
    add_executable(parity_test tests/parity_test.cpp)
    add_executable(render_test tests/render_test.cpp)
    target_compile_features(parity_test PRIVATE cxx_std_17)
    target_compile_features(render_test PRIVATE cxx_std_17)
endif()
