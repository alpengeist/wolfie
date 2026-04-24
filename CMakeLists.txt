cmake_minimum_required(VERSION 4.2)
project(wolfie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wolfie WIN32
    src/main.cpp
    src/core/models.h
    src/core/text_utils.cpp
    src/core/text_utils.h
    src/audio/asio_service.cpp
    src/audio/asio_service.h
    src/audio/audio_backend.h
    src/audio/winmm_audio_backend.cpp
    src/audio/winmm_audio_backend.h
    src/measurement/measurement_controller.cpp
    src/measurement/measurement_controller.h
    src/measurement/response_analyzer.cpp
    src/measurement/response_analyzer.h
    src/measurement/response_smoother.cpp
    src/measurement/response_smoother.h
    src/measurement/sweep_generator.cpp
    src/measurement/sweep_generator.h
    src/measurement/target_curve_designer.cpp
    src/measurement/target_curve_designer.h
    src/persistence/app_state_repository.cpp
    src/persistence/app_state_repository.h
    src/persistence/workspace_repository.cpp
    src/persistence/workspace_repository.h
    src/ui/measurement_page.cpp
    src/ui/measurement_page.h
    src/ui/response_graph.cpp
    src/ui/response_graph.h
    src/ui/settings_dialog.cpp
    src/ui/settings_dialog.h
    src/ui/smoothing_page.cpp
    src/ui/smoothing_page.h
    src/ui/target_curve_graph.cpp
    src/ui/target_curve_graph.h
    src/ui/target_curve_page.cpp
    src/ui/target_curve_page.h
    src/ui/ui_theme.h
    src/wolfie_app.cpp
    src/wolfie_app.h
)

target_include_directories(wolfie PRIVATE src)
target_compile_definitions(wolfie PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(wolfie PRIVATE comctl32 comdlg32 ole32 shell32 shlwapi winmm)

option(WOLFIE_BUILD_TESTS "Build Wolfie tests" ON)
if(WOLFIE_BUILD_TESTS)
    enable_testing()
    add_executable(wolfie_measurement_tests
        tests/measurement_loopback_tests.cpp
        src/measurement/response_analyzer.cpp
        src/measurement/response_analyzer.h
        src/measurement/sweep_generator.cpp
        src/measurement/sweep_generator.h
        src/core/models.h
    )
    target_include_directories(wolfie_measurement_tests PRIVATE src)
    add_test(NAME wolfie_measurement_tests COMMAND wolfie_measurement_tests)
endif()
