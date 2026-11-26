/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — app identity and first launch (our code, not upstream).
 */
#pragma once

#include <QString>
#include <QStringList>

namespace mu::collab::ploq {
//! Ploq's own version (src/collab/PLOQ_VERSION), separate from the MuseScore
//! version it is based on (MUSE_APP_VERSION, which scores record and must stay).
QString version();

//! Called once from main() right after the QApplication is created and BEFORE
//! the app loads its settings (one COLLAB: hook). It:
//! - publishes qApp->property("ploqVersion") for the About screen / splash;
//! - on first launch (no Ploq settings file yet) prepares Ploq's settings.
//! Never writes anywhere outside Ploq's own folders.
void onAppCreated();

//! What the first-launch step did / skipped, logged once logging is up.
QStringList firstLaunchNotes();
}
