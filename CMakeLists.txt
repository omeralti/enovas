cmake_minimum_required(VERSION 3.16)
project(mpmc_demo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(app main.cpp)
target_compile_options(app PRIVATE -O2 -pthread)
target_compile_definitions(app PRIVATE LOG_DEBUG)
target_link_libraries(app PRIVATE pthread)

