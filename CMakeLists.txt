cmake_minimum_required(VERSION 2.8)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -g3 -Wall -Werror -Wno-sign-compare -O3")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -g3 -Wall -Werror -Wno-sign-compare -O3")

add_executable(test_basic.testbin
  src/test/test_basic.cc)

add_executable(test_benchmark.testbin
  src/test/test_benchmark.cc)

enable_testing()

add_test(test_basic bin/test_basic.testbin)
# CMake test support is a total shitshow. The test targets don't have
# a dependency on the test binary, and it's in fact impossible to add
# any dependencies at all for a test target. This means that "make
# test" will never do the right thing, but just run some random
# previously compiled versions.
#
# All the workarounds suck. This one seems to suck the least; add a
# test that builds the other tests, and then add this non-standard
# test-only sequencing dependency to force that test to run first.
add_test(build_test_code "${CMAKE_COMMAND}" --build ${CMAKE_BINARY_DIR} --target all)
set_tests_properties(test_basic PROPERTIES DEPENDS build_test_code)


