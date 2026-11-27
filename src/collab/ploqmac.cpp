/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq on macOS: ploq:// links (see ploqmac.h).
 */
#include "ploqmac.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileOpenEvent>
#include <QLocalSocket>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "log.h"

namespace mu::collab::ploq {
static bool s_collabStarted = false;
static std::function<bool(const QString&)> s_openIfIdle;

void macCollabStarted(std::function<bool(const QString&)> openIfIdle)
{
    s_collabStarted = true;
    s_openIfIdle = std::move(openIfIdle);
}

static void linkMessage(const QString& text)
{
    LOGW() << "ploq: " << text;
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen") {
        return;
    }
    // After the event has been handled: no modal box inside the event filter.
    QTimer::singleShot(0, qApp, [text]() { QMessageBox::information(nullptr, "Ploq", text); });
}

//! The waiting sign-in (BrowserSignIn's QLocalServer, in this Ploq or another one) gets the link as one line.
static void handOverSignIn(const QString& state, const QString& link)
{
    auto* socket = new QLocalSocket(qApp);
    QPointer<QLocalSocket> guard(socket);
    QObject::connect(socket, &QLocalSocket::connected, socket, [socket, link]() {
        socket->write((link + u'\n').toLatin1());
        socket->flush();
        socket->disconnectFromServer();
    });
    QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    QObject::connect(socket, &QLocalSocket::errorOccurred, socket, [socket](QLocalSocket::LocalSocketError) {
        linkMessage(QStringLiteral("This sign-in has expired, or it wasn't started in Ploq on this computer. "
                                   "In Ploq, click Share to sign in again."));
        socket->deleteLater();
    });
    // If nobody ever reads, don't keep the socket forever.
    QTimer::singleShot(15000, qApp, [guard]() {
        if (guard) {
            guard->abort();
            guard->deleteLater();
        }
    });
    socket->connectToServer("ploq-signin-" + state, QIODevice::WriteOnly);
}

static void openInvite(const QString& link)
{
    if (!s_collabStarted) {
        // Ploq was started by this link: join with it right away (CollabSyncService::init reads these).
        if (!qEnvironmentVariableIsSet("COLLAB_SERVER")) {
            qputenv("COLLAB_SERVER", "https://collab.ploq.app");
        }
        if (!qEnvironmentVariableIsSet("COLLAB_COLOR")) {
            qputenv("COLLAB_COLOR", "#f58231");
        }
        qputenv("COLLAB_INVITE", link.toUtf8());
        // A leftover "restore the last session?" would hold up joining.
        QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/session/session.json");
        LOGI() << "ploq: joining from the link that opened Ploq";
        return;
    }
    // Ploq is running: join here if it has no score open (it was most likely opened by this link: macOS
    // delivers the link a moment after start-up)...
    if (s_openIfIdle && s_openIfIdle(link)) {
        return;
    }
    // ...else it is busy with its own score: a new Ploq joins (as on Windows).
    QProcess p;
    p.setProgram(QCoreApplication::applicationFilePath());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!env.contains("COLLAB_SERVER")) {
        env.insert("COLLAB_SERVER", "https://collab.ploq.app");
    }
    if (!env.contains("COLLAB_COLOR")) {
        env.insert("COLLAB_COLOR", "#f58231");
    }
    env.insert("COLLAB_INVITE", link);
    p.setProcessEnvironment(env);
    if (p.startDetached()) {
        LOGI() << "ploq: opened a new Ploq for the link";
    } else {
        linkMessage(QStringLiteral("Ploq couldn't open that link. Try again in a moment."));
    }
}

static void handleLink(const QString& raw)
{
    // Same shapes as main.cpp's Windows ploqHandleLink, ploqlinux.cpp and tools/collab/portable/Open-Link.ps1.
    static const QRegularExpression signin(QStringLiteral(R"(\Aploq://signin\?state=([0-9a-f]{32})&code=[0-9a-f]{32}/?\z)"));
    static const QRegularExpression invite(QStringLiteral(
        R"(\A(?:ploq://join/[0-9a-f]{24}|ploq://open/p_[0-9a-f]{24}\?code=[0-9a-f]{32}|https://collab\.ploq\.app/j/[0-9a-f]{24})/?\z)"));
    const QString link = raw.trimmed();
    const QRegularExpressionMatch s = signin.match(link);
    if (s.hasMatch()) {
        handOverSignIn(s.captured(1), link);
        return;
    }
    if (invite.match(link).hasMatch()) {
        openInvite(link);
        return;
    }
    linkMessage(QStringLiteral("That isn't a Ploq link, so nothing was opened."));
}

namespace {
class LinkFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == qApp && event->type() == QEvent::FileOpen) {
            const QUrl url = static_cast<QFileOpenEvent*>(event)->url();
            if (url.scheme().compare(QLatin1String("ploq"), Qt::CaseInsensitive) == 0) {
                handleLink(url.toString(QUrl::FullyEncoded));
                return true; // ours: MuseScore must not try to open it as a score
            }
        }
        return QObject::eventFilter(watched, event);
    }
};
}

void macInstallLinkHandler()
{
    qApp->installEventFilter(new LinkFilter(qApp));
}
}
