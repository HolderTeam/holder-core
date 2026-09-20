cmake_minimum_required(VERSION 3.22)
project(holder_installed_consumer LANGUAGES CXX)

find_package(holder CONFIG REQUIRED)
add_executable(holder_installed_consumer main.cpp)
target_link_libraries(holder_installed_consumer PRIVATE Holder::Core)

enable_testing()
add_test(NAME installed_core_api COMMAND holder_installed_consumer)
