cmake_minimum_required(VERSION 3.10)

project(pr5 C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(signals_game src/signals_game.c)
add_executable(pipe_game src/pipe_game.c)

foreach(target signals_game pipe_game)
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" OR CMAKE_C_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(UNIX)
        target_link_libraries(${target} PRIVATE rt)
    endif()
endforeach()
