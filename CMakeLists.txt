cmake_minimum_required(VERSION 3.20)

# Force Apple Clang on macOS
if(APPLE)
    set(CMAKE_C_COMPILER "/usr/bin/clang")
    set(CMAKE_CXX_COMPILER "/usr/bin/clang++")
endif()

project(BadApple)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(BadApple
        src/main.cpp
)

target_link_libraries(BadApple PRIVATE ${OpenCV_LIBS})
target_include_directories(BadApple PRIVATE ${OpenCV_INCLUDE_DIRS})