/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — sign in through the browser (see browsersignin.h).
 */
#include "browsersignin.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

#include "muse_framework_config.h" // MUSE_APP_UNSTABLE (dev vs installed builds)

#include "log.h"

using namespace mu::collab;

static constexpr int SIGNIN_MINUTES = 10;
static constexpr qint64 MAX_LINK_BYTES = 256;

static QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(b.data()), n / 4);
    return b;
}

BrowserSignIn::BrowserSignIn()
{
    m_expiry.setSingleShot(true);
    m_expiry.setInterval(SIGNIN_MINUTES * 60 * 1000);
    QObject::connect(&m_expiry, &QTimer::timeout, [this]() {
        LOGI() << "collab: browser sign-in not finished in " << SIGNIN_MINUTES << " minutes";
        Expired onExpired = m_onExpired;
        finish();
        if (onExpired) {
            onExpired();
        }
    });
}

BrowserSignIn::~BrowserSignIn()
{
    finish();
}

QString BrowserSignIn::pipeName(const QString& state)
{
    return "ploq-signin-" + state;
}

QString BrowserSignIn::codeFromLink(const QString& link, const QString& state)
{
    static const QRegularExpression shape("^ploq://signin\\?state=([0-9a-f]{32})&code=([0-9a-f]{32})/?$");
    const QRegularExpressionMatch m = shape.match(link.trimmed());
    if (!m.hasMatch() || m.captured(1) != state) {
        return QString();
    }
    return m.captured(2);
}

bool BrowserSignIn::isActive() const
{
    return m_server != nullptr;
}

bool BrowserSignIn::start(const QString& serverBase, CodeReceived onCode, Expired onExpired)
{
    if (isActive()) {
        m_onCode = std::move(onCode);
        m_onExpired = std::move(onExpired);
        QDesktopServices::openUrl(QUrl(m_url));
        return true;
    }

    m_state = QString::fromLatin1(randomBytes(16).toHex());
    m_verifier = QString::fromLatin1(randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    m_server = std::make_unique<QLocalServer>();
    m_server->setSocketOptions(QLocalServer::UserAccessOption); // this Windows user only
    m_server->setMaxPendingConnections(4);
    if (!m_server->listen(pipeName(m_state))) {
        LOGW() << "collab: couldn't wait for the browser sign-in: " << m_server->errorString();
        m_server.reset();
        return false;
    }
    QObject::connect(m_server.get(), &QLocalServer::newConnection, [this]() { onConnection(); });

    QUrl url(serverBase + "/app-signin");
    QUrlQuery q;
    q.addQueryItem("state", m_state);
    q.addQueryItem("challenge", challenge);
    url.setQuery(q);
    m_url = url.toString();
    m_onCode = std::move(onCode);
    m_onExpired = std::move(onExpired);
    m_expiry.start();

    LOGI() << "collab: signing in through the browser (" << serverBase << "/app-signin)";
#ifdef MUSE_APP_UNSTABLE
    // Automated tests (development builds only): write the page's address to
    // a file instead of opening a browser; the test plays the website's part.
    const QString qaFile = qEnvironmentVariable("COLLAB_QA_BROWSER");
    if (!qaFile.isEmpty()) {
        QFile f(qaFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(m_url.toUtf8());
        }
        return true;
    }
#endif
    if (!QDesktopServices::openUrl(url)) {
        LOGW() << "collab: couldn't open the browser";
        finish();
        return false;
    }
    return true;
}

void BrowserSignIn::onConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QLocalSocket* socket = m_server->nextPendingConnection();
        QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        // One short line, then the writer closes. Anything else is dropped.
        QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket]() {
            if (socket->bytesAvailable() > MAX_LINK_BYTES || !socket->canReadLine()) {
                if (socket->bytesAvailable() > MAX_LINK_BYTES) {
                    socket->abort();
                }
                return;
            }
            const QString link = QString::fromUtf8(socket->readLine(MAX_LINK_BYTES)).trimmed();
            socket->disconnectFromServer();
            const QString code = codeFromLink(link, m_state);
            if (code.isEmpty()) {
                LOGW() << "collab: ignored a sign-in link that wasn't for this sign-in";
                return;
            }
            const CodeReceived onCode = m_onCode;
            const QString verifier = m_verifier;
            finish();
            if (onCode) {
                onCode(code, verifier);
            }
        });
        QTimer::singleShot(5000, socket, [socket]() { socket->abort(); });
    }
}

void BrowserSignIn::cancel()
{
    finish();
}

void BrowserSignIn::finish()
{
    m_expiry.stop();
    if (m_server) {
        m_server->close();
        m_server.release()->deleteLater(); // may be called from its own signal
    }
    m_state.clear();
    m_verifier.clear();
    m_url.clear();
    m_onCode = nullptr;
    m_onExpired = nullptr;
}
