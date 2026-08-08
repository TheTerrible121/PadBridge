cmake_minimum_required(VERSION 3.20)
project(PadBridgeHost VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(padbridge_core
    src/protocol.cpp
    src/annex_b_reader.cpp
)
target_include_directories(padbridge_core PUBLIC src)

add_executable(padbridge_host
    src/main.cpp
    src/socket_client.cpp
    src/ffmpeg_source.cpp
    src/input_injector.cpp
)
target_link_libraries(padbridge_host PRIVATE padbridge_core)

if(WIN32)
    target_link_libraries(padbridge_host PRIVATE ws2_32 user32)
    target_compile_definitions(padbridge_host PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
        NTDDI_VERSION=0x0A000006
    )
endif()

if(MSVC)
    target_compile_options(padbridge_core PRIVATE /W4 /permissive-)
    target_compile_options(padbridge_host PRIVATE /W4 /permissive-)
else()
    target_compile_options(padbridge_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(padbridge_host PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_executable(protocol_tests tests/protocol_tests.cpp)
target_link_libraries(protocol_tests PRIVATE padbridge_core)
add_test(NAME protocol_tests COMMAND protocol_tests)
