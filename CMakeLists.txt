cmake_minimum_required(VERSION 3.16)
project(logicforge CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# e.g. -DLF_SANITIZE=address,undefined  or  -DLF_SANITIZE=thread
set(LF_SANITIZE "" CACHE STRING "sanitizers to build with")

add_compile_options(-Wall -Wextra)
if(LF_SANITIZE)
  add_compile_options(-fsanitize=${LF_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${LF_SANITIZE})
endif()

find_package(Threads REQUIRED)

add_library(logicforge
  src/ir.cpp
  src/parser.cpp
  src/elaborate.cpp
  src/passes.cpp
  src/flow.cpp
  src/sim.cpp
  src/writer.cpp)
target_include_directories(logicforge PUBLIC src)
target_link_libraries(logicforge PUBLIC Threads::Threads)

add_executable(lfc src/main.cpp)
target_link_libraries(lfc PRIVATE logicforge)

enable_testing()
add_subdirectory(tests)
