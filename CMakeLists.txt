cmake_minimum_required(VERSION 3.16)
project(Can LANGUAGES CXX)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(Threads REQUIRED)
add_library(can_core
    ServerManager.cpp 
    LinkParser.cpp
    XrayConfigBuilder.cpp
    XrayProcess.cpp
    CliOptions.cpp
    SystemCommand.cpp
    NetworkTransaction.cpp
    TunManager.cpp)
target_compile_features(can_core PUBLIC cxx_std_17)
target_include_directories(can_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(can_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(APPLE)
    target_link_libraries(can_core PUBLIC "-framework SystemConfiguration" "-framework CoreFoundation")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(can_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_executable(can can.cpp)
target_link_libraries(can PRIVATE can_core)
include(CTest)
if(BUILD_TESTING)
    add_executable(can_tests tests/CanTests.cpp)
    target_link_libraries(can_tests PRIVATE can_core)
    add_test(NAME can_tests COMMAND can_tests)
    set_tests_properties(can_tests PROPERTIES TIMEOUT 30)
endif()
