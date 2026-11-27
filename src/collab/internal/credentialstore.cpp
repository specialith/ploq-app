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

#elif defined(Q_OS_MACOS)
// macOS: the login Keychain, as generic passwords (service "Ploq", account = target).
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace mu::collab::credentials {
namespace {
//! Owns a CoreFoundation object.
template<typename T>
struct CfRef {
    T ref = nullptr;
    explicit CfRef(T r = nullptr)
        : ref(r) {}
    ~CfRef() { if (ref) { CFRelease(ref); } }
    CfRef(const CfRef&) = delete;
    CfRef& operator=(const CfRef&) = delete;
};

CFStringRef cfString(const QString& s)
{
    return s.toCFString(); // caller owns it
}

//! The query that names one item: class, service, account.
CFMutableDictionaryRef itemQuery(CFStringRef account)
{
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, CFSTR("Ploq"));
    CFDictionarySetValue(q, kSecAttrAccount, account);
    return q;
}
}

bool available()
{
    return true;
}

QString read(const QString& target)
{
    CfRef<CFStringRef> account(cfString(target));
    CfRef<CFMutableDictionaryRef> q(itemQuery(account.ref));
    CFDictionarySetValue(q.ref, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q.ref, kSecMatchLimit, kSecMatchLimitOne);
    CFTypeRef result = nullptr;
    if (SecItemCopyMatching(q.ref, &result) != errSecSuccess || !result) {
        return QString();
    }
    CfRef<CFDataRef> data(static_cast<CFDataRef>(result));
    return QString::fromUtf8(reinterpret_cast<const char*>(CFDataGetBytePtr(data.ref)),
                             static_cast<qsizetype>(CFDataGetLength(data.ref)));
}

bool write(const QString& target, const QString& secret)
{
    CfRef<CFStringRef> account(cfString(target));
    CfRef<CFMutableDictionaryRef> q(itemQuery(account.ref));
    if (secret.isEmpty()) {
        const OSStatus st = SecItemDelete(q.ref);
        return st == errSecSuccess || st == errSecItemNotFound;
    }
    const QByteArray bytes = secret.toUtf8();
    CfRef<CFDataRef> data(CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(bytes.constData()), bytes.size()));

    CfRef<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(update.ref, kSecValueData, data.ref);
    OSStatus st = SecItemUpdate(q.ref, update.ref);
    if (st == errSecItemNotFound) {
        CFDictionarySetValue(q.ref, kSecValueData, data.ref);
        CFDictionarySetValue(q.ref, kSecAttrLabel, CFSTR("Ploq sign-in"));
        // Readable while this Mac user is logged in, and never synced to other devices.
        CFDictionarySetValue(q.ref, kSecAttrAccessible, kSecAttrAccessibleWhenUnlockedThisDeviceOnly);
        st = SecItemAdd(q.ref, nullptr);
    }
    return st == errSecSuccess;
}
}

#elif !defined(PLOQ_HAVE_LIBSECRET) // Linux: credentialstore_libsecret.cpp

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
