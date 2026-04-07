cmake_minimum_required(VERSION 3.16)
project(video_server VERSION 1.1.0 LANGUAGES CXX)

option(ENABLE_VIDEO_SERVER "Build core video server library" ON)
option(ENABLE_WEBRTC_BACKEND "Build WebRTC backend" ON)
option(BUILD_TESTING "Build tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

set(VIDEO_SERVER_PACKAGE_NAME "video-server")
set(VIDEO_SERVER_CMAKE_INSTALLDIR "${CMAKE_INSTALL_LIBDIR}/cmake/${VIDEO_SERVER_PACKAGE_NAME}")

if(ENABLE_WEBRTC_BACKEND AND NOT ENABLE_VIDEO_SERVER)
  message(FATAL_ERROR "ENABLE_WEBRTC_BACKEND requires ENABLE_VIDEO_SERVER.")
endif()

if(ENABLE_VIDEO_SERVER)
  find_package(FFMPEG MODULE REQUIRED COMPONENTS avcodec avutil swscale)

  add_library(video_server_ffmpeg INTERFACE)
  if(TARGET FFMPEG::avcodec AND TARGET FFMPEG::avutil AND TARGET FFMPEG::swscale)
    target_link_libraries(video_server_ffmpeg INTERFACE
      FFMPEG::avcodec
      FFMPEG::avutil
      FFMPEG::swscale
    )
  else()
    target_include_directories(video_server_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
    if(FFMPEG_LIBRARY_DIRS)
      target_link_directories(video_server_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
    endif()
    target_link_libraries(video_server_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})
  endif()

  add_library(video_server_core
    src/core/video_server_core.cpp
    src/core/video_pixel_format_utils.cpp
    src/core/video_types.cpp
    src/transforms/display_transform.cpp
    src/testing/synthetic_frame_generator.cpp
    src/pipeline/raw_to_h264_pipeline.cpp
  )

  target_include_directories(video_server_core
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core
      ${CMAKE_CURRENT_SOURCE_DIR}/src/transforms
      ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
  )

  target_link_libraries(video_server_core
    PUBLIC
      "$<BUILD_INTERFACE:video_server_ffmpeg>"
      "$<INSTALL_INTERFACE:video_server::ffmpeg>"
  )

  install(TARGETS video_server_core
    EXPORT video_serverTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
endif()

if(ENABLE_WEBRTC_BACKEND)
  find_package(spdlog CONFIG REQUIRED)
  find_package(tomlplusplus CONFIG REQUIRED)
  find_package(OpenSSL REQUIRED)
  find_package(LibDataChannel CONFIG REQUIRED)
  find_package(LibJuice CONFIG REQUIRED)
  find_package(libSRTP CONFIG REQUIRED)

  get_target_property(LIBDATACHANNEL_INTERFACE_LINK_LIBRARIES LibDataChannel::LibDataChannel INTERFACE_LINK_LIBRARIES)
  if(NOT LIBDATACHANNEL_INTERFACE_LINK_LIBRARIES)
    message(FATAL_ERROR
      "LibDataChannel::LibDataChannel did not expose interface link libraries. "
      "Install libdatachannel with media support enabled (vcpkg feature: libdatachannel[srtp]).")
  endif()

  string(REPLACE ";" "\n" LIBDATACHANNEL_INTERFACE_LINK_LIBRARIES_TEXT "${LIBDATACHANNEL_INTERFACE_LINK_LIBRARIES}")
  if(NOT LIBDATACHANNEL_INTERFACE_LINK_LIBRARIES_TEXT MATCHES "libSRTP::srtp2")
    message(FATAL_ERROR
      "Detected a libdatachannel build without media/SRTP support. "
      "This project requires libdatachannel[srtp] so rtc::Track::send can carry media.")
  endif()

  add_library(video_server_webrtc
    src/webrtc/managed_video_server.cpp
    src/webrtc/webrtc_video_server.cpp
    src/webrtc/webrtc_stream.cpp
    src/webrtc/signaling_server.cpp
    src/webrtc/http_api_server.cpp
    src/webrtc/frame_http_encoder.cpp
  )

  add_executable(video_server_nicegui_smoke_server
    examples/nicegui_smoke/synthetic_webrtc_smoke_server.cpp
  )

  add_library(video_server_soak
    src/testing/soak_test_framework.cpp
  )

  add_executable(video_server_soak_runner
    tools/video_server_soak_runner.cpp
  )

  target_include_directories(video_server_webrtc
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core
      ${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc
  )

  target_link_libraries(video_server_webrtc
    PUBLIC
      video_server_core
      spdlog::spdlog
      tomlplusplus::tomlplusplus
      OpenSSL::SSL
      OpenSSL::Crypto
      LibDataChannel::LibDataChannel
      LibJuice::LibJuice
      libSRTP::srtp2
  )

  install(TARGETS video_server_webrtc
    EXPORT video_serverTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )

  target_include_directories(video_server_nicegui_smoke_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
  )

  target_include_directories(video_server_soak PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transforms
    ${CMAKE_CURRENT_SOURCE_DIR}/src/webrtc
  )

  target_link_libraries(video_server_nicegui_smoke_server PRIVATE
    video_server_webrtc
  )

  target_link_libraries(video_server_soak
    PUBLIC
      video_server_webrtc
  )

  target_link_libraries(video_server_soak_runner PRIVATE
    video_server_soak
  )
endif()

if(ENABLE_VIDEO_SERVER)
  add_library(video_server INTERFACE)
  add_library(video_server::video_server ALIAS video_server)

  if(ENABLE_WEBRTC_BACKEND)
    target_link_libraries(video_server INTERFACE video_server_webrtc)
  else()
    target_link_libraries(video_server INTERFACE video_server_core)
  endif()

  install(TARGETS video_server
    EXPORT video_serverTargets
  )

  configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/video-serverConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/video-serverConfig.cmake"
    INSTALL_DESTINATION "${VIDEO_SERVER_CMAKE_INSTALLDIR}"
    NO_SET_AND_CHECK_MACRO
    NO_CHECK_REQUIRED_COMPONENTS_MACRO
  )

  write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/video-serverConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
  )

  install(DIRECTORY include/video_server
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h"
  )

  install(EXPORT video_serverTargets
    FILE video_serverTargets.cmake
    NAMESPACE video_server::
    DESTINATION ${VIDEO_SERVER_CMAKE_INSTALLDIR}
  )

  install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/video-serverConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/video-serverConfigVersion.cmake"
    DESTINATION "${VIDEO_SERVER_CMAKE_INSTALLDIR}"
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
    tests/test_managed_server.cpp
    tests/test_raw_pipeline.cpp
    tests/test_webrtc_http.cpp
    tests/test_webrtc_stream.cpp
    tests/test_soak_framework.cpp
  )

  add_executable(video_server_tests ${TEST_SOURCES})
  target_include_directories(video_server_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/transforms
    ${CMAKE_CURRENT_SOURCE_DIR}/src/testing
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${CMAKE_CURRENT_SOURCE_DIR}/src/pipeline
  )

  target_compile_definitions(video_server_tests PRIVATE VIDEO_SERVER_TEST_WEBRTC=$<BOOL:${ENABLE_WEBRTC_BACKEND}>)
  target_link_libraries(video_server_tests PRIVATE GTest::gtest)

  if(ENABLE_WEBRTC_BACKEND)
    target_link_libraries(video_server_tests PRIVATE video_server_webrtc video_server_soak)
  else()
    target_link_libraries(video_server_tests PRIVATE video_server_core)
  endif()

  include(GoogleTest)
  gtest_discover_tests(video_server_tests)
endif()
