vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO adanducci/ggpo
    REF c88b667
    SHA512 340b908deef907549e9ab2a08f97310ebd27717f80f9c63eb9d202adfc9307e2f59a080553bc91aea77c3e44022433ad6c3c26e2191b7a2e1d8c64b30fc6ad5c
    PATCHES
        "install-cmake-export.patch"
        "reject-invalid-udp-messages.patch"
)

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        ${FEATURE_OPTIONS}
        -DGGPO_BUILD_SDK=ON
        -DGGPO_BUILD_VECTORWAR=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup()
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)

# Handle copyright
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/${PORT} RENAME copyright)
