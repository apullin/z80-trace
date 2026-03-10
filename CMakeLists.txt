cmake_minimum_required(VERSION 3.13)

project(z80-trace C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(z80-trace
    src/io_plugin.cpp
    src/main.cpp
    src/z80_exec.c
    src/z80_state.c
    src/z80_disasm.c
)

target_include_directories(z80-trace PRIVATE include)

find_library(DL_LIBRARY dl)
if(DL_LIBRARY)
    target_link_libraries(z80-trace PRIVATE ${DL_LIBRARY})
endif()
