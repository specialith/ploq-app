/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the Share button, the Share dialog's state, browser sign-in,
 * "Manage on web" and "Sign out".
 *
 * The website is the hub: the app only shares (uploads the score, copies an
 * invite link), shows who's in the score, and links to the score's page on
 * the site for everything else (removing people, turning invite links off,
 * deleting, billing).
 *
 * Actions (registered by CollabModule, see collabuiactions.cpp):
 *   ploq-share          the Share button (also Ctrl+Alt+I)
 *   ploq-manage-on-web  this score's page on the site (or "your scores")
 *   ploq-sign-out
 * The dialog (qml/Ploq/Collab/ShareDialog.qml) reads this object through
 * ShareDialogModel.
 */
#pragma once

#include <functional>
#include <vector>

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "actions/actionable.h"
#include "actions/iactionsdispatcher.h"
#include "context/iglobalcontext.h"
#include "global/iinteractive.h"

#include "account.h"
#include "browsersignin.h"

namespace mu::collab {
class CollabSyncService;

class ShareController : public QObject, public muse::async::Asyncable, public muse::actions::Actionable,
    public muse::Contextable
{
    Q_OBJECT

    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::ContextInject<muse::IInteractive> interactive = { this };
    muse::ContextInject<muse::actions::IActionsDispatcher> dispatcher = { this };

public:
    //! What the dialog shows.
    enum class Status {
        Idle,       // nothing yet
        SigningIn,  // waiting for the browser
        Working,    // uploading / getting the link
        Ready,      // link (or member list) shown
        Limit,      // the plan's hosted-score limit
        Frozen,     // the owner's plan lapsed
        Error
    };

    ShareController(const muse::modularity::ContextPtr& ctx, CollabSyncService& service, CollabAccount& account);
    ~ShareController() override;

    //! The controller of the running app (nullptr when collaboration is off).
    static ShareController* instance();

    void init();

    // --- actions -----------------------------------------------------------
    //! The Share button. `showDialog` false: COLLAB_HOST=1 test runs (log only).
    void share(bool showDialog = true);
    void manageOnWeb();
    void signOut();

    // --- for the dialog --------------------------------------------------------
    Status status() const { return m_status; }
    QString statusName() const;
    QString scoreTitle() const { return m_title; }
    QString message() const { return m_message; }
    QString inviteLink() const { return m_link; }
    bool linkCopied() const { return m_linkCopied; }
    bool canInvite() const { return m_canInvite; }
    bool hasUpgrade() const { return m_upgradeUrl.isValid(); }
    QString accountName() const;
    bool isSignedIn() const { return m_account.isSignedIn(); }
    bool isShared() const;
    //! The open score's shared project: its room's, the one being joined, or
    //! the one this file was shared as before. "" if none.
    QString currentProjectId() const;
    //! [{name, accountId, owner (bool), you (bool), online (bool)}]
    QVariantList members() const { return m_memberRows; }

    void copyLink();
    void openUpgrade();
    void retry();
    void reopenBrowser();
    void cancelSignIn();
    void dialogClosed() { m_dialogOpen = false; }

    // --- from CollabSyncService ---------------------------------------------
    //! Runs `then` once signed in, starting the browser sign-in if needed.
    void requireSignIn(std::function<void()> then);
    //! The room confirmed it stored our score (the upload that shares it).
    void onRoomScoreSaved();
    void onPresence(const QJsonArray& peers);
    //! The score is download-only. `announce`: open the dialog to say so.
    void onFrozen(const CollabAccount::ProjectInfo& info, bool announce = false);
    //! Signed out (here, or the server refused the sign-in): back to "not shared".
    void onSignedOutByServer();

signals:
    void changed();

private:
    void setStatus(Status status, const QString& message = QString());
    void showDialog();
    void shareUnsharedScore();
    void showSharedScore();
    void rejoinIfNeeded(const QString& projectId);
    void makeInviteLink();
    void loadMembers();
    void updateMemberRows();
    void onSignInCode(const QString& code, const QString& verifier);
    void redeemSignIn(const QString& code, const QString& verifier);
    void failSignIn(const QString& why);
    bool updateTitle();

    CollabSyncService& m_service;
    CollabAccount& m_account;
    BrowserSignIn m_browser;
    std::function<void()> m_afterSignIn;

    Status m_status = Status::Idle;
    QString m_title;
    QString m_message;
    QString m_link;
    bool m_linkCopied = false;
    bool m_canInvite = true;
    QUrl m_upgradeUrl;
    bool m_dialogOpen = false;
    bool m_withDialog = true;
    bool m_uploading = false;        // shared; waiting for the room to store the score
    QTimer m_uploadTimeout;
    QString m_scorePath;             // the score being shared
    QString m_projectId;             // the shared score the dialog is about

    std::vector<CollabAccount::Member> m_members;
    QJsonArray m_peers;              // who's connected to the room now
    QVariantList m_memberRows;
};
}
