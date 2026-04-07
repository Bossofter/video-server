
####### Expanded from @PACKAGE_INIT@ by configure_package_config_file() #######
####### Any changes to this file will be overwritten by the next CMake run ####
####### The input file was video-serverConfig.cmake.in                            ########

get_filename_component(PACKAGE_PREFIX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../" ABSOLUTE)

####################################################################################

include(CMakeFindDependencyMacro)

set(_video_server_ffmpeg_found FALSE)
set(_video_server_ffmpeg_uses_pkg_config FALSE)

find_package(FFMPEG QUIET COMPONENTS avcodec avutil swscale)
if(TARGET FFMPEG::avcodec AND TARGET FFMPEG::avutil AND TARGET FFMPEG::swscale)
  set(_video_server_ffmpeg_found TRUE)
elseif(FFMPEG_FOUND AND FFMPEG_LIBRARIES)
  set(_video_server_ffmpeg_found TRUE)
endif()

if(NOT _video_server_ffmpeg_found)
  find_path(_video_server_ffmpeg_module_dir
    NAMES FindFFMPEG.cmake
    PATH_SUFFIXES share/ffmpeg
  )

  if(_video_server_ffmpeg_module_dir)
    list(PREPEND CMAKE_MODULE_PATH "${_video_server_ffmpeg_module_dir}")
    find_package(FFMPEG MODULE QUIET COMPONENTS avcodec avutil swscale)
    list(REMOVE_ITEM CMAKE_MODULE_PATH "${_video_server_ffmpeg_module_dir}")
  endif()

  if(TARGET FFMPEG::avcodec AND TARGET FFMPEG::avutil AND TARGET FFMPEG::swscale)
    set(_video_server_ffmpeg_found TRUE)
  elseif(FFMPEG_FOUND AND FFMPEG_LIBRARIES)
    set(_video_server_ffmpeg_found TRUE)
  endif()
endif()

if(NOT _video_server_ffmpeg_found)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)
  set(_video_server_ffmpeg_found TRUE)
  set(_video_server_ffmpeg_uses_pkg_config TRUE)
endif()

if(NOT TARGET video_server::ffmpeg)
  add_library(video_server::ffmpeg INTERFACE IMPORTED)

  if(TARGET FFMPEG::avcodec AND TARGET FFMPEG::avutil AND TARGET FFMPEG::swscale)
    set_target_properties(video_server::ffmpeg PROPERTIES
      INTERFACE_LINK_LIBRARIES "FFMPEG::avcodec;FFMPEG::avutil;FFMPEG::swscale"
    )
  elseif(_video_server_ffmpeg_uses_pkg_config)
    set_target_properties(video_server::ffmpeg PROPERTIES
      INTERFACE_LINK_LIBRARIES "PkgConfig::FFMPEG"
    )
  else()
    set_target_properties(video_server::ffmpeg PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${FFMPEG_LIBRARIES}"
    )

    if(FFMPEG_LIBRARY_DIRS)
      set_target_properties(video_server::ffmpeg PROPERTIES
        INTERFACE_LINK_DIRECTORIES "${FFMPEG_LIBRARY_DIRS}"
      )
    endif()
  endif()
endif()

unset(_video_server_ffmpeg_found)
unset(_video_server_ffmpeg_module_dir)
unset(_video_server_ffmpeg_uses_pkg_config)

if(ON)
  find_dependency(spdlog CONFIG REQUIRED)
  find_dependency(tomlplusplus CONFIG REQUIRED)
  find_dependency(OpenSSL REQUIRED)
  find_dependency(LibDataChannel CONFIG REQUIRED)
  find_dependency(LibJuice CONFIG REQUIRED)
  find_dependency(libSRTP CONFIG REQUIRED)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/video_serverTargets.cmake")
