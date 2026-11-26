# SPDX-License-Identifier: GPL-3.0-only
#
# Ploq build switches. Included from the top-level CMakeLists.txt right after
# SetupConfigure (one COLLAB: line), so these win over MuseScore's defaults.
#
# How Muse Group online services are switched off in Ploq:
# 1. Network: every Qt connection to a Muse Group server is refused at run time
#    (src/collab/ploqnetguard.cpp). This is the guarantee.
# 2. Screens: their entry points are hidden with COLLAB: one-liners (Home
#    sidebar, online scores, Save to cloud / Publish online, welcome popup,
#    first-launch pages) and the update check points at Ploq's server.
# The cloud, learn and musesounds modules themselves stay built: MuseScore's
# "stub" stand-ins only work for its web build (the desktop screens import
# them directly; turning them off fails with "musesounds_qml is not a target").

set(MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT OFF) # crash reports to Muse Group (already off unless CI turns it on)

# Kept: the updater module (pointed at Ploq's own server, with signature checks:
# src/collab/ploqupdate.cpp) and the Muse Sounds *player* (MUSE_MODULE_MUSESAMPLER),
# which loads Muse Group's own library if installed (that library currently refuses
# to run inside Ploq).

message(STATUS "Ploq: crash reports off; Muse Group services blocked at run time (ploqnetguard)")
