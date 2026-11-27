# SPDX-License-Identifier: GPL-3.0-only
#
# Ploq on Linux: the desktop entry and the AppImage layout. Included from the
# top-level CMakeLists.txt (one COLLAB: line) instead of upstream's
# buildscripts/packaging/Linux+BSD/SetupAppImagePackaging.cmake, which installs
# MuseScore's names, .mscz file types, man pages and its portable-utils.
# The executable is bin/ploq (src/app/CMakeLists.txt), the icon app.ploq.Ploq
# (share/icons/CMakeLists.txt). Build an AppImage with build_appimage.sh.

include(GetPlatformInfo)
if (NOT OS_IS_LIN)
    return()
endif()

# StartupWMClass must be the X11 window class, which Qt takes from
# QCoreApplication::applicationName() (src/app/main.cpp).
if (MUSE_APP_UNSTABLE)
    set(PLOQ_LAUNCHER_NAME "Ploq Development")
    set(PLOQ_WM_CLASS "PloqDevelopment")
else()
    set(PLOQ_LAUNCHER_NAME "Ploq")
    set(PLOQ_WM_CLASS "Ploq")
endif()

# app.ploq.Ploq = QGuiApplication::setDesktopFileName() in main.cpp (Wayland app id).
# x-scheme-handler/ploq: an AppImage integration tool (Gear Lever, AppImageLauncher...)
# registers ploq:// links from this; Ploq also registers them itself on start
# (src/collab/ploqlinux.cpp, linuxRegisterLinkHandler).
configure_file(${CMAKE_CURRENT_LIST_DIR}/app.ploq.Ploq.desktop.in ${PROJECT_BINARY_DIR}/app.ploq.Ploq.desktop @ONLY)
install(FILES ${PROJECT_BINARY_DIR}/app.ploq.Ploq.desktop DESTINATION share/applications)

option(PLOQ_APPIMAGE "Install as an AppDir for an AppImage (AppRun, findlib)" OFF)
if (PLOQ_APPIMAGE)
    # findlib: AppRun uses it to decide on fallback libraries (upstream's helper, unchanged).
    add_subdirectory(${PROJECT_SOURCE_DIR}/buildscripts/packaging/Linux+BSD/portable ${PROJECT_BINARY_DIR}/ploq_portable)
    configure_file(${CMAKE_CURRENT_LIST_DIR}/AppRun.in ${PROJECT_BINARY_DIR}/AppRun @ONLY)
    install(PROGRAMS ${PROJECT_BINARY_DIR}/AppRun DESTINATION .)
endif()
