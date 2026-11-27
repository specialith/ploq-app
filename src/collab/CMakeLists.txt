# SPDX-License-Identifier: GPL-3.0-only
#
# Collab fork — real-time collaboration module (our code, not upstream).

muse_create_module(collab)

target_sources(collab PRIVATE
    collabhooks.cpp
    collabhooks.h
    collabmodule.cpp
    collabmodule.h

    internal/account.cpp
    internal/account.h
    internal/browsersignin.cpp
    internal/browsersignin.h
    internal/collabuiactions.cpp
    internal/collabuiactions.h
    internal/collabsyncservice.cpp
    internal/collabsyncservice.h
    internal/credentialstore.cpp
    internal/credentialstore.h
    internal/cursorlayer.cpp
    internal/cursorlayer.h
    internal/measurexml.cpp
    internal/measurexml.h
    internal/selectionrects.cpp
    internal/selectionrects.h
    internal/sharecontroller.cpp
    internal/sharecontroller.h

    ploqapp.cpp
    ploqapp.h
    ploqcrashreport.cpp
    ploqcrashreport.h
    ploqimport.cpp
    ploqimport.h
    ploqnetguard.cpp
    ploqnetguard.h
    ploqupdate.cpp
    ploqupdate.h
)

# Ploq's own version number lives in PLOQ_VERSION (bump it for a release; only
# ploqapp.cpp rebuilds). MUSE_APP_VERSION stays MuseScore's: scores record it.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/PLOQ_VERSION PLOQ_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/PLOQ_VERSION)
configure_file(ploqversion.h.in ${CMAKE_CURRENT_BINARY_DIR}/ploqversion.h @ONLY)
target_include_directories(collab PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
message(STATUS "PLOQ_VERSION ${PLOQ_VERSION}")

# Ploq artwork (share/icons/Ploq, made by tools/collab/branding/make_icons.py),
# served as qrc:/ploq/... to the About screen and loading screen.
qt_add_resources(collab ploq_branding
    PREFIX "/ploq"
    BASE "${PROJECT_SOURCE_DIR}/share/icons/Ploq"
    FILES
        ${PROJECT_SOURCE_DIR}/share/icons/Ploq/ploq_tile.svg
        ${PROJECT_SOURCE_DIR}/share/icons/Ploq/ploq_loading.svg
)

# Inter (SIL Open Font License, fonts/Inter-LICENSE.txt) for collaborators'
# name tags, served as qrc:/ploq/fonts/... and registered at startup.
qt_add_resources(collab ploq_fonts
    PREFIX "/ploq/fonts"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/fonts"
    FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/Inter-Light.ttf
        ${CMAKE_CURRENT_SOURCE_DIR}/fonts/Inter-Regular.ttf
)

# Qt WebSockets: found here so we don't depend on upstream's
# MUSE_MODULE_NETWORK_WEBSOCKET option (see ARCHITECTURE_NOTES.md section 6).
# GLOBAL: the app target (src/app) links our static lib and must be able to
# see Qt::WebSockets too, otherwise Qt warns the link "might be incomplete".
find_package(Qt6 REQUIRED COMPONENTS WebSockets GLOBAL)

target_link_libraries(collab PRIVATE
    Qt::WebSockets
    Qt::Network
    Qt::Widgets
    Qt::Quick
    muse::draw
    engraving
)

# <windows.h> (credential store) defines macros like ERROR/min/max: keep that
# file out of the module's unity build so they can't leak into other files.
set_source_files_properties(internal/credentialstore.cpp ploqupdate.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
if (WIN32)
    target_link_libraries(collab PRIVATE Advapi32) # CredReadW / CredWriteW
    target_link_libraries(collab PRIVATE Bcrypt Wintrust) # update signature check (ploqupdate.cpp)
elseif (APPLE)
    target_link_libraries(collab PRIVATE "-framework Security" "-framework CoreFoundation") # Keychain, update signature check
    target_sources(collab PRIVATE ploqmac.cpp ploqmac.h) # ploq:// links ("open URL" events)
endif()

# Linux: ploq:// links, the keyring (libsecret) and the update signature check (libgcrypt).
if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PLOQ_LIBSECRET REQUIRED libsecret-1) # apt install libsecret-1-dev
    pkg_check_modules(PLOQ_GCRYPT REQUIRED libgcrypt)      # apt install libgcrypt20-dev
    target_sources(collab PRIVATE ploqlinux.cpp ploqlinux.h internal/credentialstore_libsecret.cpp)
    # glib's headers (libsecret) use "signals" as a name, which Qt (and so the precompiled header) defines as a macro.
    set_source_files_properties(ploqlinux.cpp internal/credentialstore_libsecret.cpp PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
    list(REMOVE_ITEM PLOQ_LIBSECRET_INCLUDE_DIRS /usr/include) # as -isystem it breaks the C++ headers' #include_next
    set_source_files_properties(internal/credentialstore_libsecret.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON
        INCLUDE_DIRECTORIES "${PLOQ_LIBSECRET_INCLUDE_DIRS}")
    target_compile_definitions(collab PRIVATE PLOQ_HAVE_LIBSECRET)
    # Full library paths, not pkg-config's imported targets: the app links our static lib and
    # would have to see those targets too (and their -pthread would disable the module's PCH).
    target_link_libraries(collab PRIVATE ${PLOQ_LIBSECRET_LINK_LIBRARIES} ${PLOQ_GCRYPT_LINK_LIBRARIES})
endif()

# The Share dialog (QML module Ploq.Collab, linked into the app with collab:
# src/app links <module>_qml next to every module it links).
add_subdirectory(qml/Ploq/Collab)
