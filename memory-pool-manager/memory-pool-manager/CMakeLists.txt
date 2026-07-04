cmake_minimum_required(VERSION 3.20)
project(memory-pool-manager CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(mempool INTERFACE)
target_include_directories(mempool INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(mempool INTERFACE cxx_std_20)

set(WARN_FLAGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion)

# Sanitizer toggles: -DSANITIZE=address | thread | undefined
if(SANITIZE)
    add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=${SANITIZE})
endif()

find_package(Threads REQUIRED)

add_executable(unit_tests tests/test_main.cpp)
target_link_libraries(unit_tests PRIVATE mempool Threads::Threads)
target_compile_options(unit_tests PRIVATE ${WARN_FLAGS})
target_compile_definitions(unit_tests PRIVATE MEMPOOL_STATS=1)

add_executable(stress_test tests/stress_test.cpp)
target_link_libraries(stress_test PRIVATE mempool Threads::Threads)
target_compile_options(stress_test PRIVATE ${WARN_FLAGS})
target_compile_definitions(stress_test PRIVATE MEMPOOL_STATS=1)

add_executable(benchmark bench/benchmark.cpp)
target_link_libraries(benchmark PRIVATE mempool Threads::Threads)
target_compile_options(benchmark PRIVATE ${WARN_FLAGS} -O2)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
add_test(NAME stress COMMAND stress_test 8 100000)
