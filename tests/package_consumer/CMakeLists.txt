cmake_minimum_required(VERSION 3.16)
project(video_server_package_consumer LANGUAGES CXX)

find_package(video-server CONFIG REQUIRED)

add_executable(video_server_package_consumer main.cpp)
target_link_libraries(video_server_package_consumer PRIVATE video_server::video_server)
