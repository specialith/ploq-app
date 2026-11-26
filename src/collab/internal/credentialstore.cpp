/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — OS credential store for the sign-in token.
 * Built outside the module's unity build (see CMakeLists.txt): <windows.h>
 * defines macros such as ERROR, min and max that would leak into other files.
 */
#include "credentialstore.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

#include <string>

namespace mu::collab::credentials {
bool available()
{
    return true;
}

QString read(const QString& target)
{
    const std::wstring name = target.toStdWString();
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(name.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        return QString();
    }
    const QString secret = QString::fromUtf8(reinterpret_cast<const char*>(cred->CredentialBlob),
                                             static_cast<qsizetype>(cred->CredentialBlobSize));
    CredFree(cred);
    return secret;
}

bool write(const QString& target, const QString& secret)
{
    std::wstring name = target.toStdWString();
    if (secret.isEmpty()) {
        CredDeleteW(name.c_str(), CRED_TYPE_GENERIC, 0);
        return true;
    }
    QByteArray blob = secret.toUtf8();
    std::wstring user = L"Ploq";
    CREDENTIALW cred = {};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = name.data();
    cred.UserName = user.data();
    cred.CredentialBlobSize = static_cast<DWORD>(blob.size());
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(blob.data());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE; // this Windows user, this computer
    return CredWriteW(&cred, 0) == TRUE;
}
}

#else

namespace mu::collab::credentials {
bool available()
{
    return false;
}

QString read(const QString&)
{
    return QString();
}

bool write(const QString&, const QString&)
{
    return false;
}
}

#endif
