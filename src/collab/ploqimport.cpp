/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — import a MuseScore 4 setup on first launch. See ploqimport.h.
 */
#include "ploqimport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <memory>

namespace mu::collab::ploq {
MuseScoreSetup defaultMuseScoreSetup(const QString& roamingDir, const QString& localDir)
{
    MuseScoreSetup s;
    s.iniPath = qEnvironmentVariable("PLOQ_IMPORT_MUSESCORE_INI",
                                     QDir(roamingDir).filePath("MuseScore/MuseScore4.ini"));
    s.dataDir = qEnvironmentVariable("PLOQ_IMPORT_MUSESCORE_DATA",
                                     QDir(localDir).filePath("MuseScore/MuseScore4"));
    return s;
}

//! Why a MuseScore setting is not copied, or an empty string if it is safe.
static QString skipReason(const QString& key, const QVariant& value, const QString& museDataDir)
{
    const QString k = key.toLower();
    if (k.startsWith("cloud/")) {
        return "MuseScore.com / Audio.com account";
    }
    for (const char* secret : { "token", "password", "secret", "cred", "auth", "clientid" }) {
        if (k.contains(secret)) {
            return "sign-in data";
        }
    }
    if (k.contains("checkforupdate") || k.contains("skippedversion") || k.startsWith("update/")) {
        return "MuseScore's update check (Ploq has its own)";
    }
    if (k.contains("welcomedialog") || k.contains("musesounds") || k.startsWith("learn/")) {
        return "Muse Group news / Muse Sounds store state";
    }
    if (k.startsWith("collab/")) {
        return "collaboration (Ploq only)";
    }
    if (value.typeId() == QMetaType::QString || value.typeId() == QMetaType::QStringList) {
        const QString text = value.toStringList().join('|');
        const QString a = QDir::fromNativeSeparators(museDataDir).toLower();
        if (!a.isEmpty() && QDir::fromNativeSeparators(text).toLower().contains(a)) {
            return "points into MuseScore's own app data folder";
        }
    }
    return QString();
}

static void importPreferences(const QString& iniPath, const QString& museDataDir, QSettings& ploqIni, QStringList& notes)
{
    // Read a temporary copy: QSettings can then never write to MuseScore's file.
    QTemporaryDir tmp;
    const QString copy = tmp.filePath("MuseScore4.ini");
    if (!tmp.isValid() || !QFile::copy(iniPath, copy)) {
        notes << "preferences: could not read " + QDir::toNativeSeparators(iniPath) + ", skipped";
        return;
    }
    QSettings muse(copy, QSettings::IniFormat);
    int copied = 0;
    for (const QString& key : muse.allKeys()) {
        const QVariant value = muse.value(key);
        const QString why = skipReason(key, value, museDataDir);
        if (!why.isEmpty()) {
            notes << QString("preferences: skipped \"%1\" (%2)").arg(key, why);
            continue;
        }
        ploqIni.setValue(key, value);
        ++copied;
    }
    notes << QString("preferences: imported %1 settings from %2").arg(copied).arg(QDir::toNativeSeparators(iniPath));
}

//! Copies one file or folder (recursively) from MuseScore's data folder into
//! Ploq's. Never overwrites anything Ploq already has.
static int copyItem(const QString& from, const QString& to)
{
    const QFileInfo fi(from);
    if (fi.isFile()) {
        if (QFileInfo::exists(to)) {
            return 0;
        }
        QDir().mkpath(QFileInfo(to).absolutePath());
        return QFile::copy(from, to) ? 1 : 0;
    }
    int n = 0;
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        n += copyItem(file, QDir(to).filePath(QDir(from).relativeFilePath(file)));
    }
    return n;
}

static void importData(const QString& museDataDir, const QString& ploqDataDir, QStringList& notes)
{
    struct Item {
        const char* path;
        const char* what;
    };
    static const Item items[] = {
        { "shortcuts.xml", "keyboard shortcuts" },
        { "workspaces", "workspaces (palettes, panel layout)" },
        { "keysigs", "custom key signature palette cells" },
        { "timesigs", "custom time signature palette cells" },
        { "midi_mappings.xml", "MIDI remote control mappings" },
        { "plugins/plugins.json", "which plugins are switched on" },
        { "extensions", "installed extensions" },
        { "recent_files.json", "recent scores list" },
    };
    for (const Item& item : items) {
        const QString from = QDir(museDataDir).filePath(item.path);
        if (!QFileInfo::exists(from)) {
            continue; // never customized in MuseScore: nothing to import
        }
        const int n = copyItem(from, QDir(ploqDataDir).filePath(item.path));
        notes << QString("data: imported %1 (%2 file%3)").arg(item.what).arg(n).arg(n == 1 ? "" : "s");
    }
    notes << "data: not imported (on purpose): MuseScore.com sign-in, cloud scores, session, logs, cache, "
             "update history, downloaded translations, audio plugin scan list (Ploq rescans)";
}

void importMuseScoreSetup(const MuseScoreSetup& from, QSettings& ploqIni, const QString& ploqDataDir, QStringList& notes)
{
    const bool hasIni = QFileInfo(from.iniPath).isFile();
    const bool hasData = QFileInfo(from.dataDir).isDir();
    if (!hasIni && !hasData) {
        notes << "no MuseScore 4 setup found, nothing to import";
        return;
    }
    if (hasIni) {
        importPreferences(from.iniPath, from.dataDir, ploqIni, notes);
    }
    if (hasData) {
        importData(from.dataDir, ploqDataDir, notes);
    }
}
}
