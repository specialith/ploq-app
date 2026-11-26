# SPDX-License-Identifier: GPL-3.0-only
#
# Ploq — the app's own dialogs (our code, not upstream).

muse_create_qml_module(collab_qml FOR collab)

qt_add_qml_module(collab_qml
    URI Ploq.Collab
    VERSION 1.0
    SOURCES
        sharedialogmodel.cpp
        sharedialogmodel.h
    QML_FILES
        ShareDialog.qml
)
