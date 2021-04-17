cmake_minimum_required(VERSION 3.8 FATAL_ERROR)
project(pennant_cmake LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11 CACHE STRING "C++ standard to use")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(MPI COMPONENTS CXX REQUIRED)
find_package(OpenMP REQUIRED)

add_subdirectory(src)

# The main pennant executable.
add_executable(pennant ${PROJECT_SOURCE_DIR}/src/main.cc)
target_link_libraries(pennant PRIVATE libpennant)
