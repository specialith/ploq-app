/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — sign in through the browser.
 *
 * The app never shows its own sign-in form. It opens
 *   <server>/app-signin?state=<32 hex>&challenge=<base64url SHA-256(verifier)>
 * in the default browser. After the person signs in there, the site sends the
 * browser to ploq://signin?state=<same state>&code=<32 hex>. Windows runs
 * Open-Link.ps1 for that link, which checks its exact shape and writes it into
 * the named pipe \\.\pipe\ploq-signin-<state> - the one this app opened for
 * this attempt - instead of starting Ploq again.
 *
 * So a ploq://signin link someone else sends you does nothing (no app is
 * waiting for its state), and a code that leaks is useless without the
 * verifier, which never leaves this app. The pipe is only open for 10 minutes,
 * only to this Windows user, and takes one well-formed link.
 */
#pragma once

#include <functional>
#include <memory>

#include <QString>
#include <QTimer>

class QLocalServer;

namespace mu::collab {
class BrowserSignIn
{
public:
    //! The browser sent back a code for this attempt; redeem it with `verifier`.
    using CodeReceived = std::function<void (const QString& code, const QString& verifier)>;
    using Expired = std::function<void ()>;

    BrowserSignIn();
    ~BrowserSignIn();

    //! Opens the sign-in page for `serverBase` in the browser. While an attempt
    //! is running, calling it again only reopens the same page.
    bool start(const QString& serverBase, CodeReceived onCode, Expired onExpired);
    bool isActive() const;
    void cancel();

    //! The pipe for an attempt (QLocalServer name; Windows: \\.\pipe\<name>).
    static QString pipeName(const QString& state);
    //! The code in `link` if it is exactly ploq://signin?state=<state>&code=<32 hex>.
    static QString codeFromLink(const QString& link, const QString& state);

private:
    void onConnection();
    void finish();

    std::unique_ptr<QLocalServer> m_server;
    QTimer m_expiry;
    QString m_state;
    QString m_verifier;
    QString m_url;
    CodeReceived m_onCode;
    Expired m_onExpired;
};
}
