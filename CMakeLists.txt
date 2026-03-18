cmake_minimum_required(VERSION 3.16)
project(video_server LANGUAGES CXX)

option(ENABLE_VIDEO_SERVER "Build core video server library" ON)
option(ENABLE_WEBRTC_BACKEND "Build WebRTC backend" ON)
option(BUILD_TESTING "Build tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ENABLE_VIDEO_SERVER)
  add_library(video_server_core
    src/core/video_server_core.cpp
    src/core/video_types.cpp
    src/transforms/display_transform.cpp
    src/testing/synthetic_frame_generator.cpp
  )

  target_include_directories(video_server_core
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core
      ${CMAKE_CURRENT_SOURCE_DIR}/src/transforms
      ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
  )
endif()

if(ENABLE_WEBRTC_BACKEND)
  find_package(spdlog CONFIG REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(LibDataChannel CONFIG REQUIRED)
  find_package(LibJuice CONFIG REQUIRED)
  find_package(libSRTP CONFIG REQUIRED)

  add_library(video_server_webrtc
    src/webrtc/webrtc_video_server.cpp
    src/webrtc/webrtc_stream.cpp
    src/webrtc/signaling_server.cpp
    src/webrtc/http_api_server.cpp
    src/webrtc/frame_http_encoder.cpp
  )

  target_include_directories(video_server_webrtc
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core
      ${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc
  )

  target_link_libraries(video_server_webrtc
    PUBLIC
      video_server_core
      spdlog::spdlog
      OpenSSL::SSL
      OpenSSL::Crypto
      LibDataChannel::LibDataChannel
      LibJuice::LibJuice
      libSRTP::srtp2
  )
endif()

if(BUILD_TESTING)
  include(CTest)
  find_package(GTest CONFIG REQUIRED)
  set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_core.cpp
    tests/test_transforms.cpp
    tests/test_synthetic.cpp
    tests/test_video_types.cpp
    tests/test_pipeline.cpp
    tests/test_webrtc_http.cpp
  )

  add_executable(video_server_tests ${TEST_SOURCES})
  target_include_directories(video_server_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transforms
    ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  )

  target_compile_definitions(video_server_tests PRIVATE VIDEO_SERVER_TEST_WEBRTC=$<BOOL:${ENABLE_WEBRTC_BACKEND}>)
  target_link_libraries(video_server_tests PRIVATE GTest::gtest)

  if(ENABLE_WEBRTC_BACKEND)
    target_link_libraries(video_server_tests PRIVATE video_server_webrtc)
  else()
    target_link_libraries(video_server_tests PRIVATE video_server_core)
  endif()

  include(GoogleTest)
  gtest_discover_tests(video_server_tests)
  add_test(NAME video_server_tests COMMAND video_server_tests)
endif()
