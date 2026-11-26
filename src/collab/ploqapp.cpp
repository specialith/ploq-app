/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — app identity and first launch (our code, not upstream).
 *
 * Ploq's settings live in its own folders (see src/app/main.cpp):
 *   %APPDATA%\Ploq\Ploq.ini            (PloqDevelopment.ini for dev builds)
 *   %LOCALAPPDATA%\Ploq\Ploq\          (workspaces, shortcuts, logs, ...)
 * Other apps' files (MuseScore, older CollabScore/Ploq dev builds) are only
 * ever READ, and only through a temporary copy, so they stay byte-for-byte
 * unchanged.
 */
#include "ploqapp.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <memory>

#include <QStandardPaths>

#include "ploqimport.h"
#include "ploqnetguard.h"
#include "ploqversion.h"

namespace mu::collab::ploq {
static QStringList s_notes;

QString version()
{
    return QStringLiteral(PLOQ_VERSION);
}

QStringList firstLaunchNotes()
{
    return s_notes;
}

static QString ploqIniPath()
{
    // The same file muse::Settings opens (default QSettings, IniFormat, user scope).
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QCoreApplication::organizationName(), QCoreApplication::applicationName()).fileName();
}

//! %APPDATA% (the folder that holds "Ploq\Ploq.ini").
static QDir roamingDir()
{
    QDir d = QFileInfo(ploqIniPath()).absoluteDir();
    d.cdUp();
    return d;
}

//! Opens a read-only snapshot of someone else's INI file: QSettings is given a
//! temporary copy, so nothing can ever be written back to the original.
class IniSnapshot
{
public:
    explicit IniSnapshot(const QString& path)
    {
        if (!QFileInfo::exists(path) || !m_tmp.isValid()) {
            return;
        }
        const QString copy = m_tmp.filePath("snapshot.ini");
        if (QFile::copy(path, copy)) {
            m_settings = std::make_unique<QSettings>(copy, QSettings::IniFormat);
        }
    }

    QSettings* get() const { return m_settings.get(); }

private:
    QTemporaryDir m_tmp;
    std::unique_ptr<QSettings> m_settings;
};

//! Collab sign-ins (name, user id, token) from the pre-rename dev builds, which
//! kept them in MuseScore4Development.ini under [collab]. Room bookmarks are
//! not copied: they point into the old folder, and shared scores reopen from
//! the server anyway.
static void copyCollabSignIns(QSettings& ploqIni)
{
    IniSnapshot old(roamingDir().filePath("MuseScore/MuseScore4Development.ini"));
    if (!old.get()) {
        return;
    }
    int copied = 0;
    for (const QString& k : old.get()->allKeys()) {
        if (!k.startsWith("collab/")) {
            continue;
        }
        const QString leaf = k.section('/', -1);
        if (leaf == "token" || leaf == "userId" || leaf == "name") {
            ploqIni.setValue(k, old.get()->value(k));
            ++copied;
        }
    }
    if (copied) {
        s_notes << QString("copied %1 collab sign-in values from the old dev settings").arg(copied);
    }
}

static void prepareFirstLaunch()
{
    const QString iniPath = ploqIniPath();
    if (QFileInfo::exists(iniPath)) {
        return; // not the first launch
    }
    QDir().mkpath(QFileInfo(iniPath).absolutePath());

    QSettings ploqIni(iniPath, QSettings::IniFormat);
    // MuseScore 4 setup: %APPDATA%\MuseScore\MuseScore4.ini + %LOCALAPPDATA%\MuseScore\MuseScore4
    const QString ploqData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir local(ploqData);
    local.cdUp();
    local.cdUp(); // %LOCALAPPDATA%
    importMuseScoreSetup(defaultMuseScoreSetup(roamingDir().absolutePath(), local.absolutePath()), ploqIni, ploqData, s_notes);
    copyCollabSignIns(ploqIni);
    ploqIni.sync();
    s_notes << "first launch: created " + QDir::toNativeSeparators(iniPath);
}

void onAppCreated()
{
    installNetworkGuard(); // before anything can open a connection
    qApp->setProperty("ploqVersion", version());
    prepareFirstLaunch();
}
}
