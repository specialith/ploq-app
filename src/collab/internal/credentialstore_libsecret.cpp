/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — Linux credential store for the sign-in token: the desktop's
 * keyring through libsecret (Secret Service: GNOME Keyring, KWallet,
 * KeePassXC...). Built on Linux only, outside the unity build (CMakeLists.txt):
 * glib's headers clash with Qt's "signals" macro, so libsecret comes first.
 *
 * No keyring running: write() fails, and the sign-in lasts until Ploq closes
 * (account.cpp never puts the token in the plain-text settings on Linux).
 */
#include <libsecret/secret.h>

#include "credentialstore.h"

#include <QByteArray>

namespace mu::collab::credentials {
static const SecretSchema* ploqSchema()
{
    static const SecretSchema schema = {
        "app.ploq.Ploq.SignIn", SECRET_SCHEMA_NONE,
        {
            { "target", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING },
        },
        0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr
    };
    return &schema;
}

//! Say once why the keyring can't be used, not on every read and write.
static void warnOnce(GError* error)
{
    static bool warned = false;
    if (error && !warned) {
        warned = true;
        qWarning("collab: the keyring (Secret Service) isn't available, so the sign-in is kept only until Ploq closes: %s",
                 error->message);
    }
    g_clear_error(&error);
}

bool available()
{
    return true;
}

QString read(const QString& target)
{
    const QByteArray t = target.toUtf8();
    GError* error = nullptr;
    gchar* secret = secret_password_lookup_sync(ploqSchema(), nullptr, &error, "target", t.constData(), nullptr);
    if (error) {
        warnOnce(error);
        return QString();
    }
    const QString value = secret ? QString::fromUtf8(secret) : QString();
    secret_password_free(secret);
    return value;
}

bool write(const QString& target, const QString& secret)
{
    const QByteArray t = target.toUtf8();
    GError* error = nullptr;
    if (secret.isEmpty()) {
        secret_password_clear_sync(ploqSchema(), nullptr, &error, "target", t.constData(), nullptr);
        if (error) {
            warnOnce(error);
            return false;
        }
        return true;
    }
    const QByteArray label = "Ploq sign-in (" + t + ")";
    const QByteArray value = secret.toUtf8();
    const gboolean ok = secret_password_store_sync(ploqSchema(), SECRET_COLLECTION_DEFAULT, label.constData(), value.constData(),
                                                   nullptr, &error, "target", t.constData(), nullptr);
    if (error) {
        warnOnce(error);
        return false;
    }
    return ok == TRUE;
}
}
