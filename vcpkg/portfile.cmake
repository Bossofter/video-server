set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/..")

if("vaapi" IN_LIST FEATURES)
    set(VIDEO_SERVER_VAAPI_OPTION ON)
else()
    set(VIDEO_SERVER_VAAPI_OPTION OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DENABLE_VIDEO_SERVER=ON
        -DENABLE_WEBRTC_BACKEND=ON
        -DBUILD_TESTING=OFF
        -DVIDEO_SERVER_ENABLE_VAAPI=${VIDEO_SERVER_VAAPI_OPTION}
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
