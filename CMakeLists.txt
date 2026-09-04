cmake_minimum_required(VERSION 3.16)
project(Can LANGUAGES CXX)
add_executable(can can.cpp Server.cpp Server.hpp)
target_compile_features(can PRIVATE cxx_std_17)
