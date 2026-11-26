/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq: hand the current score to MuseScore Studio so it can be heard with
 * Muse Sounds, which Ploq can't load.
 */
#pragma once

#include <QString>

namespace mu::playback::ploq {
//! The MuseScore Studio executable on this computer, or empty if none was found.
//! Never returns Ploq itself, even when Ploq owns the .mscz file association.
//! PLOQ_STUDIO_EXE (QA only) overrides it.
QString findMuseScoreStudio();

//! Where to write the copy that is opened in MuseScore Studio:
//! <temp>/Ploq/Listen in MuseScore Studio/<title>.mscz (title made safe for a file name).
QString studioCopyPath(const QString& scoreTitle);

//! Start MuseScore Studio on `scorePath`, detached from Ploq.
bool openInMuseScoreStudio(const QString& studioExe, const QString& scorePath);

inline constexpr const char* MUSE_SOUNDS_NOTICE
    = "Muse Sounds playback isn't available in Ploq. Open your score in MuseScore Studio to hear it with Muse Sounds.";
inline constexpr const char* MUSESCORE_DOWNLOAD_URL = "https://musescore.org/download";
}
