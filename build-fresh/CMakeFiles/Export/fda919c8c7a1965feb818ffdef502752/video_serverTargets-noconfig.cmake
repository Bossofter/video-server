#----------------------------------------------------------------
# Generated CMake target import file.
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "video_server::video_server_core" for configuration ""
set_property(TARGET video_server::video_server_core APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(video_server::video_server_core PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libvideo_server_core.a"
  )

list(APPEND _cmake_import_check_targets video_server::video_server_core )
list(APPEND _cmake_import_check_files_for_video_server::video_server_core "${_IMPORT_PREFIX}/lib/libvideo_server_core.a" )

# Import target "video_server::video_server_webrtc" for configuration ""
set_property(TARGET video_server::video_server_webrtc APPEND PROPERTY IMPORTED_CONFIGURATIONS NOCONFIG)
set_target_properties(video_server::video_server_webrtc PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_NOCONFIG "CXX"
  IMPORTED_LOCATION_NOCONFIG "${_IMPORT_PREFIX}/lib/libvideo_server_webrtc.a"
  )

list(APPEND _cmake_import_check_targets video_server::video_server_webrtc )
list(APPEND _cmake_import_check_files_for_video_server::video_server_webrtc "${_IMPORT_PREFIX}/lib/libvideo_server_webrtc.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
