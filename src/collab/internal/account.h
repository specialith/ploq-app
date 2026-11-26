/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — accounts, projects and invite links (Phase 7).
 *
 * Talks to the sync server's HTTP API (ploq-server). "Signed in" = holding a
 * token the server gave this app through the browser sign-in (see
 * browsersignin.h) or a website "Open in Ploq" link. The token is kept in the
 * OS credential store, the name and user id in MuseScore's settings, under
 * <server>/<profile> (profile: COLLAB_PROFILE, default "default", so two test
 * instances on one PC can be different people).
 *
 * Everything the server sends back is checked here before the rest of the
 * app sees it: ids must have their exact shape, names and titles lose
 * control/invisible characters and are length-limited, and web links must
 * point at the configured server.
 */
#pragma once

#include <functional>
#include <vector>

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace mu::collab {
class CollabAccount
{
public:
    struct Project {
        QString projectId;
        QString room;
        QString title;
    };

    //! Why a call failed. `code` is the server's machine-readable reason
    //! ("limit", "frozen", ...), `upgradeUrl` only set (and checked) for "limit".
    struct Error {
        int status = 0;        // HTTP status, 0 = server unreachable
        QString text;          // for people
        QString code;
        QUrl upgradeUrl;
    };

    struct Member {
        QString userId;
        QString accountId;     // short id, to tell same-named people apart
        QString name;
        QString role;          // "owner" | "editor"
    };

    struct ProjectInfo {
        QString projectId;
        QString title;
        QString role;          // our role in it
        QString ownerName;
        bool frozen = false;   // the owner's plan lapsed: download-only
    };

    struct Preview {
        QString name;       // who the link signs in as / who invited you
        QString email;      // app sign-in codes only
        QString accountId;  // short account id, to tell same-named people apart
        QString title;      // the score (empty for an app sign-in)
    };

    using Done = std::function<void (bool ok, const Error& error)>;
    using ProjectDone = std::function<void (bool ok, const Project& project, const Error& error)>;
    using LinkDone = std::function<void (bool ok, const QString& url, const Error& error)>;
    using MembersDone = std::function<void (bool ok, const std::vector<Member>& members, const Error& error)>;
    using InfoDone = std::function<void (bool ok, const ProjectInfo& info, const Error& error)>;
    using PreviewDone = std::function<void (bool ok, const Preview& preview, const Error& error)>;

    CollabAccount(const QString& httpBase, const QString& profile);

    QString serverBase() const { return m_base; }
    bool isSignedIn() const { return !m_token.isEmpty(); }
    QString token() const { return m_token; }
    QString name() const { return m_name; }
    QString userId() const { return m_userId; }

    //! Our name and id from the server (after a sign-in that didn't say them).
    void fetchMe(Done done);

    //! Ends this sign-in on the server and forgets it here (even if the
    //! server can't be reached: the token is deleted from this computer).
    void signOut(Done done);

    // App sign-in through the browser (browsersignin.h hands us the code).
    void previewAppSignIn(const QString& code, PreviewDone done);
    void redeemAppSignIn(const QString& code, const QString& verifier, Done done);

    void createProject(const QString& title, ProjectDone done);
    void createInvite(const QString& projectId, LinkDone done);
    void acceptInvite(const QString& code, ProjectDone done);
    void members(const QString& projectId, MembersDone done);
    void projectInfo(const QString& projectId, InfoDone done);

    //! The invite code inside anything the user pastes: a link, ploq://join/<code>, or the code.
    static QString inviteCodeFrom(const QString& text);

    //! The one-time code in a website "Open in Ploq" link
    //! (ploq://open/<projectId>?code=<32 hex>), or empty.
    static QString loginCodeFrom(const QString& text);

    //! Trades that code for a sign-in as the website's user (replacing any
    //! sign-in this profile had) and returns the score to open. Ask first
    //! (previewLoginCode): a link from someone else would sign you in as them.
    void redeemLoginCode(const QString& code, ProjectDone done);

    //! What a website link or invite would do, without using it.
    void previewLoginCode(const QString& code, PreviewDone done);
    void previewInvite(const QString& code, PreviewDone done);

    //! Shared scores this profile has opened before: file path -> project.
    Project rememberedProject(const QString& scorePath) const;
    void rememberProject(const QString& scorePath, const Project& project);

    //! A page on the configured server's website, e.g. webUrl("/scores/p_...").
    QUrl webUrl(const QString& path) const;

    //! Only pages on the configured server (same scheme, host and port) may be
    //! opened from something the server sent.
    bool isServerUrl(const QUrl& url) const;

    // Shapes of what the server hands out.
    static bool isProjectId(const QString& s);
    static bool isRoom(const QString& s);
    static bool isUserId(const QString& s);
    //! Plain, single-line text: no control or invisible direction characters, at most `max` long.
    static QString cleanText(const QString& s, int max);

private:
    //! withToken = false: sent without our sign-in (sign-in codes are their own
    //! proof), which stays in use meanwhile.
    void call(const QString& method, const QString& path, const QJsonObject& body,
              std::function<void(int status, const QJsonObject& reply)> done, bool withToken = true);
    Error errorFrom(int status, const QJsonObject& reply) const;
    Project projectFrom(const QJsonObject& json) const;
    bool takeSignIn(const QJsonObject& reply);
    void load();
    void save();

    QString m_base;
    QString m_profile;
    QString m_token;
    QString m_userId;
    QString m_name;
    QNetworkAccessManager m_net;
};
}
