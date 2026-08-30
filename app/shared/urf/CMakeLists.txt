cmake_minimum_required(VERSION 3.20)
project(urf CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(urf
  src/packbits.cpp
  src/writer.cpp
  src/validate.cpp
)
target_include_directories(urf PUBLIC include)
target_compile_options(urf PRIVATE -Wall -Wextra -Werror)

enable_testing()

add_executable(urf_tests
  tests/test_main.cpp
  tests/test_packbits.cpp
  tests/test_writer.cpp
  tests/test_validate.cpp
)
target_link_libraries(urf_tests PRIVATE urf)
# -include 把测试框架头强制注入每个测试 .cpp，省得每个文件都写一遍 include。
# clang / gcc 都支持；本项目的三个构建环境（开发机、Xcode、NDK）用的都是这两者。
target_compile_options(urf_tests PRIVATE -Wall -Wextra
  -include ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_framework.h)

add_test(NAME urf_tests COMMAND urf_tests)
