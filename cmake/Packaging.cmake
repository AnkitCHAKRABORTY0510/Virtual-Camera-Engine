# =============================================================================
# Packaging.cmake — `make package` builds installable packages with CPack:
#
#   *.tar.gz   any Linux: unpack and run bin/virtual-camera (needs FFmpeg libs installed)
#   *.deb      Debian, Ubuntu, Mint, ...     (only when dpkg-deb is available)
#   *.rpm      Fedora, openSUSE, RHEL, ...   (only when rpmbuild is available)
#
# A package is built for the distribution it is built ON (it links that
# distribution's FFmpeg). The .deb/.rpm record those libraries as dependencies,
# so the package manager installs them automatically.
# =============================================================================
set(CPACK_PACKAGE_NAME "virtual-camera-engine")
set(CPACK_PACKAGE_VENDOR "Ankit Chakraborty")
set(CPACK_PACKAGE_CONTACT "Ankit Chakraborty")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Turn a video, images or a script into a Linux webcam (v4l2loopback)")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_RESOURCE_FILE_LICENSE ${PROJECT_SOURCE_DIR}/LICENSE)
set(CPACK_RESOURCE_FILE_README ${PROJECT_SOURCE_DIR}/README.md)
set(CPACK_PACKAGE_INSTALL_DIRECTORY "virtual-camera-engine")
set(CPACK_STRIP_FILES ON)                       # smaller binaries in packages
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")      # .deb / .rpm install into /usr

# Package file name: virtual-camera-engine-1.1.0-Linux-x86_64.<ext>
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-Linux-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_GENERATOR "TGZ")
find_program(VCAM_DPKG_DEB dpkg-deb)
find_program(VCAM_DPKG_SHLIBDEPS dpkg-shlibdeps)
find_program(VCAM_FILE_UTILITY file)       # CPack needs `file` to find the binaries to scan
find_program(VCAM_RPMBUILD rpmbuild)

if(VCAM_DPKG_DEB)
    list(APPEND CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_PACKAGE_SECTION "video")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    if(VCAM_DPKG_SHLIBDEPS AND VCAM_FILE_UTILITY)
        set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)  # dependencies (FFmpeg libs) found automatically
    else()
        message(STATUS "Packaging: install 'dpkg-dev' and 'file' so the .deb lists its library "
                       "dependencies automatically (without them the .deb has none)")
    endif()
    # The camera driver is a kernel module: recommended, not required (the
    # program can also write to a file or run without a device).
    set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "v4l2loopback-dkms, v4l-utils")
endif()

if(VCAM_RPMBUILD)
    list(APPEND CPACK_GENERATOR "RPM")
    set(CPACK_RPM_PACKAGE_LICENSE "MIT")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Multimedia")
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
    set(CPACK_RPM_PACKAGE_AUTOREQ ON)           # library dependencies found automatically
endif()

include(CPack)
