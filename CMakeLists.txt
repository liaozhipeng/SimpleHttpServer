project(Ctt)
cmake_minimum_required(VERSION 3.5)
add_compile_options(-std=c++11)

find_package(Threads)

add_executable(gdb_test gdb_test.cpp)

target_link_libraries(gdb_test ${CMAKE_THREAD_LIBS_INIT})