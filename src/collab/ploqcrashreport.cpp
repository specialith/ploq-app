/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — crash reports (our code, not upstream).
 *
 * src/app/main.cpp writes Ploq-<pid>-<ticks>.dmp into crashDumpFolder() when Ploq
 * crashes (stacks and thread info only, no heap). Nothing is ever uploaded: the next
 * time Ploq starts, it says a report was saved and the user decides whether to send it.
 */
#include "ploqcrashreport.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QTimer>
#include <QUrl>

#include "log.h"

namespace mu::collab::ploq {
static constexpr int KEEP_DUMPS = 5;
static const char* SEEN_FILE = "last-reported.txt";

QString crashDumpFolder()
{
    return QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA")) + "/Ploq/crashes";
}

static bool isQaRun()
{
    if (qEnvironmentVariable("PLOQ_QA_SHOW_NOTICES") == "1") {
        return false; // tools/collab/qa-auto/shots.ps1 -Notices: show it (and grab it, below)
    }
    return !qEnvironmentVariableIsEmpty("COLLAB_AUTOBOT")
           || qEnvironmentVariable("PLOQ_NO_ACTIVATE") == "1"
           || !qEnvironmentVariableIsEmpty("COLLAB_CRASH_DUMP")
           || qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen";
}

//! The dumps, newest first. Drops all but the newest KEEP_DUMPS (1 MB-ish each, but a
//! crash loop shouldn't fill the disk).
static QFileInfoList dumpsKeepingNewest(const QDir& dir)
{
    QFileInfoList dumps = dir.entryInfoList({ "Ploq-*.dmp" }, QDir::Files, QDir::Time);
    while (dumps.size() > KEEP_DUMPS) {
        QFile::remove(dumps.takeLast().absoluteFilePath());
    }
    return dumps;
}

void offerCrashReportIfAny(QObject* ctx)
{
    const QDir dir(crashDumpFolder());
    if (!dir.exists()) {
        return;
    }
    const QFileInfoList dumps = dumpsKeepingNewest(dir);
    if (dumps.isEmpty()) {
        return;
    }
    const QFileInfo newest = dumps.first();
    LOGI() << "ploq: newest crash report " << newest.fileName() << " (" << newest.lastModified().toString(Qt::ISODate) << ")";

    QFile seen(dir.filePath(SEEN_FILE));
    if (seen.open(QIODevice::ReadOnly) && QString::fromUtf8(seen.readAll()).trimmed() == newest.fileName()) {
        return; // already told them about this one
    }
    seen.close();
    if (isQaRun()) {
        return;
    }

    // After the main window is up, so the message sits on top of it.
    QTimer::singleShot(4000, ctx, [dir, name = newest.fileName()]() {
        QFile mark(dir.filePath(SEEN_FILE));
        if (mark.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            mark.write(name.toUtf8());
        }

        QMessageBox box;
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(QObject::tr("Ploq closed unexpectedly"));
        box.setText(QObject::tr("Ploq crashed the last time you used it. Sorry about that."));
        box.setInformativeText(QObject::tr(
                                   "It saved a crash report on this computer. If you send it to us, we can see what went wrong and fix it.\n\n"
                                   "Send it privately, for example by replying to any email from Ploq, not in a public post: "
                                   "it can contain bits of what Ploq was doing at the time.\n\n"
                                   "The report is %1 in the folder that opens.").arg(name));
        QPushButton* open = box.addButton(QObject::tr("Open folder"), QMessageBox::AcceptRole);
        box.addButton(QObject::tr("Not now"), QMessageBox::RejectRole);
        box.setDefaultButton(open);
        const QString grabDir = qEnvironmentVariable("PLOQ_QA_GRAB"); // QA screenshots, offscreen
        if (!grabDir.isEmpty()) {
            QTimer::singleShot(1500, &box, [&box, grabDir]() {
                QDir().mkpath(grabDir);
                box.grab().save(QDir(grabDir).filePath("crash-report.png"));
                box.reject();
            });
        }
        box.exec();
        if (box.clickedButton() == open) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
        }
    });
}
}
