cmake_minimum_required(VERSION 3.16)
project(SerialAnswer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(answer answer.cpp
        main.cpp)
target_compile_options(answer PRIVATE -Wall -Wextra)
