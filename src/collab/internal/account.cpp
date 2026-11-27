/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — accounts, projects and invite links (Phase 7).
 */
#include "account.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include "global/settings.h"

#include "credentialstore.h"

#include "log.h"

using namespace mu::collab;
using muse::Settings;
using muse::Val;

static Settings::Key key(const QString& profile, const char* name)
{
    return Settings::Key("collab", QString("collab/%1/%2").arg(profile, name).toStdString());
}

CollabAccount::CollabAccount(const QString& httpBase, const QString& profile)
    : m_base(httpBase), m_profile(profile)
{
    while (m_base.endsWith('/')) {
        m_base.chop(1);
    }
    // Profile names become settings keys: keep them simple.
    m_profile.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    if (m_profile.isEmpty()) {
        m_profile = "default";
    }
    // Accounts belong to one server: keep each server's sign-in separate
    // (e.g. collab.ploq.app vs a self-hosted or local test server).
    QString server = QUrl(m_base).authority();
    server.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
    m_profile = server + "/" + m_profile;
    load();
}

// ---------------------------------------------------------------------------
// What the server sends: shapes and clean text
// ---------------------------------------------------------------------------

bool CollabAccount::isProjectId(const QString& s)
{
    static const QRegularExpression re("^p_[0-9a-f]{24}$");
    return re.match(s).hasMatch();
}

bool CollabAccount::isRoom(const QString& s)
{
    static const QRegularExpression re("^[0-9a-f]{32}$");
    return re.match(s).hasMatch();
}

bool CollabAccount::isUserId(const QString& s)
{
    static const QRegularExpression re("^u_[0-9a-f]{24}$");
    return re.match(s).hasMatch();
}

QString CollabAccount::cleanText(const QString& s, int max)
{
    // Control characters, and invisible direction / zero-width characters that
    // could make one name look like another (same set as the server strips).
    static const QRegularExpression invisible(
        "[\\x{0000}-\\x{001f}\\x{007f}-\\x{009f}\\x{061c}\\x{200b}-\\x{200f}\\x{202a}-\\x{202e}\\x{2060}-\\x{2069}\\x{feff}]");
    QString out = s;
    out.replace(invisible, QString());
    return out.trimmed().left(max);
}

static QString shortIdOf(const QString& id)
{
    return QString(id).remove(QRegularExpression("^[up]_")).left(6);
}

QUrl CollabAccount::webUrl(const QString& path) const
{
    return QUrl(m_base + path);
}

bool CollabAccount::isServerUrl(const QUrl& url) const
{
    const QUrl base(m_base);
    return url.isValid() && url.scheme() == base.scheme() && url.host() == base.host()
           && url.port(base.scheme() == "https" ? 443 : 80) == base.port(base.scheme() == "https" ? 443 : 80)
           && url.userInfo().isEmpty();
}

CollabAccount::Project CollabAccount::projectFrom(const QJsonObject& json) const
{
    Project p { json.value("projectId").toString(), json.value("room").toString(),
                cleanText(json.value("title").toString(), 128) };
    if (!isProjectId(p.projectId) || !isRoom(p.room)) {
        return {};
    }
    if (p.title.isEmpty()) {
        p.title = "Untitled score";
    }
    return p;
}

CollabAccount::Error CollabAccount::errorFrom(int status, const QJsonObject& reply) const
{
    Error e;
    e.status = status;
    if (status == 0) {
        e.text = "Can't reach the collaboration server. Check your internet connection.";
        return e;
    }
    e.code = cleanText(reply.value("code").toString(), 32);
    e.text = cleanText(reply.value("error").toString(), 300);
    if (e.text.isEmpty()) {
        e.text = QString("Server error %1").arg(status);
    }
    if (e.code == "limit") {
        const QUrl upgrade(reply.value("upgradeUrl").toString());
        if (isServerUrl(upgrade)) {
            e.upgradeUrl = upgrade;
        } else {
            LOGW() << "collab: ignoring an upgrade link that isn't on " << m_base;
        }
    }
    return e;
}

// ---------------------------------------------------------------------------
// Stored sign-in
// ---------------------------------------------------------------------------

// The sign-in token lives in the OS credential store (Windows Credential
// Manager) where available; only the name and user id go in MuseScore's
// plain-text settings. A token found in the settings file (older builds) is
// moved into the credential store.
static QString credentialTarget(const QString& profile)
{
    return "Ploq/" + profile;
}

void CollabAccount::load()
{
    m_token = credentials::read(credentialTarget(m_profile));
    if (m_token.isEmpty()) {
        // Saved before the rename to Ploq: move it to the new name.
        const QString oldTarget = "CollabScore/" + m_profile;
        m_token = credentials::read(oldTarget);
        if (!m_token.isEmpty() && credentials::write(credentialTarget(m_profile), m_token)) {
            credentials::write(oldTarget, QString());
        }
    }
    const QString legacy = muse::settings()->value(key(m_profile, "token")).toQString();
    if (m_token.isEmpty() && !legacy.isEmpty()) {
        m_token = legacy;
        if (credentials::write(credentialTarget(m_profile), legacy)) {
            muse::settings()->setSharedValue(key(m_profile, "token"), Val(QString()));
            LOGI() << "collab: moved the sign-in token into the credential store";
        }
    }
    m_userId = muse::settings()->value(key(m_profile, "userId")).toQString();
    m_name = muse::settings()->value(key(m_profile, "name")).toQString();
}

void CollabAccount::save()
{
    const bool stored = credentials::write(credentialTarget(m_profile), m_token);
#if defined(Q_OS_LINUX) || defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    // The credential store (Credential Manager, Keychain, libsecret keyring) or nothing: if saving there fails,
    // the sign-in lasts until Ploq closes. Never the plain-text settings file.
    Q_UNUSED(stored);
    muse::settings()->setSharedValue(key(m_profile, "token"), Val(QString()));
#else
    // Plain-text fallback only where there's no credential store.
    muse::settings()->setSharedValue(key(m_profile, "token"), Val(stored ? QString() : m_token));
#endif
    muse::settings()->setSharedValue(key(m_profile, "userId"), Val(m_userId));
    muse::settings()->setSharedValue(key(m_profile, "name"), Val(m_name));
}

bool CollabAccount::takeSignIn(const QJsonObject& r)
{
    static const QRegularExpression tokenShape("^[A-Za-z0-9_-]{20,200}$");
    const QString token = r.value("token").toString();
    const QString userId = r.value("userId").toString();
    if (!tokenShape.match(token).hasMatch() || !isUserId(userId)) {
        return false;
    }
    m_token = token;
    m_userId = userId;
    m_name = cleanText(r.value("name").toString(), 40);
    save();
    return true;
}

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------

void CollabAccount::call(const QString& method, const QString& path, const QJsonObject& body,
                         std::function<void(int, const QJsonObject&)> done, bool withToken)
{
    QNetworkRequest req(QUrl(m_base + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const bool sentToken = withToken && !m_token.isEmpty();
    if (sentToken) {
        req.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    }
    req.setTransferTimeout(15000);
    const QByteArray payload = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    QNetworkReply* reply = m_net.sendCustomRequest(req, method.toUtf8(), payload);
    QObject::connect(reply, &QNetworkReply::finished, reply, [this, reply, done, sentToken]() {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // Replies are small JSON objects; anything big is not one.
        const QByteArray bytes = reply->read(2 * 1024 * 1024);
        const QJsonObject json = QJsonDocument::fromJson(bytes).object();
        if (status == 0) {
            LOGW() << "collab: server unreachable: " << reply->errorString();
        }
        if (status == 401 && sentToken) {
            // The sign-in expired or was signed out elsewhere: forget it, so
            // the next Share signs in again instead of failing every time.
            LOGW() << "collab: sign-in no longer valid, forgetting it";
            m_token.clear();
            save();
        }
        reply->deleteLater();
        done(status, json);
    });
}

void CollabAccount::fetchMe(Done done)
{
    call("GET", "/api/me", QJsonObject(), [this, done](int status, const QJsonObject& r) {
        const QString userId = r.value("userId").toString();
        if (status != 200 || !isUserId(userId)) {
            done(false, errorFrom(status, r));
            return;
        }
        m_userId = userId;
        m_name = cleanText(r.value("name").toString(), 40);
        save();
        done(true, {});
    });
}

void CollabAccount::signOut(Done done)
{
    if (!isSignedIn()) {
        done(true, {});
        return;
    }
    call("POST", "/api/signout", QJsonObject(), [this, done](int status, const QJsonObject& r) {
        // Forget it here whatever the server said: signing out must always work.
        m_token.clear();
        m_userId.clear();
        m_name.clear();
        save();
        LOGI() << "collab: signed out";
        done(status == 200 || status == 401, status == 200 || status == 401 ? Error {} : errorFrom(status, r));
    });
}

void CollabAccount::previewAppSignIn(const QString& code, PreviewDone done)
{
    // Asked without our token: the preview is about the code, not about us.
    call("GET", "/api/login/" + code, QJsonObject(), [done](int status, const QJsonObject& r) {
        Preview p { cleanText(r.value("name").toString(), 40), cleanText(r.value("email").toString(), 254),
                    cleanText(r.value("accountId").toString(), 12), QString() };
        done(status == 200 && !p.name.isEmpty(), p, {});
    }, /*withToken*/ false);
}

void CollabAccount::redeemAppSignIn(const QString& code, const QString& verifier, Done done)
{
    // Sent without our token (the code + verifier are the proof), which stays
    // in use until this one replaces it.
    call("POST", "/api/login/" + code, QJsonObject { { "verifier", verifier } },
         [this, done](int status, const QJsonObject& r) {
        if (status != 200 || !takeSignIn(r)) { // keeps whatever sign-in we had
            Error e = errorFrom(status, r);
            if (status == 404) {
                e.text = "That sign-in has expired. Click Share to start again.";
            }
            done(false, e);
            return;
        }
        LOGI() << "collab: signed in through the browser as \"" << m_name << "\"";
        done(true, {});
    });
}

void CollabAccount::createProject(const QString& title, ProjectDone done)
{
    call("POST", "/api/projects", QJsonObject { { "title", cleanText(title, 128) } },
         [this, done](int status, const QJsonObject& r) {
        const Project p = projectFrom(r);
        const bool ok = status == 200 && !p.room.isEmpty();
        done(ok, p, ok ? Error {} : errorFrom(status, r));
    });
}

void CollabAccount::createInvite(const QString& projectId, LinkDone done)
{
    if (!isProjectId(projectId)) {
        done(false, QString(), Error { -1, "Not a shared score." });
        return;
    }
    call("POST", "/api/projects/" + projectId + "/invites", QJsonObject(), [this, done](int status, const QJsonObject& r) {
        // Only an invite link of the exact shape, on this server's host with its
        // scheme (never http:// elsewhere); it goes on the clipboard. The port may
        // differ: the server names its public address, which a test proxy or a
        // self-hoster's reverse proxy may put on another port.
        static const QRegularExpression shape("^https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?/j/[0-9a-f]{24}\\z"); // \z: $ allows a final newline
        const QString url = r.value("url").toString();
        const QUrl link(url);
        const QUrl base(m_base);
        if (status == 200 && shape.match(url).hasMatch() && link.scheme() == base.scheme()
            && link.host().compare(base.host(), Qt::CaseInsensitive) == 0) {
            done(true, url, {});
            return;
        }
        Error e = errorFrom(status, r);
        if (status == 200) {
            e.text = "The server sent an invite link Ploq didn't expect.";
        }
        done(false, QString(), e);
    });
}

void CollabAccount::acceptInvite(const QString& code, ProjectDone done)
{
    call("POST", "/api/invites/" + code + "/accept", QJsonObject(), [this, done](int status, const QJsonObject& r) {
        const Project p = projectFrom(r);
        const bool ok = status == 200 && !p.room.isEmpty();
        done(ok, p, ok ? Error {} : errorFrom(status, r));
    });
}

void CollabAccount::members(const QString& projectId, MembersDone done)
{
    if (!isProjectId(projectId)) {
        done(false, {}, Error { -1, "Not a shared score." });
        return;
    }
    call("GET", "/api/projects/" + projectId + "/members", QJsonObject(), [this, done](int status, const QJsonObject& r) {
        if (status != 200) {
            done(false, {}, errorFrom(status, r));
            return;
        }
        std::vector<Member> list;
        for (const QJsonValue& v : r.value("members").toArray()) {
            const QJsonObject m = v.toObject();
            Member member { m.value("userId").toString(), QString(), cleanText(m.value("name").toString(), 40),
                            m.value("role").toString() == "owner" ? "owner" : "editor" };
            if (!isUserId(member.userId)) {
                continue;
            }
            member.accountId = shortIdOf(member.userId);
            if (member.name.isEmpty()) {
                member.name = "Someone";
            }
            list.push_back(member);
            if (list.size() >= 500) {
                break;
            }
        }
        done(true, list, {});
    });
}

void CollabAccount::projectInfo(const QString& projectId, InfoDone done)
{
    if (!isProjectId(projectId)) {
        done(false, {}, Error { -1, "Not a shared score." });
        return;
    }
    call("GET", "/api/projects/" + projectId, QJsonObject(), [this, done, projectId](int status, const QJsonObject& r) {
        if (status != 200) {
            done(false, {}, errorFrom(status, r));
            return;
        }
        ProjectInfo info;
        info.projectId = projectId;
        info.title = cleanText(r.value("title").toString(), 128);
        info.role = r.value("role").toString() == "owner" ? "owner" : "editor";
        info.ownerName = cleanText(r.value("ownerName").toString(), 40);
        info.frozen = r.value("frozen").toBool();
        done(true, info, {});
    });
}

QString CollabAccount::inviteCodeFrom(const QString& text)
{
    // Codes are 24 hex chars; take the last one in whatever was pasted.
    static const QRegularExpression re("([0-9a-f]{24})(?![0-9a-f])");
    QString code;
    auto it = re.globalMatch(text.trimmed().toLower());
    while (it.hasNext()) {
        code = it.next().captured(1);
    }
    return code;
}

QString CollabAccount::loginCodeFrom(const QString& text)
{
    static const QRegularExpression re("open/p_[0-9a-f]{24}\\?code=([0-9a-f]{32})(?![0-9a-f])");
    const QRegularExpressionMatch m = re.match(text.trimmed().toLower());
    return m.hasMatch() ? m.captured(1) : QString();
}

void CollabAccount::previewLoginCode(const QString& code, PreviewDone done)
{
    call("GET", "/api/login/" + code, QJsonObject(), [this, done](int status, const QJsonObject& r) {
        Preview p { cleanText(r.value("name").toString(), 40), cleanText(r.value("email").toString(), 254),
                    cleanText(r.value("accountId").toString(), 12), cleanText(r.value("title").toString(), 128) };
        done(status == 200, p, status == 200 ? Error {} : errorFrom(status, r));
    });
}

void CollabAccount::previewInvite(const QString& code, PreviewDone done)
{
    call("GET", "/api/invites/" + code, QJsonObject(), [this, done](int status, const QJsonObject& r) {
        Preview p { cleanText(r.value("inviter").toString(), 40), QString(),
                    cleanText(r.value("inviterId").toString(), 12), cleanText(r.value("title").toString(), 128) };
        done(status == 200, p, status == 200 ? Error {} : errorFrom(status, r));
    });
}

void CollabAccount::redeemLoginCode(const QString& code, ProjectDone done)
{
    // Sent without a token: the code itself is the proof.
    call("POST", "/api/login/" + code, QJsonObject(), [this, done](int status, const QJsonObject& r) {
        const Project project = projectFrom(r.value("project").toObject());
        if (status != 200 || project.room.isEmpty() || !takeSignIn(r)) { // keeps whatever sign-in we had
            done(false, project, errorFrom(status, r));
            return;
        }
        LOGI() << "collab: signed in from the website as \"" << m_name << "\"";
        done(true, project, {});
    });
}

CollabAccount::Project CollabAccount::rememberedProject(const QString& scorePath) const
{
    const QJsonObject all = QJsonDocument::fromJson(
        muse::settings()->value(key(m_profile, "projects")).toQString().toUtf8()).object();
    const QJsonObject p = all.value(scorePath).toObject();
    Project project { p.value("projectId").toString(), p.value("room").toString(), p.value("title").toString() };
    if (!isProjectId(project.projectId) || !isRoom(project.room)) {
        return {};
    }
    return project;
}

void CollabAccount::rememberProject(const QString& scorePath, const Project& project)
{
    QJsonObject all = QJsonDocument::fromJson(
        muse::settings()->value(key(m_profile, "projects")).toQString().toUtf8()).object();
    all.insert(scorePath, QJsonObject {
        { "projectId", project.projectId },
        { "room", project.room },
        { "title", project.title },
    });
    muse::settings()->setSharedValue(key(m_profile, "projects"),
                                     Val(QString::fromUtf8(QJsonDocument(all).toJson(QJsonDocument::Compact))));
}
