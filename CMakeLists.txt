cmake_minimum_required(VERSION 3.16)
project(video_server LANGUAGES CXX)

option(ENABLE_VIDEO_SERVER "Build core video server library" ON)
option(ENABLE_WEBRTC_BACKEND "Build WebRTC backend" ON)
option(BUILD_TESTING "Build tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
  set(_video_server_vcpkg_toolchain "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
  if(EXISTS "${_video_server_vcpkg_toolchain}")
    set(CMAKE_TOOLCHAIN_FILE "${_video_server_vcpkg_toolchain}" CACHE FILEPATH "CMake toolchain file")
    message(STATUS "Using vcpkg toolchain from VCPKG_ROOT: ${CMAKE_TOOLCHAIN_FILE}")
  endif()
endif()

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

  find_package(LibDataChannel CONFIG QUIET)
  find_package(libdatachannel CONFIG QUIET)
  find_package(libjuice CONFIG QUIET)
  find_package(juice CONFIG QUIET)
  find_package(unofficial-libsrtp CONFIG QUIET)
  find_package(libsrtp CONFIG QUIET)

  set(VIDEO_SERVER_LIBDATACHANNEL_TARGET "")
  if(TARGET LibDataChannel::LibDataChannel)
    set(VIDEO_SERVER_LIBDATACHANNEL_TARGET LibDataChannel::LibDataChannel)
  elseif(TARGET LibDataChannel::datachannel)
    set(VIDEO_SERVER_LIBDATACHANNEL_TARGET LibDataChannel::datachannel)
  elseif(TARGET libdatachannel::libdatachannel)
    set(VIDEO_SERVER_LIBDATACHANNEL_TARGET libdatachannel::libdatachannel)
  endif()

  set(VIDEO_SERVER_LIBJUICE_TARGET "")
  if(TARGET LibJuice::LibJuice)
    set(VIDEO_SERVER_LIBJUICE_TARGET LibJuice::LibJuice)
  elseif(TARGET libjuice::libjuice)
    set(VIDEO_SERVER_LIBJUICE_TARGET libjuice::libjuice)
  elseif(TARGET juice::juice)
    set(VIDEO_SERVER_LIBJUICE_TARGET juice::juice)
  endif()

  set(VIDEO_SERVER_LIBSRTP_TARGET "")
  if(TARGET unofficial::libsrtp::srtp2)
    set(VIDEO_SERVER_LIBSRTP_TARGET unofficial::libsrtp::srtp2)
  elseif(TARGET libsrtp2::libsrtp2)
    set(VIDEO_SERVER_LIBSRTP_TARGET libsrtp2::libsrtp2)
  elseif(TARGET srtp2)
    set(VIDEO_SERVER_LIBSRTP_TARGET srtp2)
  endif()

  foreach(_dep_target
      VIDEO_SERVER_LIBDATACHANNEL_TARGET
      VIDEO_SERVER_LIBJUICE_TARGET
      VIDEO_SERVER_LIBSRTP_TARGET)
    if("${${_dep_target}}" STREQUAL "")
      message(FATAL_ERROR
        "ENABLE_WEBRTC_BACKEND=ON requires libdatachannel, libjuice, and libsrtp via vcpkg. "
        "Configure with -DCMAKE_TOOLCHAIN_FILE=$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    endif()
  endforeach()

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
      ${VIDEO_SERVER_LIBDATACHANNEL_TARGET}
      ${VIDEO_SERVER_LIBJUICE_TARGET}
      ${VIDEO_SERVER_LIBSRTP_TARGET}
      spdlog::spdlog
      OpenSSL::SSL
      OpenSSL::Crypto
  )
endif()

if(BUILD_TESTING)
  include(CTest)
  set(TEST_SOURCES
    tests/test_main.cpp
    tests/test_core.cpp
    tests/test_transforms.cpp
    tests/test_synthetic.cpp
    tests/test_webrtc_http.cpp
  )

  add_executable(video_server_tests ${TEST_SOURCES})
  target_include_directories(video_server_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transforms
    ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
  )

  target_compile_definitions(video_server_tests PRIVATE VIDEO_SERVER_TEST_WEBRTC=$<BOOL:${ENABLE_WEBRTC_BACKEND}>)

  if(ENABLE_WEBRTC_BACKEND)
    target_link_libraries(video_server_tests PRIVATE video_server_webrtc)
  else()
    target_link_libraries(video_server_tests PRIVATE video_server_core)
  endif()

  add_test(NAME video_server_tests COMMAND video_server_tests)
endif()
