/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the Share button and dialog (see sharecontroller.h).
 */
#include "sharecontroller.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QVariantMap>

#include "project/inotationproject.h"

#include "collabsyncservice.h"
#include "collabuiactions.h"

#include "muse_framework_config.h" // MUSE_APP_UNSTABLE (dev vs installed builds)

#include "log.h"

using namespace mu::collab;
using muse::IInteractive;

static ShareController* s_instance = nullptr;
static const char* SHARE_DIALOG_URI = "musescore://ploq/share";
static constexpr int UPLOAD_TIMEOUT_MS = 45000;

//! Automated sign-in tests have nobody to click "Sign in". Development builds
//! only: compiled out of release builds, where the question is always asked.
static bool qaSignInWithoutAsking()
{
#ifdef MUSE_APP_UNSTABLE
    return !qEnvironmentVariable("COLLAB_QA_BROWSER").isEmpty();
#else
    return false;
#endif
}

ShareController::ShareController(const muse::modularity::ContextPtr& ctx, CollabSyncService& service,
                                 CollabAccount& account)
    : muse::Contextable(ctx), m_service(service), m_account(account)
{
    m_uploadTimeout.setSingleShot(true);
    m_uploadTimeout.setInterval(UPLOAD_TIMEOUT_MS);
    QObject::connect(&m_uploadTimeout, &QTimer::timeout, this, [this]() {
        if (m_uploading) {
            m_uploading = false;
            setStatus(Status::Error, "The score didn't finish uploading. Check your connection and try again.");
        }
    });
}

ShareController::~ShareController()
{
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

ShareController* ShareController::instance()
{
    return s_instance;
}

void ShareController::init()
{
    s_instance = this;
    if (dispatcher()) {
        dispatcher()->reg(this, "ploq-share", [this]() { share(); });
        dispatcher()->reg(this, "ploq-manage-on-web", [this]() { manageOnWeb(); });
        dispatcher()->reg(this, "ploq-sign-out", [this]() { signOut(); });
    }
}

QString ShareController::statusName() const
{
    switch (m_status) {
    case Status::Idle: return "idle";
    case Status::SigningIn: return "signin";
    case Status::Working: return "working";
    case Status::Ready: return "ready";
    case Status::Limit: return "limit";
    case Status::Frozen: return "frozen";
    case Status::Error: return "error";
    }
    return "idle";
}

QString ShareController::accountName() const
{
    return m_account.isSignedIn() ? (m_account.name().isEmpty() ? QString("your Ploq account") : m_account.name())
           : QString();
}

QString ShareController::currentProjectId() const
{
    // The open score's own room (after closing a shared score and opening
    // another, that one isn't shared: the room was left)...
    QString id = m_service.sharedProjectId();
    if (id.isEmpty()) {
        id = m_service.joiningProjectId(); // ...the one we're on the way into...
    }
    if (id.isEmpty()) {
        // ...or the project this file was shared as / joined before (not
        // rejoined yet: signed out, or signed in only now through Share).
        const project::INotationProjectPtr project = globalContext()->currentProject();
        if (project && !project->path().empty()) {
            id = m_account.rememberedProject(project->path().toQString()).projectId;
        }
    }
    return id;
}

bool ShareController::isShared() const
{
    return !currentProjectId().isEmpty();
}

void ShareController::rejoinIfNeeded(const QString& projectId)
{
    if (m_service.sharedProjectId() == projectId || m_service.joiningProjectId() == projectId) {
        return;
    }
    const project::INotationProjectPtr project = globalContext()->currentProject();
    const CollabAccount::Project known = project && !project->path().empty()
                                         ? m_account.rememberedProject(project->path().toQString())
                                         : CollabAccount::Project();
    if (known.projectId == projectId) {
        LOGI() << "collab: share: rejoining \"" << known.title << "\" (shared before)";
        m_service.joinSharedProject(known);
    }
}

void ShareController::setStatus(Status status, const QString& message)
{
    m_status = status;
    m_message = message;
    if (status != Status::Limit) {
        m_upgradeUrl = QUrl();
    }
    if (status == Status::Error) {
        LOGW() << "collab: share: " << message;
    }
    if (status == Status::Error && !m_dialogOpen) {
        m_service.notify(message);
    }
    CollabUiActions::notifyEnabledChanged(); // "Sign out" follows the sign-in
    emit changed();
}

void ShareController::showDialog()
{
    if (!m_withDialog || m_dialogOpen || !interactive()) {
        return;
    }
    m_dialogOpen = true; // ShareDialogModel calls dialogClosed() when it goes away
    interactive()->open(muse::UriQuery(std::string(SHARE_DIALOG_URI)));
}

bool ShareController::updateTitle()
{
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project) {
        return false;
    }
    m_title = CollabAccount::cleanText(project->displayName(), 128);
    if (m_title.isEmpty()) {
        m_title = "Untitled score";
    }
    return true;
}

// ---------------------------------------------------------------------------
// Share
// ---------------------------------------------------------------------------

void ShareController::share(bool withDialog)
{
    if (!updateTitle()) {
        return; // the action is disabled without an open score
    }
    m_withDialog = withDialog;
    m_linkCopied = false;
    if (m_status == Status::Working) {
        showDialog(); // already on it
        return;
    }

    if (!m_account.isSignedIn()) {
        showDialog();
        requireSignIn([this, withDialog]() { share(withDialog); });
        return;
    }
    // Shared already, being joined, or shared before from this file: never
    // upload it again (a second copy would split its collaborators).
    if (const QString id = currentProjectId(); !id.isEmpty()) {
        m_projectId = id;
        showDialog();
        showSharedScore();
        return;
    }

    // Not shared yet: the upload is the saved score, so save first (asking).
    const project::INotationProjectPtr project = globalContext()->currentProject();
    const bool neverSaved = project->isNewlyCreated() || project->path().empty();
    if (neverSaved || project->needSave().val) {
        if (!withDialog) {
            if (!neverSaved) {
                project->save();
            }
        } else {
            const int saveBtn = int(IInteractive::Button::CustomButton) + 1;
            const IInteractive::Text text(
                (neverSaved
                 ? QString("Save “%1” first. Ploq shares the saved score.")
                 : QString("“%1” has unsaved changes. Ploq shares the saved score.")).arg(m_title).toStdString(),
                IInteractive::TextFormat::PlainText);
            interactive()->question("Save before sharing?", text,
                                    { interactive()->buttonData(IInteractive::Button::Cancel),
                                      IInteractive::ButtonData(saveBtn, "Save and share", true) },
                                    saveBtn)
            .onResolve(this, [this, saveBtn](const IInteractive::Result& res) {
                if (!res.isButton(saveBtn)) {
                    return;
                }
                dispatcher()->dispatch("file-save");
                const project::INotationProjectPtr p = globalContext()->currentProject();
                if (!p || p->isNewlyCreated() || p->path().empty() || p->needSave().val) {
                    LOGI() << "collab: share: the score wasn't saved; not sharing";
                    return; // Save As was cancelled
                }
                updateTitle();
                showDialog();
                shareUnsharedScore();
            });
            return;
        }
    }
    showDialog();
    shareUnsharedScore();
}

void ShareController::shareUnsharedScore()
{
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || project->path().empty()) {
        return;
    }
    m_scorePath = project->path().toQString();
    setStatus(Status::Working, QString("Uploading “%1”…").arg(m_title));
    m_account.createProject(m_title, [this](bool ok, const CollabAccount::Project& p, const CollabAccount::Error& err) {
        if (!ok) {
            if (err.code == "limit") {
                // A self-hosted server never sends this: it has no limits.
                m_upgradeUrl = err.upgradeUrl.isValid() ? err.upgradeUrl : m_account.webUrl("/account?upgrade=1");
                m_status = Status::Limit;
                m_message = err.text;
                LOGI() << "collab: share: hosted-score limit reached (" << err.text << "), upgrade page "
                       << m_upgradeUrl.toString();
                emit changed();
                if (!m_dialogOpen) {
                    m_withDialog = true;
                    showDialog();
                }
                return;
            }
            setStatus(Status::Error, err.text);
            return;
        }
        m_account.rememberProject(m_scorePath, p);
        m_projectId = p.projectId;
        LOGI() << "collab: shared \"" << p.title << "\" as project " << p.projectId;
        m_uploading = true;
        m_uploadTimeout.start();
        m_members.clear();
        m_service.joinSharedProject(p); // empty room: our score becomes the room's score
    });
}

void ShareController::onRoomScoreSaved()
{
    if (!m_uploading) {
        return;
    }
    m_uploading = false;
    m_uploadTimeout.stop();
    LOGI() << "collab: share: the room stored the score";
    makeInviteLink();
}

void ShareController::showSharedScore()
{
    // A link to hand out (owners), and who's in the score.
    setStatus(Status::Working, "Getting the invite link…");
    const QString projectId = m_projectId;
    m_account.projectInfo(projectId, [this, projectId](bool ok, const CollabAccount::ProjectInfo& info,
                                                       const CollabAccount::Error& err) {
        if (projectId != m_projectId) {
            return; // another Share meanwhile
        }
        if (!ok && err.status == 403 && m_service.sharedProjectId() != projectId) {
            // Shared from this file before, but we're no longer in that score
            // (removed, or it was deleted on the website): share it anew.
            LOGI() << "collab: share: not a member of the score this file was shared as any more; sharing it again";
            m_projectId.clear();
            shareUnsharedScore();
            return;
        }
        rejoinIfNeeded(projectId); // not in its room yet: live editing starts too
        if (ok && info.frozen) {
            onFrozen(info);
            return;
        }
        if (ok && info.role != "owner") {
            m_canInvite = false;
            m_link.clear();
            setStatus(Status::Ready, "Only the score's owner can make invite links.");
            loadMembers();
            return;
        }
        if (!ok && err.status == 403) {
            setStatus(Status::Error, err.text);
            return;
        }
        // Owner (or an older server without this call): make a link.
        makeInviteLink();
    });
}

void ShareController::makeInviteLink()
{
    setStatus(Status::Working, "Getting the invite link…");
    m_account.createInvite(m_projectId, [this](bool ok, const QString& url, const CollabAccount::Error& err) {
        if (!ok) {
            if (err.status == 403) {
                m_canInvite = false;
                m_link.clear();
                setStatus(Status::Ready, "Only the score's owner can make invite links.");
                loadMembers();
                return;
            }
            if (err.code == "frozen") {
                onFrozen({ m_projectId, m_title, QString(), QString(), true });
                return;
            }
            setStatus(Status::Error, err.text);
            return;
        }
        m_canInvite = true;
        m_link = url;
        QGuiApplication::clipboard()->setText(url);
        m_linkCopied = true;
#ifdef MUSE_APP_UNSTABLE
        LOGI() << "collab: invite link " << url; // development builds: the QA harness reads it from the log
#else
        // The link lets anyone join the score, and logs end up attached to bug
        // reports: only its start goes in the log.
        LOGI() << "collab: invite link made (" << url.left(url.lastIndexOf('/') + 5) << "…)";
#endif
        setStatus(Status::Ready, "Link copied");
        loadMembers();
    });
}

void ShareController::copyLink()
{
    if (m_link.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(m_link);
    m_linkCopied = true;
    m_message = "Link copied";
    emit changed();
}

// ---------------------------------------------------------------------------
// Members: from the server, "in the score now" from the room's presence
// ---------------------------------------------------------------------------

void ShareController::loadMembers()
{
    const QString projectId = m_projectId;
    m_account.members(projectId, [this, projectId](bool ok, const std::vector<CollabAccount::Member>& list,
                                                   const CollabAccount::Error& err) {
        if (projectId != m_projectId || projectId != currentProjectId()) {
            return; // switched scores meanwhile
        }
        if (!ok) {
            LOGW() << "collab: couldn't load the member list: " << err.text;
            return;
        }
        m_members = list;
        updateMemberRows();
    });
}

void ShareController::onPresence(const QJsonArray& peers)
{
    m_peers = peers;
    if (!m_members.empty()) {
        updateMemberRows();
    }
}

void ShareController::updateMemberRows()
{
    QVariantList rows;
    for (const CollabAccount::Member& m : m_members) {
        const bool you = m.userId == m_account.userId();
        bool online = you && m_service.isConnected();
        for (const QJsonValue& v : m_peers) {
            const QJsonObject p = v.toObject();
            const QString accountId = p.value("accountId").toString();
            // Match on the account id; servers that don't send it yet: on the name.
            if (accountId.isEmpty() ? p.value("name").toString() == m.name : accountId == m.accountId) {
                online = true;
            }
        }
        QVariantMap row;
        row["name"] = m.name;
        row["accountId"] = m.accountId;
        row["owner"] = m.role == "owner";
        row["you"] = you;
        row["online"] = online;
        rows.push_back(row);
    }
    int online = 0;
    for (const QVariant& r : rows) {
        online += r.toMap().value("online").toBool() ? 1 : 0;
    }
    if (rows != m_memberRows) {
        LOGI() << "collab: share: " << rows.size() << " member(s), " << online << " in the score now";
    }
    m_memberRows = rows;
    emit changed();
}

// ---------------------------------------------------------------------------
// Frozen, upgrade, manage, sign out
// ---------------------------------------------------------------------------

void ShareController::onFrozen(const CollabAccount::ProjectInfo& info, bool announce)
{
    m_uploading = false;
    m_uploadTimeout.stop();
    m_link.clear();
    m_canInvite = false;
    updateTitle();
    setStatus(Status::Frozen, info.role == "owner"
              ? "Your plan has lapsed. This score is download-only."
              : "The owner's plan has lapsed.");
    LOGI() << "collab: share state frozen: " << (info.role == "owner" ? "owner" : "member") << ", \""
           << (info.role == "owner" ? "Your plan has lapsed. This score is download-only." : "The owner's plan has lapsed.") << "\"";
    if (announce && !m_dialogOpen) {
        m_withDialog = true;
        showDialog();
    }
}

void ShareController::openUpgrade()
{
    if (m_upgradeUrl.isValid() && m_account.isServerUrl(m_upgradeUrl)) {
        interactive()->openUrl(m_upgradeUrl);
    }
}

void ShareController::manageOnWeb()
{
    const QString projectId = currentProjectId();
    const QUrl url = CollabAccount::isProjectId(projectId) ? m_account.webUrl("/scores/" + projectId) : m_account.webUrl("/");
    LOGI() << "collab: opening " << url.toString();
    interactive()->openUrl(url);
}

void ShareController::signOut()
{
    m_browser.cancel();
    m_afterSignIn = nullptr;
    m_account.signOut([this](bool, const CollabAccount::Error&) {
        m_service.leaveRoom();
        onSignedOutByServer();
        m_service.notifySignedOut();
    });
}

void ShareController::onSignedOutByServer()
{
    m_uploading = false;
    m_uploadTimeout.stop();
    m_members.clear();
    m_memberRows.clear();
    m_link.clear();
    if (m_status != Status::SigningIn) { // a sign-in already under way carries on
        setStatus(Status::Idle);
    }
}

void ShareController::retry()
{
    m_status = Status::Idle;
    share(true);
}

// ---------------------------------------------------------------------------
// Browser sign-in
// ---------------------------------------------------------------------------

void ShareController::requireSignIn(std::function<void()> then)
{
    if (m_account.isSignedIn()) {
        then();
        return;
    }
    m_afterSignIn = std::move(then);
    const bool started = m_browser.start(m_account.serverBase(),
                                         [this](const QString& code, const QString& verifier) { onSignInCode(code, verifier); },
                                         [this]() { failSignIn("Sign-in wasn't finished. Click Share to try again."); });
    if (!started) {
        failSignIn("Ploq couldn't open your browser to sign in.");
        return;
    }
    setStatus(Status::SigningIn, "Finish signing in in your browser.");
    if (!m_dialogOpen) {
        m_service.notify("Finish signing in in your browser.", /*info*/ true);
    }
}

void ShareController::reopenBrowser()
{
    if (m_browser.isActive()) {
        m_browser.start(m_account.serverBase(),
                        [this](const QString& code, const QString& verifier) { onSignInCode(code, verifier); },
                        [this]() { failSignIn("Sign-in wasn't finished. Click Share to try again."); });
    }
}

void ShareController::cancelSignIn()
{
    if (m_status == Status::SigningIn) {
        m_browser.cancel();
        m_afterSignIn = nullptr;
        setStatus(Status::Idle);
    }
}

void ShareController::failSignIn(const QString& why)
{
    m_afterSignIn = nullptr;
    setStatus(Status::Error, why);
}

void ShareController::onSignInCode(const QString& code, const QString& verifier)
{
    // The link matched the sign-in this app started (state) - still say who
    // it signs in as and ask, like every other link.
    m_account.previewAppSignIn(code, [this, code, verifier](bool ok, const CollabAccount::Preview& pv,
                                                             const CollabAccount::Error&) {
        if (!ok) {
            failSignIn("That sign-in has expired. Click Share to start again.");
            return;
        }
        const QString who = pv.email.isEmpty() ? pv.name : QString("%1 (%2)").arg(pv.name, pv.email);
        if (qaSignInWithoutAsking()) {
            LOGI() << "collab: QA run, signing in as " << who << " without asking";
            redeemSignIn(code, verifier);
            return;
        }
        const int signInBtn = int(IInteractive::Button::CustomButton) + 2;
        interactive()->question("Sign in to Ploq?",
                                IInteractive::Text(QString("Sign in to Ploq as %1?").arg(who).toStdString(),
                                                   IInteractive::TextFormat::PlainText),
                                { interactive()->buttonData(IInteractive::Button::Cancel),
                                  IInteractive::ButtonData(signInBtn, "Sign in", true) },
                                signInBtn)
        .onResolve(this, [this, code, verifier, signInBtn](const IInteractive::Result& res) {
            if (!res.isButton(signInBtn)) {
                LOGI() << "collab: browser sign-in declined";
                m_afterSignIn = nullptr;
                setStatus(Status::Idle, "Not signed in. Click Share to try again.");
                return;
            }
            redeemSignIn(code, verifier);
        });
    });
}

void ShareController::redeemSignIn(const QString& code, const QString& verifier)
{
    m_account.redeemAppSignIn(code, verifier, [this](bool ok, const CollabAccount::Error& err) {
        if (!ok) {
            failSignIn(err.text);
            return;
        }
        m_service.onSignedIn();
        setStatus(Status::Idle);
        std::function<void()> then = std::move(m_afterSignIn);
        m_afterSignIn = nullptr;
        if (then) {
            then();
        }
    });
}
