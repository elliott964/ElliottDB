cmake_minimum_required(VERSION 3.16)
project(ElliottDB C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(EDB_STATIC "Link statically into a single standalone binary" ON)

set(EDB_SOURCES
    src/edb_util.c
    src/config.c
    src/metrics.c
    src/pager.c
    src/wal.c
    src/index.c
    src/storage.c
    src/sql_parser.c
    src/sql_exec.c
    src/protocol.c
    src/server.c
    src/cli.c
    src/bench.c
    src/main.c
)

if(WIN32)
    list(APPEND EDB_SOURCES src/pal_win.c src/reactor_win.c)
else()
    list(APPEND EDB_SOURCES src/pal_linux.c src/reactor_linux.c)
endif()

add_executable(elliottdb ${EDB_SOURCES})
target_include_directories(elliottdb PRIVATE src)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    target_compile_options(elliottdb PRIVATE $<$<NOT:$<CONFIG:Debug>>:/O2> /W4 /D_CRT_SECURE_NO_WARNINGS)
    if(EDB_STATIC)
        set_property(TARGET elliottdb PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    target_link_libraries(elliottdb PRIVATE ws2_32)
else()
    target_compile_definitions(elliottdb PRIVATE _GNU_SOURCE)
    target_compile_options(elliottdb PRIVATE -O3 -Wall -Wextra -Werror=implicit-function-declaration -pthread)
    target_link_libraries(elliottdb PRIVATE pthread m)
    if(EDB_STATIC AND NOT APPLE)
        target_link_options(elliottdb PRIVATE -static)
    endif()
endif()
