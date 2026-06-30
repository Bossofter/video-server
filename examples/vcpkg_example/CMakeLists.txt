cmake_minimum_required(VERSION 3.16)
project(video_server_vcpkg_example LANGUAGES CXX)

find_package(video-server CONFIG REQUIRED)

add_executable(video_server_vcpkg_example main.cpp)
target_compile_features(video_server_vcpkg_example PRIVATE cxx_std_17)
target_link_libraries(video_server_vcpkg_example PRIVATE video_server::video_server)
