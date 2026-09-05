cmake_minimum_required(VERSION 3.16)
project(Can LANGUAGES CXX)
add_executable(can 
    can.cpp 
    ServerManager.cpp 
    LinkParser.cpp)
target_compile_features(can PRIVATE cxx_std_17)
