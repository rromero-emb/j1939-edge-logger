cmake_minimum_required(VERSION 3.22)
project(j1939-edge-logger CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(J1939_SANITIZE "Build with AddressSanitizer + UBSan" OFF)
set(WARN -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror)

add_library(j1939core STATIC src/signals.cpp src/tp.cpp src/request.cpp src/logger.cpp src/journal.cpp)
target_include_directories(j1939core PUBLIC include)
target_compile_options(j1939core PRIVATE ${WARN})
if(J1939_SANITIZE)
    target_compile_options(j1939core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(j1939core PUBLIC -fsanitize=address,undefined)
endif()

add_executable(j1939log app/j1939log.cpp)
target_link_libraries(j1939log PRIVATE j1939core)
target_compile_options(j1939log PRIVATE -Wall -Wextra -Werror)

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
        URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    add_executable(unit_tests tests/test_id_signals.cpp tests/test_tp.cpp tests/test_journal.cpp tests/test_logger.cpp)
    target_link_libraries(unit_tests PRIVATE j1939core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(unit_tests)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME replay_sample_trace
        COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_SOURCE_DIR}/tests/test_replay.py $<TARGET_FILE:j1939log> ${CMAKE_SOURCE_DIR})
endif()

include(GNUInstallDirs)
install(TARGETS j1939log RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES traces/truck_pull_away.log DESTINATION ${CMAKE_INSTALL_DATADIR}/j1939-edge-logger/traces)
install(FILES LICENSE README.md DESTINATION ${CMAKE_INSTALL_DOCDIR})
