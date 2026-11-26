/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — import a MuseScore 4 setup on first launch (our code, not upstream).
 */
#pragma once

#include <QSettings>
#include <QString>
#include <QStringList>

namespace mu::collab::ploq {
//! Where the MuseScore 4 setup is read from. Defaults:
//!   ini:  %APPDATA%\MuseScore\MuseScore4.ini
//!   data: %LOCALAPPDATA%\MuseScore\MuseScore4
//! Tests can point at a copy with PLOQ_IMPORT_MUSESCORE_INI / PLOQ_IMPORT_MUSESCORE_DATA.
struct MuseScoreSetup {
    QString iniPath;
    QString dataDir;
};
MuseScoreSetup defaultMuseScoreSetup(const QString& roamingDir, const QString& localDir);

//! Copies preferences (into ploqIni) and shortcuts, palettes, workspaces, MIDI
//! mappings, plugin/extension state and recent files (into ploqDataDir).
//! MuseScore's files are only read, through temporary copies. Anything that
//! can't be imported safely (sign-ins, Muse Group service state, paths into
//! MuseScore's own app data) is skipped; each skip is listed in notes (setting
//! names only, never values). No MuseScore install -> nothing happens.
void importMuseScoreSetup(const MuseScoreSetup& from, QSettings& ploqIni, const QString& ploqDataDir, QStringList& notes);
}
