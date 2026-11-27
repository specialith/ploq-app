include(GetPlatformInfo)

if (OS_IS_MAC)
    set(INSTALL_SUBDIR Ploq.app/Contents/MacOS/) # COLLAB: was mscore.app
    set(INSTALL_BIN_DIR ${CMAKE_INSTALL_PREFIX}/${INSTALL_SUBDIR})
else()
    set(INSTALL_SUBDIR bin)
    set(INSTALL_BIN_DIR ${CMAKE_INSTALL_PREFIX}/${INSTALL_SUBDIR})
endif()
