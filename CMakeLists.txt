cmake_minimum_required(VERSION 4.2)
project(wolfie LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(wolfie WIN32
    src/main.cpp
    src/wolfie_app.cpp
    src/wolfie_app.h
)

target_compile_definitions(wolfie PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(wolfie PRIVATE comctl32 comdlg32 ole32 shell32 shlwapi winmm)
