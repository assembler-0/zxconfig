cmake_minimum_required(VERSION 3.31)
project(zxconfig CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(ftxui 7 CONFIG REQUIRED)

set(ZXCONFIG_SOURCES
    src/ast.cpp
    src/cache.cpp
    src/eval.cpp
    src/fuzzy.cpp
    src/generator.cpp
    src/hash.cpp
    src/lexer.cpp
    src/io.cpp
    src/log.cpp
    src/parser.cpp
    src/source.cpp
    src/text.cpp
    src/token.cpp
    src/tui/app.cpp
)

add_library(zxconfig_core STATIC ${ZXCONFIG_SOURCES})
target_include_directories(zxconfig_core PUBLIC include)
target_compile_options(zxconfig_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(zxconfig_core PUBLIC ftxui::screen ftxui::dom ftxui::component)

add_executable(zxconfig src/main.cpp)
target_compile_options(zxconfig PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(zxconfig PRIVATE zxconfig_core)

include(GNUInstallDirs)
install(TARGETS zxconfig RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(BUILD_TESTING)
    find_package(GTest CONFIG REQUIRED)

    add_executable(zxconfig_tests
        tests/cache_test.cpp
        tests/error_test.cpp
        tests/eval_test.cpp
        tests/fuzzy_test.cpp
        tests/generator_test.cpp
        tests/io_test.cpp
        tests/lexer_test.cpp
        tests/log_test.cpp
        tests/parser_test.cpp
        tests/source_test.cpp
        tests/text_test.cpp
        tests/types_test.cpp)
    target_compile_options(zxconfig_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(zxconfig_tests PRIVATE zxconfig_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(zxconfig_tests)
endif()
