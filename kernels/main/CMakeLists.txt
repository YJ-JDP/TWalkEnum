cmake_minimum_required(VERSION 3.16)
project(TWalkEnumAdaptedBaselines LANGUAGES CXX)
find_package(Threads REQUIRED)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
add_library(twalk_common STATIC common/src/worker.cpp)
target_include_directories(twalk_common PUBLIC common/include)
target_link_libraries(twalk_common PUBLIC Threads::Threads)
target_compile_options(twalk_common PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-O3;-DNDEBUG;-Wall;-Wextra;-Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/O2;/DNDEBUG;/W4>)
add_executable(main_worker apps/main.cpp)
target_link_libraries(main_worker PRIVATE twalk_common)
target_compile_options(main_worker PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-O3;-DNDEBUG;-Wall;-Wextra;-Wpedantic>
  $<$<CXX_COMPILER_ID:MSVC>:/O2;/DNDEBUG;/W4>)
set_target_properties(main_worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
