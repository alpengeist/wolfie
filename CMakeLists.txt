cmake_minimum_required(VERSION 4.2)
project(wolfie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

set(RTAUDIO_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_API_ASIO ON CACHE BOOL "" FORCE)
set(RTAUDIO_API_WASAPI OFF CACHE BOOL "" FORCE)
set(RTAUDIO_API_DS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG 6.0.1
)
FetchContent_MakeAvailable(rtaudio)

add_executable(wolfie WIN32
    src/main.cpp
    src/wolfie.rc
    src/wolfie_resources.h
    src/core/models.h
    src/core/text_utils.cpp
    src/core/text_utils.h
    src/audio/asio_service.cpp
    src/audio/asio_service.h
    src/audio/asio_audio_backend.cpp
    src/audio/asio_audio_backend.h
    src/audio/asio_sdk.cpp
    src/audio/asio_sdk.h
    src/audio/audio_backend.h
    src/audio/wasapi_audio_backend.cpp
    src/audio/wasapi_audio_backend.h
    src/audio/wasapi_service.cpp
    src/audio/wasapi_service.h
    src/audio/winmm_audio_backend.cpp
    src/audio/winmm_audio_backend.h
    src/measurement/measurement_controller.cpp
    src/measurement/measurement_controller.h
    src/measurement/filter_designer.cpp
    src/measurement/filter_designer.h
    src/measurement/filter_analysis.cpp
    src/measurement/filter_analysis.h
    src/measurement/filter_wav_export.cpp
    src/measurement/filter_wav_export.h
    src/measurement/dsp_utils.cpp
    src/measurement/dsp_utils.h
    src/measurement/phase_preparation.cpp
    src/measurement/phase_preparation.h
    src/measurement/room_simulator.cpp
    src/measurement/room_simulator.h
    src/measurement/response_analyzer.cpp
    src/measurement/response_analyzer.h
    src/measurement/response_smoother.cpp
    src/measurement/response_smoother.h
    src/measurement/stereo_diagnostics.cpp
    src/measurement/stereo_diagnostics.h
    src/measurement/sweet_spot_alignment.cpp
    src/measurement/sweet_spot_alignment.h
    src/measurement/sweep_generator.cpp
    src/measurement/sweep_generator.h
    src/measurement/target_curve_designer.cpp
    src/measurement/target_curve_designer.h
    src/measurement/waterfall_builder.cpp
    src/measurement/waterfall_builder.h
    src/persistence/app_state_repository.cpp
    src/persistence/app_state_repository.h
    src/persistence/filter_store_repository.cpp
    src/persistence/filter_store_repository.h
    src/persistence/microphone_calibration_repository.cpp
    src/persistence/microphone_calibration_repository.h
    src/persistence/room_simulation_repository.cpp
    src/persistence/room_simulation_repository.h
    src/persistence/wave_file_repository.cpp
    src/persistence/wave_file_repository.h
    src/persistence/workspace_repository.cpp
    src/persistence/workspace_repository.h
    src/ui/alignment_page.cpp
    src/ui/alignment_page.h
    src/ui/measurement_page.cpp
    src/ui/measurement_page.h
    src/ui/analysis_page.cpp
    src/ui/analysis_page.h
    src/ui/filters_page.cpp
    src/ui/filters_page.h
    src/ui/help_bubble.cpp
    src/ui/help_bubble.h
    src/ui/plot_graph.cpp
    src/ui/plot_graph.h
    src/ui/response_graph.cpp
    src/ui/response_graph.h
    src/ui/room_simulation_dialog.cpp
    src/ui/room_simulation_dialog.h
    src/ui/settings_dialog.cpp
    src/ui/settings_dialog.h
    src/ui/splash_screen.cpp
    src/ui/splash_screen.h
    src/ui/smoothing_page.cpp
    src/ui/smoothing_page.h
    src/ui/target_curve_graph.cpp
    src/ui/target_curve_graph.h
    src/ui/target_curve_page.cpp
    src/ui/target_curve_page.h
    src/ui/ui_theme.h
    src/ui/waterfall_graph.cpp
    src/ui/waterfall_graph.h
    src/wolfie_app.cpp
    src/wolfie_app.h
)

target_include_directories(wolfie PRIVATE src)
target_compile_definitions(wolfie PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(wolfie PRIVATE rtaudio comctl32 comdlg32 gdiplus msimg32 ole32 shell32 shlwapi uuid winmm)

option(WOLFIE_BUILD_TESTS "Build Wolfie tests" ON)
if(WOLFIE_BUILD_TESTS)
    enable_testing()

    add_library(wolfie_measurement_test_common OBJECT
        tests/filter_test_support.cpp
        tests/filter_test_support.h
        src/measurement/filter_designer.cpp
        src/measurement/filter_designer.h
        src/measurement/filter_analysis.cpp
        src/measurement/filter_analysis.h
        src/measurement/filter_wav_export.cpp
        src/measurement/filter_wav_export.h
        src/measurement/dsp_utils.cpp
        src/measurement/dsp_utils.h
        src/measurement/phase_preparation.cpp
        src/measurement/phase_preparation.h
        src/measurement/response_analyzer.cpp
        src/measurement/response_analyzer.h
        src/measurement/response_smoother.cpp
        src/measurement/response_smoother.h
        src/measurement/stereo_diagnostics.cpp
        src/measurement/stereo_diagnostics.h
        src/measurement/sweet_spot_alignment.cpp
        src/measurement/sweet_spot_alignment.h
        src/measurement/waterfall_builder.cpp
        src/measurement/waterfall_builder.h
        src/persistence/microphone_calibration_repository.cpp
        src/persistence/microphone_calibration_repository.h
        src/measurement/sweep_generator.cpp
        src/measurement/sweep_generator.h
        src/measurement/target_curve_designer.cpp
        src/measurement/target_curve_designer.h
        src/core/models.h
    )
    target_include_directories(wolfie_measurement_test_common PUBLIC src tests)

    function(add_wolfie_measurement_test target_name label)
        add_executable(${target_name}
            ${ARGN}
            tests/test_harness.h
            $<TARGET_OBJECTS:wolfie_measurement_test_common>
        )
        target_include_directories(${target_name} PRIVATE src tests)
        add_test(NAME ${target_name} COMMAND ${target_name})
        set_tests_properties(${target_name} PROPERTIES LABELS ${label})
    endfunction()

    add_wolfie_measurement_test(wolfie_measurement_pipeline_tests measurement
        tests/measurement_loopback_tests.cpp
    )
    add_wolfie_measurement_test(wolfie_filter_design_basics_tests filter_design
        tests/filter_design_basics_tests.cpp
    )
    add_wolfie_measurement_test(wolfie_phase_preparation_tests phase
        tests/phase_preparation_tests.cpp
    )
    add_wolfie_measurement_test(wolfie_excess_phase_mode_tests phase
        tests/excess_phase_mode_tests.cpp
    )
    add_wolfie_measurement_test(wolfie_mixed_phase_tests mixed_phase
        tests/mixed_phase_tests.cpp
    )
    add_wolfie_measurement_test(wolfie_filter_analysis_tests analysis
        tests/filter_analysis_tests.cpp
    )
    add_wolfie_measurement_test(wolfie_filter_export_tests export
        tests/filter_export_tests.cpp
    )

    add_custom_target(wolfie_measurement_tests
        DEPENDS
            wolfie_measurement_pipeline_tests
            wolfie_filter_design_basics_tests
            wolfie_phase_preparation_tests
            wolfie_excess_phase_mode_tests
            wolfie_mixed_phase_tests
            wolfie_filter_analysis_tests
            wolfie_filter_export_tests
    )

    add_executable(wolfie_workspace_filter_probe
        tools/workspace_filter_probe.cpp
        src/core/text_utils.cpp
        src/measurement/filter_designer.cpp
        src/measurement/filter_analysis.cpp
        src/measurement/filter_wav_export.cpp
        src/measurement/dsp_utils.cpp
        src/measurement/phase_preparation.cpp
        src/measurement/room_simulator.cpp
        src/measurement/response_analyzer.cpp
        src/measurement/response_smoother.cpp
        src/measurement/stereo_diagnostics.cpp
        src/measurement/sweet_spot_alignment.cpp
        src/measurement/sweep_generator.cpp
        src/measurement/target_curve_designer.cpp
        src/measurement/waterfall_builder.cpp
        src/persistence/filter_store_repository.cpp
        src/persistence/microphone_calibration_repository.cpp
        src/persistence/room_simulation_repository.cpp
        src/persistence/wave_file_repository.cpp
        src/persistence/workspace_repository.cpp
    )
    target_include_directories(wolfie_workspace_filter_probe PRIVATE src)
endif()
