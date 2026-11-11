cmake_minimum_required(VERSION 3.16)
project(TWalkEnumAblation LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
add_executable(ablation_worker worker.cpp)
target_include_directories(ablation_worker PRIVATE include)
target_compile_options(ablation_worker PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-O3;-DNDEBUG>
  $<$<CXX_COMPILER_ID:MSVC>:/O2;/DNDEBUG>)
target_link_libraries(ablation_worker PRIVATE Threads::Threads)
set_target_properties(ablation_worker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
