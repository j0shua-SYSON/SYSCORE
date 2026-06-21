cmake_minimum_required(VERSION 3.16)
project(syscore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(syscore WIN32
    src/main.cpp
    src/metrics.cpp
    src/renderer.cpp
    src/config.cpp
    src/ai.cpp
)

target_compile_definitions(syscore PRIVATE
    UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

if (MINGW)
    target_link_options(syscore PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

target_link_libraries(syscore PRIVATE
    gdiplus gdi32 user32 pdh powrprof dxgi psapi ole32 oleaut32 advapi32 shell32 winhttp)
