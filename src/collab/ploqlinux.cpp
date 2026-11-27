/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq on Linux (see ploqlinux.h).
 */
#include "ploqlinux.h"

#include <gcrypt.h>

#include <cstdio>

#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QMessageBox>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QString>
#include <QThread>

#include "log.h"

namespace mu::collab::ploq {
// ---------------------------------------------------------------------------
// ploq:// links on the command line
// ---------------------------------------------------------------------------

//! Tell the person why nothing happened. A link usually comes from the browser, so there's no
//! terminal to read: show a small box when there's a screen (never on the offscreen platform).
static void linkMessage(const char* argv0, const QString& text)
{
    std::fprintf(stderr, "Ploq: %s\n", qPrintable(text));
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == "offscreen" || qEnvironmentVariable("MU_QT_QPA_PLATFORM") == "offscreen") {
        return;
    }
    const bool x11 = !qEnvironmentVariableIsEmpty("DISPLAY");
    if (!x11 && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        return;
    }
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", x11 ? "xcb" : "wayland"); // as main.cpp: X11 where there is one
    }
    int n = 1;
    char* args[] = { const_cast<char*>(argv0), nullptr };
    QApplication app(n, args);
    QMessageBox::information(nullptr, "Ploq", text);
}

//! The waiting Ploq's QLocalServer (BrowserSignIn) gets the link as one line, then we're done.
static bool handOverSignIn(const char* argv0, const QString& state, const QString& link)
{
    int n = 1;
    char* args[] = { const_cast<char*>(argv0), nullptr };
    QCoreApplication app(n, args);
    QLocalSocket socket;
    for (int i = 0; i < 2; ++i) {
        if (i > 0) {
            QThread::msleep(1000); // a busy moment in the waiting Ploq: try once more
        }
        socket.connectToServer("ploq-signin-" + state, QIODevice::WriteOnly);
        if (socket.waitForConnected(3000)) {
            break;
        }
    }
    if (socket.state() != QLocalSocket::ConnectedState) {
        return false;
    }
    const QByteArray bytes = (link + u'\n').toLatin1();
    // If the waiting Ploq never reads, don't sit here as an invisible process forever.
    bool ok = socket.write(bytes) == bytes.size() && socket.waitForBytesWritten(10000);
    socket.disconnectFromServer();
    if (socket.state() != QLocalSocket::UnconnectedState) {
        socket.waitForDisconnected(10000);
    }
    return ok;
}

int linuxHandleLinkArgs(int& argc, char** argv)
{
    // Same shapes as main.cpp's Windows ploqHandleLink and tools/collab/portable/Open-Link.ps1.
    static const QRegularExpression signin(QStringLiteral(R"(\Aploq://signin\?state=([0-9a-f]{32})&code=[0-9a-f]{32}/?\z)"));
    static const QRegularExpression invite(QStringLiteral(
        R"(\A(?:ploq://join/[0-9a-f]{24}|ploq://open/p_[0-9a-f]{24}\?code=[0-9a-f]{32}|https://collab\.ploq\.app/j/[0-9a-f]{24})/?\z)"));
    static const char* const flagArg = "--ploq-link";
    static const QString notALink = QStringLiteral("That isn't a Ploq link, so nothing was opened.");

    QString link;
    if (argc >= 2 && qstrcmp(argv[1], flagArg) == 0) {
        if (argc != 3) { // exactly one link and nothing else
            linkMessage(argv[0], notALink);
            return 1;
        }
        link = QString::fromUtf8(argv[2]);
    } else if (argc == 2) {
        // The desktop entry runs `ploq <link>`. Anything that starts like a Ploq link is
        // handled (and refused unless it's an exact shape); everything else is a file to open.
        const QString arg = QString::fromUtf8(argv[1]);
        if (!arg.startsWith(QLatin1String("ploq:"), Qt::CaseInsensitive)
            && !arg.startsWith(QLatin1String("https://collab.ploq.app/"), Qt::CaseInsensitive)) {
            return -1;
        }
        link = arg;
    } else {
        return -1;
    }
    link = link.trimmed();

    const QRegularExpressionMatch s = signin.match(link);
    if (s.hasMatch()) {
        if (handOverSignIn(argv[0], s.captured(1), link)) {
            return 0;
        }
        linkMessage(argv[0], QStringLiteral("This sign-in has expired, or it wasn't started in Ploq on this computer. "
                                            "In Ploq, click Share to sign in again."));
        return 1;
    }
    if (invite.match(link).hasMatch()) {
        if (!qEnvironmentVariableIsSet("COLLAB_SERVER")) {
            qputenv("COLLAB_SERVER", "https://collab.ploq.app");
        }
        if (!qEnvironmentVariableIsSet("COLLAB_COLOR")) {
            qputenv("COLLAB_COLOR", "#f58231");
        }
        qputenv("COLLAB_INVITE", link.toUtf8());
        // A leftover "restore the last session?" would hold up joining (~/.local/share/Ploq/Ploq).
        QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
        if (dataHome.isEmpty() || !QDir::isAbsolutePath(dataHome)) {
            dataHome = QDir::homePath() + "/.local/share";
        }
        QFile::remove(dataHome + "/Ploq/Ploq/session/session.json");
        argc = 1; // nothing from the command line but the program name
        return -2;
    }
    linkMessage(argv[0], notALink);
    return 1;
}

// ---------------------------------------------------------------------------
// Registering the ploq:// handler
// ---------------------------------------------------------------------------

void linuxRegisterLinkHandler()
{
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (appImage.isEmpty()) {
        return; // a dev build or a distro package: no AppImage to point links at
    }
    const QFileInfo fi(appImage);
    if (!fi.isAbsolute() || !fi.isFile() || !fi.isExecutable()) {
        LOGW() << "collab: not registering ploq:// links: $APPIMAGE isn't an executable file";
        return;
    }
    // The path goes into a desktop entry's Exec= line, in double quotes. Only plain path
    // characters, so nothing in it can need escaping there (" ` $ \ % and control characters).
    static const QRegularExpression plainPath(QStringLiteral(R"(\A/[\p{L}\p{N} ._+,@()~/-]+\z)"));
    if (!plainPath.match(appImage).hasMatch()) {
        LOGW() << "collab: not registering ploq:// links: the AppImage's path has unusual characters";
        return;
    }

    const QString appsDir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (appsDir.isEmpty() || !QDir().mkpath(appsDir)) {
        return;
    }
    // Hidden (NoDisplay): only a link handler, so an AppImage integrated by another tool
    // doesn't show up twice in the menu.
    static const char* const desktopId = "app.ploq.PloqLinks.desktop";
    QByteArray entry = "[Desktop Entry]\n"
                       "Type=Application\n"
                       "Name=Ploq\n"
                       "Comment=Opens ploq:// links (sign-in, invites) in Ploq\n";
    entry += "Exec=\"" + appImage.toUtf8() + "\" %u\n";
    entry += "Icon=app.ploq.Ploq\n"
             "Terminal=false\n"
             "NoDisplay=true\n"
             "MimeType=x-scheme-handler/ploq;\n";
    const QString path = appsDir + "/" + desktopId;
    QFile f(path);
    if (f.open(QIODevice::ReadOnly) && f.readAll() == entry) {
        return; // already points here
    }
    f.close();
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(entry) != entry.size()) {
        LOGW() << "collab: couldn't write " << path;
        return;
    }
    f.close();

    const QString xdgMime = QStandardPaths::findExecutable("xdg-mime");
    if (!xdgMime.isEmpty()) {
        QProcess::startDetached(xdgMime, { "default", desktopId, "x-scheme-handler/ploq" });
    }
    const QString updateDb = QStandardPaths::findExecutable("update-desktop-database");
    if (!updateDb.isEmpty()) {
        QProcess::startDetached(updateDb, { appsDir });
    }
    LOGI() << "collab: ploq:// links open " << appImage;
}

// ---------------------------------------------------------------------------
// Update signature (libgcrypt)
// ---------------------------------------------------------------------------

//! As an unsigned number: libgcrypt reads some values as signed, so no leading 1 bit.
static QByteArray unsignedMpi(const QByteArray& bigEndian)
{
    return (!bigEndian.isEmpty() && (static_cast<unsigned char>(bigEndian.at(0)) & 0x80)) ? QByteArray(1, '\0') + bigEndian : bigEndian;
}

bool linuxEcdsaP256Verify(const QByteArray& message, const QByteArray& signature, const char* keyXHex, const char* keyYHex)
{
    if (signature.size() != 64) {
        return false; // r||s, 32 bytes each (Node: dsaEncoding "ieee-p1363")
    }
    // Initialise libgcrypt unless the app (or libsecret) already has.
    if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) {
        if (!gcry_check_version("1.6.0")) {
            return false;
        }
        gcry_control(GCRYCTL_DISABLE_SECMEM, 0); // only public data here
        gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);
    }

    const QByteArray q = QByteArray(1, '\x04') + QByteArray::fromHex(keyXHex) + QByteArray::fromHex(keyYHex);
    const QByteArray digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);
    const QByteArray r = unsignedMpi(signature.left(32));
    const QByteArray s = unsignedMpi(signature.mid(32));
    if (q.size() != 65) {
        return false;
    }

    gcry_sexp_t key = nullptr;
    gcry_sexp_t data = nullptr;
    gcry_sexp_t sig = nullptr;
    bool ok = false;
    if (!gcry_sexp_build(&key, nullptr, "(public-key (ecc (curve \"NIST P-256\") (q %b)))", int(q.size()), q.constData())
        && !gcry_sexp_build(&data, nullptr, "(data (flags raw) (value %b))", int(digest.size()), digest.constData())
        && !gcry_sexp_build(&sig, nullptr, "(sig-val (ecdsa (r %b) (s %b)))", int(r.size()), r.constData(), int(s.size()),
                            s.constData())) {
        ok = gcry_pk_verify(sig, data, key) == 0;
    }
    gcry_sexp_release(sig);
    gcry_sexp_release(data);
    gcry_sexp_release(key);
    return ok;
}
}
