/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — verified auto-update (our code, not upstream). See ploqupdate.h.
 */
#include "ploqupdate.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>
#endif

namespace mu::collab::ploq {
// Ploq release key, public half (ECDSA P-256). The private half is
// %USERPROFILE%\.ploq\update-signing-key.pem on the release machine only.
// Changing keys: ship one release signed with the OLD key whose app has the
// NEW public key here; after that, sign with the new key.
static const char* RELEASE_KEY_X = "095d589a692d7e1f5af64dd1381f0b4298fdf5898dad99061fd33e13a8f151ac";
static const char* RELEASE_KEY_Y = "0acbbb01f62bdcee1b6ffaa5e90684cefbf57147f9b560e9a53594f16ea52dfb";

static const char* DEFAULT_FEED = "https://collab.ploq.app/updates/latest.json";

struct Expected {
    qint64 size = -1;
    QByteArray sha256; // raw bytes
};
static QMutex s_mutex;
static QHash<QString, Expected> s_expected; // asset name -> expected size/hash

static bool isLocalHttp(const QUrl& u)
{
    if (u.scheme() != "http") {
        return false;
    }
    if (u.host() == "localhost") {
        return true;
    }
    QHostAddress a(u.host());
    return !a.isNull() && a.isLoopback();
}

QString updateFeedUrl()
{
    const QString overrideUrl = qEnvironmentVariable("PLOQ_UPDATE_URL");
    if (!overrideUrl.isEmpty()) {
        const QUrl u(overrideUrl);
        if (u.isValid() && (u.scheme() == "https" || isLocalHttp(u))) {
            return overrideUrl;
        }
        qWarning("ploq: ignoring PLOQ_UPDATE_URL (must be https://, or http:// to this computer)");
    }
    return DEFAULT_FEED;
}

#ifdef Q_OS_WIN
static bool ecdsaP256Verify(const QByteArray& message, const QByteArray& signature)
{
    if (signature.size() != 64) {
        return false; // r||s, 32 bytes each (Node: dsaEncoding "ieee-p1363")
    }
    const QByteArray x = QByteArray::fromHex(RELEASE_KEY_X);
    const QByteArray y = QByteArray::fromHex(RELEASE_KEY_Y);
    QByteArray blob(sizeof(BCRYPT_ECCKEY_BLOB), 0);
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    blob += x;
    blob += y;

    const QByteArray digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))
        && BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                              reinterpret_cast<PUCHAR>(blob.data()), ULONG(blob.size()), 0))) {
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr,
                                                  reinterpret_cast<PUCHAR>(const_cast<char*>(digest.constData())), ULONG(digest.size()),
                                                  reinterpret_cast<PUCHAR>(const_cast<char*>(signature.constData())), ULONG(signature.size()),
                                                  0));
    }
    if (key) {
        BCryptDestroyKey(key);
    }
    if (alg) {
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

#else
static bool ecdsaP256Verify(const QByteArray&, const QByteArray&)
{
    return false; // Ploq updates are Windows-only for now: never trust an unverified update
}

#endif

static bool fail(QString* error, const QString& why)
{
    if (error) {
        *error = why;
    }
    qWarning("ploq: update rejected: %s", qPrintable(why));
    return false;
}

static bool validAssetName(const QString& name)
{
    // Only "Ploq-<version>.msi": also keeps the download inside the update folder.
    static const QRegularExpression re("^Ploq-[0-9]+(\\.[0-9]+){1,3}\\.msi$");
    return re.match(name).hasMatch();
}

QByteArray unwrapSignedRelease(const QByteArray& feed, QString* error)
{
    const QJsonObject envelope = QJsonDocument::fromJson(feed).object();
    const QByteArray signedText = envelope.value("signed").toString().toUtf8();
    const QByteArray signature = QByteArray::fromBase64(envelope.value("signature").toString().toLatin1());
    if (signedText.isEmpty() || signature.isEmpty()) {
        fail(error, "update description is not signed");
        return {};
    }
    if (!ecdsaP256Verify(signedText, signature)) {
        fail(error, "update description has an invalid signature");
        return {};
    }

    const QJsonObject release = QJsonDocument::fromJson(signedText).object();
    if (release.value("product").toString() != "ploq") {
        fail(error, "update description is not for Ploq");
        return {};
    }
    QHash<QString, Expected> expected;
    for (const QJsonValue& v : release.value("assets").toArray()) {
        const QJsonObject a = v.toObject();
        const QString name = a.value("name").toString();
        Expected e;
        e.size = a.value("size").toInteger(-1);
        e.sha256 = QByteArray::fromHex(a.value("sha256").toString().toLatin1());
        if (validAssetName(name) && e.size > 0 && e.sha256.size() == 32) {
            expected.insert(name, e);
        }
    }
    if (expected.isEmpty()) {
        fail(error, "signed update description lists no usable installer");
        return {};
    }
    QMutexLocker lock(&s_mutex);
    s_expected = expected;
    return signedText;
}

bool verifyDownloadedUpdate(const QString& fileName, const QByteArray& data, QString* error)
{
    if (!validAssetName(fileName)) {
        return fail(error, "unexpected installer name: " + fileName);
    }
    Expected e;
    {
        QMutexLocker lock(&s_mutex);
        if (!s_expected.contains(fileName)) {
            return fail(error, "installer is not listed in a signed update description");
        }
        e = s_expected.value(fileName);
    }
    if (data.size() != e.size) {
        return fail(error, QString("installer size %1, expected %2").arg(data.size()).arg(e.size));
    }
    if (QCryptographicHash::hash(data, QCryptographicHash::Sha256) != e.sha256) {
        return fail(error, "installer SHA-256 does not match the signed update description");
    }
    return true;
}

// CODE SIGNING (later): once Ploq's installers are Authenticode-signed
// (tools/collab/installer/sign.ps1), define PLOQ_REQUIRE_AUTHENTICODE in
// src/collab/CMakeLists.txt. The updater then also refuses installers without a
// valid Windows signature. Ideally also pin the publisher name here.
bool verifyAuthenticode(const QString& path, QString* error)
{
#if defined(Q_OS_WIN) && defined(PLOQ_REQUIRE_AUTHENTICODE)
    const std::wstring file = path.toStdWString();
    WINTRUST_FILE_INFO fileInfo {};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = file.c_str();
    WINTRUST_DATA data {};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    if (status != ERROR_SUCCESS) {
        return fail(error, QString("installer is not code-signed (WinVerifyTrust 0x%1)").arg(quint32(status), 8, 16, QChar('0')));
    }
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(error);
    return true; // not enforced yet: signed description + SHA-256 protect the download
#endif
}

bool launchVerifiedInstaller(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return fail(error, "cannot read the downloaded installer: " + path);
    }
    const QByteArray data = file.readAll();
    file.close();
    if (!verifyDownloadedUpdate(QFileInfo(path).fileName(), data, error)) {
        return false;
    }
    if (!verifyAuthenticode(path, error)) {
        return false;
    }
#ifdef Q_OS_WIN
    const QString msiexec = qEnvironmentVariable("SystemRoot", "C:\\Windows") + "\\System32\\msiexec.exe";
    // /passive: progress bar only. PLOQ_RELAUNCH=1: Ploq.wxs starts the new Ploq when done.
    const QStringList args { "/i", QDir::toNativeSeparators(path), "/passive", "PLOQ_RELAUNCH=1" };
    if (!QProcess::startDetached(msiexec, args)) {
        return fail(error, "could not start Windows Installer");
    }
    qInfo("ploq: installing update %s", qPrintable(QFileInfo(path).fileName()));
    return true;
#else
    return fail(error, "updates are Windows-only");
#endif
}
}
