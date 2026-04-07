vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Bossofter/video-server
    REF "v1.1.1"
    SHA512 e75c69e84291b3fa2dabeec80f62413e734595512c79a014177629b58a916ff51b85f7a049a6e2ed02a851d2efa84ddff2669a60b211264a3d9f5cb4d8be6502
    HEAD_REF main
    PATCHES
        fix-ffmpeg-link-keywords.patch
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
