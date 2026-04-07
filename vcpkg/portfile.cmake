get_filename_component(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

if(NOT EXISTS "${SOURCE_PATH}/CMakeLists.txt")
    message(FATAL_ERROR "Expected video-server source tree at '${SOURCE_PATH}'.")
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_VIDEO_SERVER=ON
        -DENABLE_WEBRTC_BACKEND=ON
        -DBUILD_TESTING=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(
    PACKAGE_NAME "video-server"
    CONFIG_PATH "lib/cmake/video-server"
)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
