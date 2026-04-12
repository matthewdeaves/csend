cmake_minimum_required(VERSION 3.14)
project(csend C)

include(FetchContent)

# Platform detection (same as peertalk)
if(NOT DEFINED PT_PLATFORM)
    if(CMAKE_SYSTEM_NAME MATCHES "Retro68")
        set(PT_PLATFORM "MACTCP")
    elseif(CMAKE_SYSTEM_NAME MATCHES "RetroPPC")
        set(PT_PLATFORM "OT")
    else()
        set(PT_PLATFORM "POSIX")
    endif()
endif()

message(STATUS "csend platform: ${PT_PLATFORM}")

# --- clog dependency ---
# Use local dir if provided, otherwise fetch from GitHub
if(NOT DEFINED CLOG_DIR AND DEFINED ENV{CLOG_DIR})
    set(CLOG_DIR "$ENV{CLOG_DIR}")
endif()

if(DEFINED CLOG_DIR)
    message(STATUS "Using local clog: ${CLOG_DIR}")
    set(CLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CLOG_DIR} ${CMAKE_BINARY_DIR}/clog)
else()
    message(STATUS "Fetching clog from GitHub")
    FetchContent_Declare(clog
        GIT_REPOSITORY https://github.com/matthewdeaves/clog.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    set(CLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(clog)
endif()

# --- peertalk dependency ---
# Use local dir if provided, otherwise fetch from GitHub
if(NOT DEFINED PEERTALK_DIR AND DEFINED ENV{PEERTALK_DIR})
    set(PEERTALK_DIR "$ENV{PEERTALK_DIR}")
endif()

set(PEERTALK_BUILD_TESTS OFF CACHE BOOL "" FORCE)

if(DEFINED PEERTALK_DIR)
    message(STATUS "Using local peertalk: ${PEERTALK_DIR}")
    add_subdirectory(${PEERTALK_DIR} ${CMAKE_BINARY_DIR}/peertalk)
else()
    message(STATUS "Fetching peertalk from GitHub")
    FetchContent_Declare(peertalk
        GIT_REPOSITORY https://github.com/matthewdeaves/peertalk.git
        GIT_TAG        main
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(peertalk)
    set(PEERTALK_DIR "${peertalk_SOURCE_DIR}")
endif()

if(PT_PLATFORM STREQUAL "POSIX")
    # ---- POSIX build ----
    add_executable(csend-posix
        posix/main.c
        posix/peertalk_bridge.c
        posix/commands.c
        posix/ui_factory.c
        posix/ui_terminal.c
        posix/ui_terminal_interactive.c
        posix/ui_terminal_machine.c
        posix/signal_handler.c
        posix/test.c
        shared/test.c
    )

    target_include_directories(csend-posix PRIVATE
        posix
        shared
        ${PEERTALK_DIR}/include
    )

    target_link_libraries(csend-posix PRIVATE peertalk clog pthread)

    set_target_properties(csend-posix PROPERTIES C_STANDARD 11)
    target_compile_options(csend-posix PRIVATE -Wall -Wextra)

else()
    # ---- Classic Mac build (MacTCP or OT - peertalk handles the difference) ----
    add_application(csend-mac
        classic_mac/main.c
        classic_mac/peertalk_bridge.c
        classic_mac/dialog.c
        classic_mac/test.c
        shared/classic_mac/ui/dialog_input.c
        shared/classic_mac/ui/dialog_messages.c
        shared/classic_mac/ui/dialog_peerlist.c
        shared/test.c
        MPW_resources/csend.r
    )

    target_include_directories(csend-mac PRIVATE
        classic_mac
        shared
        shared/classic_mac
        shared/classic_mac/ui
        ${PEERTALK_DIR}/include
    )

    target_compile_definitions(csend-mac PRIVATE __CLASSIC_MAC__)
    target_link_libraries(csend-mac PRIVATE peertalk clog)

    # Suppress Classic Mac header warnings
    target_compile_options(csend-mac PRIVATE -Wall -Wextra
        -Wno-unknown-pragmas -Wno-unused-parameter)

    # OT libraries (when PT_PLATFORM=OT)
    if(PT_PLATFORM STREQUAL "OT")
        if(CMAKE_SYSTEM_NAME MATCHES "Retro68")
            find_library(OT_APP_LIB OpenTransportApp)
            find_library(OT_LIB OpenTransport)
            find_library(OT_INET_LIB OpenTptInet)
        else()
            find_library(OT_APP_LIB OpenTransportAppPPC)
            find_library(OT_LIB OpenTransportLib)
            find_library(OT_INET_LIB OpenTptInternetLib)
        endif()
        foreach(LIB OT_APP_LIB OT_LIB OT_INET_LIB)
            if(${LIB})
                target_link_libraries(csend-mac PRIVATE ${${LIB}})
            endif()
        endforeach()
    endif()
endif()
