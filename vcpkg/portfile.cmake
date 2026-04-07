vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Bossofter/video-server
    REF "v1.1.1"
    SHA512 0
    HEAD_REF main
)

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
