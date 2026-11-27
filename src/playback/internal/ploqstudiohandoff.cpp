/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq: hand the current score to MuseScore Studio (see ploqstudiohandoff.h).
 */
#include "ploqstudiohandoff.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

namespace mu::playback::ploq {
static bool isUsableStudio(const QString& exe)
{
    if (exe.isEmpty()) {
        return false;
    }
    const QFileInfo fi(exe);
    // Ploq installs as its own app and may own the .mscz association: never hand the score back to it.
    return fi.isFile() && fi.isExecutable() && !fi.fileName().contains("ploq", Qt::CaseInsensitive);
}

#ifdef Q_OS_WIN
//! `"C:\Program Files\MuseScore 4\bin\MuseScore4.exe" "%1"` -> the executable path
static QString exeFromOpenCommand(const QString& command)
{
    const QString cmd = command.trimmed();
    if (cmd.startsWith('"')) {
        const qsizetype end = cmd.indexOf('"', 1);
        return end > 1 ? cmd.mid(1, end - 1) : QString();
    }
    const QRegularExpressionMatch m = QRegularExpression("^(.+?\\.exe)\\b", QRegularExpression::CaseInsensitiveOption).match(cmd);
    return m.hasMatch() ? m.captured(1) : QString();
}

static QString exeForProgId(const QString& progId)
{
    if (progId.isEmpty()) {
        return {};
    }
    const QSettings cmd("HKEY_CLASSES_ROOT\\" + progId + "\\shell\\open\\command", QSettings::NativeFormat);
    return exeFromOpenCommand(cmd.value("Default").toString());
}
#endif

QString findMuseScoreStudio()
{
    // QA (tools/collab/qa-auto/listen.ps1): a stand-in, so tests never open a real window
    const QString qaStandIn = qEnvironmentVariable("PLOQ_STUDIO_EXE");
    if (!qaStandIn.isEmpty()) {
        return qaStandIn;
    }

    QStringList candidates;

#if defined(Q_OS_WIN)
    // The installer doesn't register App Paths; it does register the .mscz file type.
    QStringList progIds;
    const QSettings mscz("HKEY_CLASSES_ROOT\\.mscz", QSettings::NativeFormat);
    progIds << mscz.value("Default").toString();
    QSettings openWith("HKEY_CLASSES_ROOT\\.mscz\\OpenWithProgids", QSettings::NativeFormat);
    progIds << openWith.childKeys();
    progIds << "MuseScore.mscz.4.stable" << "MuseScore.mscz.5.stable";
    for (const QString& progId : progIds) {
        if (progId.startsWith("MuseScore", Qt::CaseInsensitive)) {
            candidates << exeForProgId(progId);
        }
    }

    for (const QString& root : { qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramW6432"),
                                 qEnvironmentVariable("ProgramFiles(x86)") }) {
        if (root.isEmpty()) {
            continue;
        }
        candidates << root + "/MuseScore 4/bin/MuseScore4.exe"
                   << root + "/MuseScore Studio 5/bin/MuseScore5.exe"
                   << root + "/MuseScore 5/bin/MuseScore5.exe";
    }
#elif defined(Q_OS_MACOS)
    for (const QString& apps : { QString("/Applications"), QDir::homePath() + "/Applications" }) {
        candidates << apps + "/MuseScore Studio.app/Contents/MacOS/mscore"
                   << apps + "/MuseScore 4.app/Contents/MacOS/mscore";
    }
#else
    for (const char* name : { "mscore4portable", "musescore4", "musescore", "mscore" }) {
        candidates << QStandardPaths::findExecutable(QString::fromLatin1(name));
    }
#ifdef Q_OS_LINUX
    // Flatpak (system-wide or per user), then MuseScore AppImages kept in the usual folders (newest name first;
    // a downloaded AppImage that was never made executable can't run and is skipped by isUsableStudio).
    candidates << QStringLiteral("/var/lib/flatpak/exports/bin/org.musescore.MuseScore")
               << QDir::homePath() + "/.local/share/flatpak/exports/bin/org.musescore.MuseScore";
    for (const QString& folder : { QDir::homePath() + "/Applications", QDir::homePath() + "/.local/bin", QDir::homePath() + "/Downloads" }) {
        const QDir dir(folder);
        for (const QString& name : dir.entryList({ "MuseScore*.AppImage" }, QDir::Files, QDir::Name | QDir::Reversed)) {
            candidates << dir.absoluteFilePath(name);
        }
    }
#endif
#endif

    for (const QString& exe : candidates) {
        if (isUsableStudio(exe)) {
            return QDir::toNativeSeparators(QFileInfo(exe).absoluteFilePath());
        }
    }
    return {};
}

QString studioCopyPath(const QString& scoreTitle)
{
    QString name = scoreTitle.trimmed();
    name.replace(QRegularExpression("[<>:\"/\\\\|?*\\x00-\\x1F]"), "_");
    if (name.isEmpty()) {
        name = "Untitled score";
    }

#ifdef Q_OS_LINUX
    // Not /tmp: a Flatpak MuseScore Studio has its own /tmp and couldn't open the copy (~/.cache/Ploq/Ploq/...)
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/Listen in MuseScore Studio";
#else
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/Ploq/Listen in MuseScore Studio";
#endif
    QDir().mkpath(dir);
    return dir + "/" + name + ".mscz";
}

bool openInMuseScoreStudio(const QString& studioExe, const QString& scorePath)
{
    return QProcess::startDetached(studioExe, { QDir::toNativeSeparators(scorePath) });
}
}
