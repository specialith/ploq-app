/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — edit sync (Phase 3).
 */
#include "collabsyncservice.h"

#include "muse_framework_config.h" // MUSE_APP_UNSTABLE (dev vs installed builds)

#include <algorithm>
#include <climits>

#include <QBuffer>
#include <QClipboard>
#include <QMessageBox>
#include <QPushButton>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QWindow>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QWebSocket>

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/input.h"
#include "engraving/dom/masterscore.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/score.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/select.h"
#include "engraving/editing/undo.h"
#include "notation/inotation.h"
#include "notation/inotationinteraction.h"
#include "project/inotationproject.h"
#include "ui/view/iconcodes.h"
#include "global/io/buffer.h"
#include "global/serialization/zipreader.h"

#include "log.h"

#ifdef Q_OS_MACOS
#include "../ploqmac.h" // macCollabStarted
#endif

using namespace mu::collab;
using namespace mu::engraving;

static QString envOr(const char* name, const QString& fallback)
{
    const QString v = qEnvironmentVariable(name);
    return v.isEmpty() ? fallback : v;
}

static qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

CollabSyncService::CollabSyncService(const muse::modularity::ContextPtr& iocCtx)
    : muse::Contextable(iocCtx)
{
}

CollabSyncService::~CollabSyncService() = default;

static CollabSyncService* s_activeService = nullptr;

// Collab notices fade out on their own (MuseScore's showWarning() etc. stay
// until clicked, which piles them up).
void CollabSyncService::showToast(ToastKind kind, const std::string& title, const std::string& message)
{
    if (!toastService()) {
        return;
    }
    muse::ui::IconCode::Code icon = muse::ui::IconCode::Code::INFO;
    int seconds = 5;
    switch (kind) {
    case ToastKind::Warning: icon = muse::ui::IconCode::Code::WARNING;
        seconds = 4;
        break;
    case ToastKind::Info: icon = muse::ui::IconCode::Code::INFO;
        seconds = 6;
        break;
    case ToastKind::Success: icon = muse::ui::IconCode::Code::TICK;
        seconds = 10;
        break;
    case ToastKind::Error: icon = muse::ui::IconCode::Code::ERROR;
        seconds = 8;
        break;
    }
    toastService()->showWithTimeout(title, message, std::chrono::seconds(seconds), icon);
}

CollabSyncService* CollabSyncService::active()
{
    return s_activeService;
}

void CollabSyncService::init()
{
#ifdef Q_OS_MACOS
    ploq::macCollabStarted([this](const QString& link) { return openLinkIfIdle(link); }); // COLLAB_INVITE is read below
#endif
    m_serverUrl = qEnvironmentVariable("COLLAB_SERVER");
#ifndef MUSE_APP_UNSTABLE
    if (m_serverUrl.isEmpty()) {
        m_serverUrl = "https://collab.ploq.app"; // installed Ploq: the real server (dev builds stay off unless set)
    }
#endif
    if (m_serverUrl.isEmpty()) {
        LOGI() << "collab: COLLAB_SERVER not set, collaboration off";
        return;
    }

    const qint64 pid = QCoreApplication::applicationPid();

    m_enabled = true;
    // Phase 7: an http(s) server address means accounts + projects + invite
    // links; a ws:// address means plain room names (the local test relay).
    m_accountsMode = m_serverUrl.startsWith("http://") || m_serverUrl.startsWith("https://");
    if (m_accountsMode && m_serverUrl.startsWith("http://")) {
        // Plain http would send the sign-in token unencrypted: only allowed
        // for a test server on this computer.
        const QString host = QUrl(m_serverUrl).host();
        if (host != "localhost" && host != "127.0.0.1") {
            LOGW() << "collab: refusing unencrypted server " << m_serverUrl << " (use https://)";
            m_enabled = false;
            return;
        }
    }
    if (m_accountsMode) {
        while (m_serverUrl.endsWith('/')) {
            m_serverUrl.chop(1);
        }
        m_wsBase = QString(m_serverUrl).replace(QRegularExpression("^http"), "ws");
        m_account = std::make_unique<CollabAccount>(m_serverUrl, envOr("COLLAB_PROFILE", "default"));
        m_share = std::make_unique<ShareController>(iocContext(), *this, *m_account);
        m_share->init();
        // COLLAB_INVITE holds the clicked link: a website "Open in Ploq"
        // link (one-time sign-in code) or an invite link/code.
        const QString link = qEnvironmentVariable("COLLAB_INVITE");
        m_loginCode = CollabAccount::loginCodeFrom(link);
        m_inviteCode = m_loginCode.isEmpty() ? CollabAccount::inviteCodeFrom(link) : QString();
#ifdef MUSE_APP_UNSTABLE
        // COLLAB_HOST (tests, development builds only): share the score we open;
        // 1 quietly, 2 through the Share dialog.
        m_hostRequested = !qEnvironmentVariable("COLLAB_HOST").isEmpty();
        m_hostWithDialog = qEnvironmentVariable("COLLAB_HOST") == "2";
#endif
    } else {
        m_room = envOr("COLLAB_ROOM", "default");
    }
    m_name = envOr("COLLAB_NAME", QString("Player %1").arg(pid));
    // Never blue (MuseScore's own selection colour): CursorLayer::safeColor
    // swaps a blue-ish or missing colour for a palette one.
    m_color = CursorLayer::safeColor(qEnvironmentVariable("COLLAB_COLOR"), pid);

    m_socket = std::make_unique<QWebSocket>();
    QObject::connect(m_socket.get(), &QWebSocket::connected, m_socket.get(), [this]() { onConnected(); });
    QObject::connect(m_socket.get(), &QWebSocket::disconnected, m_socket.get(), [this]() {
        const bool wasConnected = m_connected;
        const int closeCode = m_socket->closeCode();
        if (m_connected) {
            LOGW() << "collab: disconnected from " << m_serverUrl << " (close code " << closeCode << ")";
        }
        m_connected = false;
        m_caughtUp = false; // edits from now on go to the outbox
        m_structureEditId = -1; // its ack is lost with the connection; the catch-up sorts it out
        m_cursors.clear();
        // Edits the server never acknowledged may not have reached it: their
        // measures go out again, as they are then, once we're back. Not after
        // leaving the room (leaveRoom): they belong to a score that's closed.
        for (const OwnEdit& e : m_ownEdits) {
            if (e.seq < 0 && !m_room.isEmpty()) {
                addToOutbox(e.range);
            }
        }
        m_ownEdits.clear();
        if (m_share) {
            m_share->onPresence(QJsonArray());
        }
        if (m_accountsMode && closeCode == 4005) {
            // The room closed for good: the owner's plan lapsed (score frozen).
            LOGI() << "collab: the room was frozen (owner's plan lapsed)";
            m_roomRefused = true;
            onRoomRefused();
            return;
        }
        if (m_accountsMode && !wasConnected && !m_room.isEmpty()) {
            // Never got in: after a few tries, ask the server why (frozen,
            // removed from the score, signed out...) instead of retrying forever.
            if (++m_failedConnects % 3 == 0) {
                onRoomRefused();
            }
        }
        if (!m_roomRefused && !m_room.isEmpty()) {
            scheduleReconnect();
        }
    });
    QObject::connect(m_socket.get(), &QWebSocket::textMessageReceived, m_socket.get(), [this](const QString& text) {
        onTextMessage(text);
    });

    m_reconnectTimer.setSingleShot(true);
    m_reconnectTimer.setInterval(2000);
    QObject::connect(&m_reconnectTimer, &QTimer::timeout, [this]() { connectToServer(); });

    m_retryApplyTimer.setSingleShot(true);
    m_retryApplyTimer.setInterval(25);
    QObject::connect(&m_retryApplyTimer, &QTimer::timeout, [this]() { processRemoteQueue(); });

    m_flushTimer.setSingleShot(true);
    m_flushTimer.setInterval(100);
    QObject::connect(&m_flushTimer, &QTimer::timeout, [this]() { flushOutbox(); });

    m_cursors.setLocalColor(m_color);
    m_cursors.setSend([this](const QJsonObject& msg) {
        if (m_connected) {
            m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
        }
    });
    s_activeService = this;

    globalContext()->currentMasterNotationChanged().onNotify(this, [this]() {
        onMasterNotationChanged();
    });
    onMasterNotationChanged();

    if (m_accountsMode) {
        if (m_account->isSignedIn() && m_account->name().isEmpty()) {
            m_account->fetchMe([this](bool ok, const CollabAccount::Error&) {
                if (ok) {
                    m_name = m_account->name();
                }
            });
        }
        startAccountFlow();
        installInviteShortcut();
    } else {
        connectToServer();
    }
}

// ---------------------------------------------------------------------------
// Accounts, projects, invite links (Phase 7)
// ---------------------------------------------------------------------------

//! Ask before acting on a link: a link can come from anyone, and using it
//! signs you in / opens someone's score. Plain text only (names and titles
//! come from other people), and "No" is the default.
static bool askUser(const QString& title, const QString& text, const QString& yesText)
{
    QMessageBox box;
    box.setWindowTitle(title);
    box.setTextFormat(Qt::PlainText);
    box.setText(text);
    box.setIcon(QMessageBox::Question);
    QPushButton* yes = box.addButton(yesText, QMessageBox::AcceptRole);
    QPushButton* no = box.addButton("Cancel", QMessageBox::RejectRole);
    box.setDefaultButton(no);
    box.exec();
    return box.clickedButton() == yes;
}

//! Automated two-instance tests (COLLAB_AUTOBOT) have nobody to click "Join".
//! Development builds only: compiled out of release builds, so in a real
//! Ploq an invite link always asks first.
static bool qaAutoConfirm()
{
#ifdef MUSE_APP_UNSTABLE
    if (!qEnvironmentVariable("COLLAB_AUTOBOT").isEmpty()) {
        LOGI() << "collab: QA run, joining without asking";
        return true;
    }
#endif
    return false;
}

static QString who(const CollabAccount::Preview& p)
{
    return p.accountId.isEmpty() ? p.name : QString("%1 (account %2)").arg(p.name, p.accountId);
}

bool CollabSyncService::openLinkIfIdle(const QString& link)
{
    if (!m_accountsMode || !m_account || !m_room.isEmpty() || m_masterNotation) {
        return false; // busy with a score: the link gets a Ploq of its own
    }
    m_loginCode = CollabAccount::loginCodeFrom(link);
    m_inviteCode = m_loginCode.isEmpty() ? CollabAccount::inviteCodeFrom(link) : QString();
    if (m_loginCode.isEmpty() && m_inviteCode.isEmpty()) {
        return false;
    }
    LOGI() << "collab: opening the link in this Ploq (no score open)";
    startAccountFlow();
    return true;
}

void CollabSyncService::startAccountFlow()
{
    // Opened from the website ("Open in Ploq"): the link would sign this
    // computer in as the website's user. Ask first - a link someone else sent
    // you would sign you in as them.
    if (!m_loginCode.isEmpty()) {
        m_account->previewLoginCode(m_loginCode, [this](bool ok, const CollabAccount::Preview& pv, const CollabAccount::Error& err) {
            if (!ok) {
                LOGW() << "collab: website link not usable: " << err.text;
                showToast(ToastKind::Error, "Couldn't open the score", err.text.toStdString());
                return;
            }
            QString text = QString("Open “%1”, signed in as %2?").arg(pv.title, who(pv));
            // Compare accounts, not names: anyone can give their account your display name, and a link from
            // them would then switch this computer to their account without a warning.
            const QString mine = QString(m_account->userId()).remove(QRegularExpression("^[up]_")).left(6);
            if (m_account->isSignedIn() && !mine.isEmpty() && mine != pv.accountId) {
                text += QString("\n\nThis computer is signed in as %1 (account %2). Continuing switches it to a "
                                "DIFFERENT account: %3.")
                        .arg(m_account->name(), mine, who(pv));
            }
            text += QString("\n\nOnly continue if you clicked “Open in Ploq” on %1 yourself.").arg(QUrl(m_serverUrl).host());
            if (!askUser("Open shared score", text, "Open")) {
                LOGI() << "collab: website link declined";
                return;
            }
            m_account->redeemLoginCode(m_loginCode, [this](bool ok2, const CollabAccount::Project& p, const CollabAccount::Error& err2) {
                if (!ok2) {
                    LOGW() << "collab: website link not accepted: " << err2.text;
                    showToast(ToastKind::Error, "Couldn't open the score", err2.text.toStdString());
                    return;
                }
                m_name = m_account->name();
                LOGI() << "collab: opening \"" << p.title << "\" from the website";
                joinProject(p);
            });
        });
        return;
    }

    // Opened from an invite link: say whose score it is and ask first.
    if (!m_inviteCode.isEmpty()) {
        m_account->previewInvite(m_inviteCode, [this](bool ok, const CollabAccount::Preview& pv, const CollabAccount::Error& err) {
            if (!ok) {
                showToast(ToastKind::Error, "Couldn't join", err.text.toStdString());
                return;
            }
            const QString text = QString("Join “%1”, shared by %2?\n\nYou'll see each other's edits and cursors live.")
                                 .arg(pv.title, who(pv));
            if (!qaAutoConfirm() && !askUser("Join shared score", text, "Join")) {
                LOGI() << "collab: invite declined";
                m_inviteCode.clear();
                return;
            }
            signInAndContinue();
        });
        return;
    }

    signInAndContinue();
}

void CollabSyncService::signInAndContinue()
{
    if (!m_account->isSignedIn() && m_inviteCode.isEmpty()) {
        // Nothing to do until the person clicks Share or opens a link: no
        // sign-in prompt at startup.
        return;
    }
    // Not signed in: through the browser (no sign-in form in the app).
    m_share->requireSignIn([this]() {
        m_name = m_account->name().isEmpty() ? m_name : m_account->name();

        // Opened from an invite link: join that project's score.
        if (!m_inviteCode.isEmpty()) {
            const QString code = m_inviteCode;
            m_inviteCode.clear();
            m_account->acceptInvite(code, [this](bool ok2, const CollabAccount::Project& p, const CollabAccount::Error& err) {
                if (!ok2) {
                    LOGW() << "collab: invite not accepted: " << err.text;
                    if (err.code == "frozen") {
                        announceFrozen({ QString(), QString(), "editor", QString(), true });
                        return;
                    }
                    showToast(ToastKind::Error, "Couldn't join", err.text.toStdString());
                    return;
                }
                LOGI() << "collab: accepted invite to \"" << p.title << "\"";
                joinProject(p);
            });
            return;
        }
        // Otherwise: rejoin the score that's open, if it was shared before.
        onMasterNotationChanged();
    });
}

void CollabSyncService::onSignedIn()
{
    if (!m_account->name().isEmpty()) {
        m_name = m_account->name();
    }
    LOGI() << "collab: signed in as \"" << m_name << "\"";
}

void CollabSyncService::notify(const QString& text, bool info)
{
    showToast(info ? ToastKind::Info : ToastKind::Error, "Share", text.toStdString());
}

void CollabSyncService::notifySignedOut()
{
    showToast(ToastKind::Info, "Signed out of Ploq", "Click Share to sign in again.");
}

void CollabSyncService::joinProject(const CollabAccount::Project& project)
{
    if (project.room == m_joiningRoom) {
        return; // already asking about this one
    }
    if (project.room == m_room && (m_connected || m_reconnectTimer.isActive()
                                   || m_socket->state() != QAbstractSocket::UnconnectedState)) {
        return;
    }
    if (m_roomRefused && project.projectId == m_projectId) {
        return; // frozen / not a member: reopening the score doesn't change that
    }
    if (!m_accountsMode) {
        connectToProject(project);
        return;
    }
    // Ask first whether the room is open: a frozen score (the owner's plan
    // lapsed) is download-only, so it opens without live editing.
    m_joiningRoom = project.room;
    m_joiningProjectId = project.projectId;
    const int generation = m_roomGeneration;
    m_account->projectInfo(project.projectId, [this, project, generation](bool ok, const CollabAccount::ProjectInfo& info,
                                                                          const CollabAccount::Error& err) {
        if (generation != m_roomGeneration) {
            return; // left meanwhile (the score was closed, or signed out)
        }
        m_joiningRoom.clear();
        m_joiningProjectId.clear();
        if (ok && info.frozen) {
            m_projectId = project.projectId; // for "Manage on web"
            m_roomRefused = true;
            announceFrozen(info);
            return;
        }
        if (!ok && err.status == 401) {
            onSignInRejected(); // connecting would only be refused too
            return;
        }
        if (!ok && err.status == 403) {
            LOGW() << "collab: not opening the shared score: " << err.text;
            showToast(ToastKind::Error, "Couldn't open the shared score", err.text.toStdString());
            return;
        }
        // OK, or the server is unreachable / older (no such call): try the room.
        connectToProject(project);
    });
}

void CollabSyncService::connectToProject(const CollabAccount::Project& project)
{
    if (m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close();
    }
    m_room = project.room;
    m_projectId = project.projectId;
    m_synced = false;
    m_lastSeq = 0;
    m_savedSeq = -1;
    m_roomHasSnapshot = false;
    m_roomRefused = false;
    m_failedConnects = 0;
    // Numbers from another room (or an earlier stay in this one) mean nothing
    // here: our old client ids would hide edits after the copy we download, an
    // old structure number would drop every edit as out of date.
    m_myClientIds.clear();
    m_structureSeq = 0;
    m_structureEditId = -1;
    m_othersInRoom = 0;
    connectToServer();
}

bool CollabSyncService::isRoomScoreOpen() const
{
    if (!m_accountsMode || !m_account || m_room.isEmpty()) {
        return false;
    }
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || project->path().empty()) {
        return false;
    }
    // Sharing, joining and downloading the room's copy all remember the file.
    return m_account->rememberedProject(project->path().toQString()).room == m_room;
}

QString CollabSyncService::joiningProjectId() const
{
    if (!m_joiningProjectId.isEmpty()) {
        return m_joiningProjectId; // asking the server about it
    }
    if (m_waitingForRoomScore && !m_room.isEmpty()) {
        return m_projectId; // in its room, its score still downloading / opening
    }
    return QString();
}

QString CollabSyncService::sharedProjectId() const
{
    if (m_projectId.isEmpty()) {
        return QString();
    }
    if (m_room.isEmpty()) {
        return m_projectId; // frozen when we asked (no room): "Manage on web" still opens it
    }
    return isRoomScoreOpen() ? m_projectId : QString();
}

void CollabSyncService::onRoomRefused()
{
    const QString projectId = m_projectId;
    m_account->projectInfo(projectId, [this, projectId](bool ok, const CollabAccount::ProjectInfo& info,
                                                        const CollabAccount::Error& err) {
        if (projectId != m_projectId) {
            return;
        }
        if (ok && info.frozen) {
            m_roomRefused = true;
            m_reconnectTimer.stop();
            announceFrozen(info);
            return;
        }
        if (!ok && err.status == 401) {
            // Signed out on the website, expired or revoked: every connect is refused.
            onSignInRejected();
            return;
        }
        if (!ok && err.status == 403) {
            // Removed from the score, or it was deleted on the website (both: not a member).
            LOGW() << "collab: the room refused us: " << err.text;
            m_roomRefused = true;
            m_reconnectTimer.stop();
            showToast(ToastKind::Error, "Live editing stopped", err.text.toStdString());
            return;
        }
        if (m_roomRefused) {
            // Closed with 4005 but the server no longer says frozen: try again.
            m_roomRefused = false;
            scheduleReconnect();
        }
    });
}

void CollabSyncService::scheduleReconnect()
{
    // Each try is a request to the server (the hosted one has a daily quota):
    // after failed tries, wait longer - 2 s twice, then doubling, at most a
    // minute. Back to 2 s once a connection gets in (welcome).
    int delay = 2000;
    if (m_accountsMode && m_failedConnects > 1) {
        delay = std::min(60000, 2000 << std::min(m_failedConnects - 2, 5));
    }
    m_reconnectTimer.start(delay);
}

void CollabSyncService::onSignInRejected()
{
    // The server refused our sign-in (401): it was signed out on the website,
    // expired or revoked; CollabAccount has already forgotten it. Stop here:
    // the next Share signs in again and rejoins this score.
    LOGW() << "collab: the server no longer accepts this sign-in; stopped reconnecting";
    leaveRoom();
    m_share->onSignedOutByServer();
    notifySignedOut();
}

void CollabSyncService::announceFrozen(const CollabAccount::ProjectInfo& info)
{
    // Say it once per score and session (the dialog), after that only update it.
    const QString id = info.projectId.isEmpty() ? m_projectId : info.projectId;
    const bool first = id.isEmpty() || id != m_frozenShownFor;
    m_frozenShownFor = id;
    LOGI() << "collab: \"" << info.title << "\" is download-only (the owner's plan lapsed)";
    m_share->onFrozen(info, first);
}

void CollabSyncService::leaveRoom()
{
    const bool wasInRoom = !m_room.isEmpty() || !m_joiningRoom.isEmpty();
    ++m_roomGeneration; // a join still asking the server about its room stops there
    m_joiningRoom.clear();
    m_joiningProjectId.clear();
    m_reconnectTimer.stop();
    m_room.clear();
    m_projectId.clear();
    m_synced = false;
    m_roomRefused = false;
    m_failedConnects = 0;
    m_othersInRoom = 0;
    // Nothing more goes to (or comes from) this room.
    m_outbox.clear();
    m_outboxStructure = false;
    m_ownEdits.clear();
    m_remoteQueue.clear();
    if (m_socket && m_socket->state() != QAbstractSocket::UnconnectedState) {
        m_socket->close();
    }
    m_cursors.clear();
    if (wasInRoom) {
        LOGI() << "collab: left the room";
    }
}

void CollabSyncService::onScoreAboutToClose()
{
    if (!m_accountsMode) {
        return; // local relay: one room per run (COLLAB_ROOM), nothing kept on a server
    }
    if (m_waitingForRoomScore) {
        return; // we're closing it ourselves, to open the room's copy in its place
    }
    if (isRoomScoreOpen()) {
        saveRoomScoreIfLast();
        LOGI() << "collab: closed the shared score";
    }
    // Closing a score always leaves its room: the next score opened (or Home)
    // must not send its edits there, receive the room's, or be saved as it.
    leaveRoom();
}

void CollabSyncService::saveRoomScoreIfLast()
{
    // The room's edits live in the server's log; its saved copy (what the
    // website downloads) is only refreshed by a full score. If we're the last
    // one here, nobody else will send one: send ours before it's gone.
    if (!m_serverKeepsScores || !m_connected || !m_synced || m_othersInRoom > 0) {
        return;
    }
    // Our edits the server hasn't numbered yet aren't counted in m_lastSeq.
    const bool unacked = std::any_of(m_ownEdits.begin(), m_ownEdits.end(), [](const OwnEdit& e) { return e.seq < 0; });
    if (m_lastSeq <= m_savedSeq && m_outbox.empty() && !m_outboxStructure && !unacked) {
        return; // the saved copy already has everything
    }
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || project->path().empty()) {
        return;
    }
    LOGI() << "collab: saving the room's score on the server before closing (edit #" << m_lastSeq
           << ", saved copy has #" << m_savedSeq << ")";
    // Wait for the server's answer: on quit the socket closes right after this.
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    bool answered = false;
    m_onSnapshotReply = [&]() { answered = true; loop.quit(); };
    // Gone meanwhile: no answer will come.
    const QMetaObject::Connection gone = QObject::connect(m_socket.get(), &QWebSocket::disconnected, &loop, &QEventLoop::quit);
    uploadSnapshot(m_lastSeq);
    timeout.start(3000);
    if (m_connected) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    QObject::disconnect(gone);
    m_onSnapshotReply = nullptr;
    if (!answered) {
        LOGW() << "collab: no answer from the server within 3 s; its copy may miss the latest edits";
    }
}

void CollabSyncService::installInviteShortcut()
{
    // The main window may not exist yet this early in startup: retry.
    QWindow* window = mainWindow() ? mainWindow()->qWindow() : nullptr;
    if (!window) {
        QTimer::singleShot(1000, [this]() { installInviteShortcut(); });
        return;
    }
    // Ctrl+Alt+I ("invite"): free in MuseScore's default shortcuts (Ctrl+Shift+I is re-pitch).
    m_inviteShortcut = std::make_unique<QShortcut>(QKeySequence("Ctrl+Alt+I"), window);
    m_inviteShortcut->setContext(Qt::ApplicationShortcut);
    QObject::connect(m_inviteShortcut.get(), &QShortcut::activated, [this]() { m_share->share(); });
}

void CollabSyncService::deinit()
{
    if (s_activeService == this) {
        s_activeService = nullptr;
    }
    m_reconnectTimer.stop();
    m_retryApplyTimer.stop();
    if (m_socket) {
        m_socket->disconnect();
        m_socket->close();
    }
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

void CollabSyncService::connectToServer()
{
    if (m_room.isEmpty() || m_roomRefused) {
        return; // accounts mode: not in a project (yet), or the room is closed to us
    }
    if (m_accountsMode) {
        if (!m_account->isSignedIn()) {
            onSignInRejected(); // forgotten after a 401 elsewhere: the room would refuse us
            return;
        }
        // Rooms are members-only: send our sign-in token.
        QNetworkRequest req(QUrl(m_wsBase + "/room/" + m_room));
        req.setRawHeader("Authorization", "Bearer " + m_account->token().toUtf8());
        LOGI() << "collab: connecting to " << req.url().toString();
        m_socket->open(req);
        return;
    }
    QString url = m_serverUrl;
    if (!url.endsWith('/')) {
        url += '/';
    }
    url += QUrl::toPercentEncoding(m_room);
    LOGI() << "collab: connecting to " << url;
    m_socket->open(QUrl(url));
}

//! QA only: COLLAB_AUTOBOT=<script.js> runs that autobot script (api.dispatcher,
//! api.navigation, ...) inside this GUI instance once it has joined the room,
//! so two-instance tests can drive real local edits without simulated OS input.
//! (--test-case can't: it switches the app to console mode, where collab is off.)
//! Development builds only: a release build never runs a script named in its environment.
static QString qaScriptPath()
{
#ifdef MUSE_APP_UNSTABLE
    return qEnvironmentVariable("COLLAB_AUTOBOT");
#else
    return QString();
#endif
}

void CollabSyncService::startQaScriptOnce()
{
    const QString script = qaScriptPath();
    if (script.isEmpty() || m_qaScriptStarted) {
        return;
    }
    // Only once we're caught up with the room and its score is the one open
    // (joining a project downloads and opens the room's copy first).
    if (!m_caughtUp || m_waitingForRoomScore || !globalContext()->currentProject()) {
        return; // called again from the welcome / "synced" / score-opened paths
    }
    m_qaScriptStarted = true;
#ifdef MUSE_MODULE_AUTOBOT
    QTimer::singleShot(1500, [this, script]() {
        if (m_waitingForRoomScore) {
            m_qaScriptStarted = false; // a reload started meanwhile: wait for it
            QTimer::singleShot(500, [this]() { startQaScriptOnce(); });
            return;
        }
        if (!autobot()) {
            LOGE() << "collab: COLLAB_AUTOBOT set but the autobot service isn't available";
            return;
        }
        LOGI() << "collab: QA script " << script;
        autobot()->execScript(muse::io::path_t(script));
    });
#else
    LOGE() << "collab: COLLAB_AUTOBOT set but this build has no autobot module";
#endif
}

void CollabSyncService::onConnected()
{
    m_connected = true;
    m_welcomedThisConnection = false;
    m_caughtUp = false; // until the welcome (local relay) or "synced" (server)
    m_streamSeq = -1;   // known again once we ask to catch up (onWelcomeSync)
    m_gapResyncs = 0;
    QJsonObject join {
        { "type", "join" },
        { "name", m_name },
        { "color", m_color },
    };
    m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(join).toJson(QJsonDocument::Compact)));
    LOGI() << "collab: connected as \"" << m_name << "\" in room \"" << m_room << "\"";
    m_cursors.resendSelection(); // let the room see what we already have selected
    sendSelection();
}

void CollabSyncService::onTextMessage(const QString& text)
{
    const QJsonObject msg = QJsonDocument::fromJson(text.toUtf8()).object();
    const QString type = msg.value("type").toString();

    if (type == "edit") {
        if (!checkSeq(msg.value("seq").toInt())) {
            return; // an earlier edit never came: starting again from the room's score, which has this one
        }
        // When catching up after a reconnect, the server replays edits that
        // include our own: we already have those.
        if (m_myClientIds.count(msg.value("from").toInt(-1))) {
            m_lastSeq = std::max(m_lastSeq, msg.value("seq").toInt());
            return;
        }
        // Incoming edits come from other people: bound what we accept.
        static constexpr qsizetype MAX_EDIT_XML_BYTES = 8 * 1024 * 1024;
        static constexpr size_t MAX_QUEUED_EDITS = 2000; // a late joiner may replay a few hundred
        if (text.size() > MAX_EDIT_XML_BYTES) {
            LOGW() << "collab: dropped oversized edit (" << text.size() << " chars)";
            return;
        }
        if (m_remoteQueue.size() >= MAX_QUEUED_EDITS) {
            LOGW() << "collab: too many pending edits, dropping incoming edit";
            return;
        }

        RemoteEdit edit;
        edit.seq = msg.value("seq").toInt();
        edit.from = CollabAccount::cleanText(msg.value("name").toString(), 40); // shown in toasts and logs
        edit.range.tickFrom = msg.value("tickFrom").toInt(-1);
        edit.range.tickTo = msg.value("tickTo").toInt(-1);
        edit.range.staffFrom = static_cast<staff_idx_t>(msg.value("staffFrom").toInt());
        edit.range.staffTo = static_cast<staff_idx_t>(msg.value("staffTo").toInt());
        edit.xml = msg.value("xml").toString().toUtf8();
        edit.sentAtMs = static_cast<qint64>(msg.value("sentAtMs").toDouble());
        edit.structSeq = msg.value("structSeq").toInt(-1);
        edit.baseSeq = msg.value("baseSeq").toInt(-1);
        m_remoteQueue.push_back(std::move(edit));
        processRemoteQueue();
    } else if (type == "selection") {
        m_cursors.onRemoteSelection(msg);
    } else if (type == "cursor") {
        m_cursors.onRemoteCursor(msg);
    } else if (type == "welcome") {
        // Exactly one welcome per connection. Another one is forged (an old
        // relay forwarded any message type) and would give us someone else's id.
        if (m_welcomedThisConnection) {
            LOGW() << "collab: ignoring a second welcome on this connection";
            return;
        }
        m_welcomedThisConnection = true;
        // Old ids identify our own edits when the server replays its log after a
        // reconnect. A server without a log (the local relay), or one whose edit
        // count went backwards (restarted / reset), hands ids out from 1 again:
        // keeping old ones made us drop a peer's edits as our own.
        const int roomSeq = msg.value("seq").toInt();
        if (!msg.value("snapshot").isObject() || roomSeq < m_lastSeq) {
            m_myClientIds.clear();
        }
        m_myClientId = msg.value("clientId").toInt(-1);
        m_myClientIds.insert(m_myClientId);
        LOGI() << "collab: welcome, client id " << m_myClientId
               << ", room edit count " << msg.value("seq").toInt();
        m_cursors.onPresence(msg.value("peers").toArray());
        countOthers(msg.value("peers").toArray());
        if (m_share) {
            m_share->onPresence(msg.value("peers").toArray());
        }
        m_failedConnects = 0;
        onWelcomeSync(msg);
        startQaScriptOnce();
    } else if (type == "snapshot") {
        if (msg.value("reload").toBool()) {
            // Someone changed the score's structure: reload the room's score.
            // Queued edits older than it are already inside it.
            const int baseSeq = msg.value("baseSeq").toInt();
            m_structureSeq = std::max(m_structureSeq, baseSeq);
            m_remoteQueue.erase(std::remove_if(m_remoteQueue.begin(), m_remoteQueue.end(),
                                               [baseSeq](const RemoteEdit& e) { return e.seq <= baseSeq; }),
                                m_remoteQueue.end());
            // The whole score as of edit #baseSeq: whatever came before it, nothing is missing.
            if (m_streamSeq >= 0) {
                m_streamSeq = std::max(m_streamSeq, baseSeq);
            }
            m_waitingForRoomScore = true;
            LOGI() << "collab: " << CollabAccount::cleanText(msg.value("name").toString(), 40) << " changed the score's structure; reloading it";
            if (toastService()) {
                showToast(ToastKind::Info, "Score updated", (CollabAccount::cleanText(msg.value("name").toString(), 40)
                                                             + " changed instruments, measures or signatures. Reloaded.").toStdString());
            }
        } else {
            // Catching up from scratch: the room's score as of edit #baseSeq, then the edits after it.
            // Queued edits up to it are inside it; applied on top they could undo later ones.
            const int baseSeq = msg.value("baseSeq").toInt();
            m_remoteQueue.erase(std::remove_if(m_remoteQueue.begin(), m_remoteQueue.end(),
                                               [baseSeq](const RemoteEdit& e) { return e.seq <= baseSeq; }),
                                m_remoteQueue.end());
            m_streamSeq = baseSeq;
        }
        openRoomScore(msg);
    } else if (type == "no-snapshot") {
        // We asked to catch up but the room has no score yet: ours becomes it.
        m_waitingForRoomScore = false;
        m_synced = true;
        m_caughtUp = true;
        m_outbox.clear(); // the score we upload already has them
        m_outboxStructure = false;
        m_lastSeq = msg.value("seq").toInt();
        m_streamSeq = m_lastSeq;
        uploadSnapshot(m_lastSeq);
        startQaScriptOnce();
    } else if (type == "synced") {
        // The room is at edit #seq: one we haven't had by now never arrived.
        const int roomSeq = msg.value("seq").toInt();
        if (m_serverKeepsScores && m_streamSeq >= 0 && roomSeq > m_streamSeq) {
            const bool resyncing = m_gapResyncs == 0;
            onSeqGap(m_streamSeq + 1, roomSeq);
            if (resyncing) {
                return; // a "synced" follows the score that comes back
            }
            m_streamSeq = roomSeq;
        }
        m_synced = true;
        m_caughtUp = true;
        LOGI() << "collab: caught up with the room (edit #" << msg.value("seq").toInt() << ")";
        flushOutbox(); // after the replayed edits are applied (processRemoteQueue flushes then)
        startQaScriptOnce();
    } else if (type == "snapshot-request") {
        // The server's edit log is getting long: give it a fresh full score.
        uploadSnapshot(m_remoteQueue.empty() ? m_lastSeq : std::min(m_lastSeq, m_remoteQueue.front().seq - 1));
    } else if (type == "ack") {
        checkSeq(msg.value("seq").toInt()); // our own edit still counts as acknowledged if it finds a gap
        m_lastSeq = std::max(m_lastSeq, msg.value("seq").toInt());
        onOwnEditAcked(msg.value("editId").toInt(-1), msg.value("seq").toInt());
        if (m_structureEditId >= 0 && msg.value("editId").toInt(-1) == m_structureEditId) {
            // Our structural change is now room edit #seq: our edits since then
            // (held in the outbox) go out marked with it.
            m_structureEditId = -1;
            m_structureSeq = msg.value("seq").toInt();
            flushOutbox();
        }
    } else if (type == "snapshot-saved") {
        LOGI() << "collab: room score saved on the server (as of edit #" << msg.value("baseSeq").toInt() << ")";
        m_savedSeq = std::max(m_savedSeq, msg.value("baseSeq").toInt(-1));
        if (m_share) {
            m_share->onRoomScoreSaved();
        }
        if (m_onSnapshotReply) {
            m_onSnapshotReply();
        }
    } else if (type == "snapshot-refused") {
        LOGW() << "collab: the server didn't take our copy of the score: " << msg.value("reason").toString();
        if (m_onSnapshotReply) {
            m_onSnapshotReply();
        }
    } else if (type == "presence") {
        LOGI() << "collab: " << msg.value("peers").toArray().size() << " people in room";
        m_cursors.onPresence(msg.value("peers").toArray());
        countOthers(msg.value("peers").toArray());
        if (m_share) {
            m_share->onPresence(msg.value("peers").toArray());
        }
        flushOutbox(); // local relay: held edits wait for someone to send them to
    } else if (type == "edit-rejected") {
        // The server refused it (its edit log is full until a fresh score
        // arrives; a server from before 2026-11-25 also refused edits inside
        // someone's lock). Others never got it; our copy keeps it until the next sync.
        const int editId = msg.value("editId").toInt(-1);
        // "slow down": the server's rate limit (e.g. a burst of held edits after being offline). Nothing was
        // stored, so send those measures again shortly, as they are then, instead of keeping them only here.
        if (msg.value("reason").toString() == "slow down") {
            for (const OwnEdit& e : m_ownEdits) {
                if (e.editId == editId) {
                    addToOutbox(e.range);
                    // After the limit has refilled (2 s), not the usual 100 ms, so it doesn't just bounce again.
                    QTimer::singleShot(2000, &m_flushTimer, [this]() { flushOutbox(); });
                    break;
                }
            }
        } else if (msg.value("reason").toString() == "too big") {
            // Too big for the server's edit log (sendMeasureEdit avoids this; a peer's limit could be lower):
            // the whole score carries it instead, so the others don't miss it.
            sendStructureChange();
        }
        m_ownEdits.erase(std::remove_if(m_ownEdits.begin(), m_ownEdits.end(),
                                        [editId](const OwnEdit& e) { return e.editId == editId; }),
                         m_ownEdits.end());
        LOGW() << "collab: server rejected edit " << editId << ": "
               << (msg.contains("by") ? ("measures locked by " + msg.value("by").toString())
                   : msg.value("reason").toString());
    }
    // "locks" / "lock-denied" (servers from before 2026-11-25): there are no locks any more.
}

// ---------------------------------------------------------------------------
// Joining late / reconnecting (Phase 6)
// ---------------------------------------------------------------------------

void CollabSyncService::sendJson(const QJsonObject& msg)
{
    if (m_connected) {
        m_socket->sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
    }
}

void CollabSyncService::onWelcomeSync(const QJsonObject& welcome)
{
    const QJsonValue snap = welcome.value("snapshot");
    m_serverKeepsScores = snap.isObject();
    if (!m_serverKeepsScores) {
        m_synced = true; // local relay (Phase 3): everyone starts from the same file
        m_caughtUp = true; // it keeps no log: nothing to catch up on
        flushOutbox();
        return;
    }
    m_roomHasSnapshot = snap.toObject().value("exists").toBool();
    m_savedSeq = m_roomHasSnapshot ? snap.toObject().value("baseSeq").toInt(-1) : -1;
    const int roomSeq = welcome.value("seq").toInt();

    if (m_synced) {
        // Reconnecting: only what we missed.
        m_streamSeq = m_lastSeq;
        sendJson(QJsonObject { { "type", "sync" }, { "sinceSeq", m_lastSeq } });
    } else if (m_roomHasSnapshot) {
        // Joining a room that already has a score: get it, then the edits since.
        LOGI() << "collab: room already has a score, downloading it";
        m_waitingForRoomScore = true;
        sendJson(QJsonObject { { "type", "sync" }, { "sinceSeq", -1 } });
    } else {
        // Empty room: our score becomes the room's score (with any offline edits in it).
        m_synced = true;
        m_caughtUp = true;
        m_outbox.clear();
        m_outboxStructure = false;
        m_lastSeq = roomSeq;
        m_streamSeq = roomSeq;
        uploadSnapshot(roomSeq);
    }
}

//! The server numbers every edit it keeps (and every structural change) one after another, and a
//! connection sees each number once, in order: as an edit, or as the ack of its own. A number
//! skipped is an edit we will never get. Returns false when it started a full re-sync for that.
bool CollabSyncService::checkSeq(int seq)
{
    if (!m_serverKeepsScores || m_streamSeq < 0 || seq <= m_streamSeq) {
        return true; // local relay (no log), not caught up yet, or replayed again
    }
    if (seq == m_streamSeq + 1) {
        m_streamSeq = seq;
        return true;
    }
    const bool resyncing = m_gapResyncs == 0;
    onSeqGap(m_streamSeq + 1, seq - 1);
    if (!resyncing) {
        m_streamSeq = seq; // gave up re-syncing on this connection: keep watching from here
    }
    return !resyncing;
}

//! Edits #from..#to never arrived (a server from before 2026-11-27 numbered edits it then didn't keep).
//! Start again from the room's saved score, once per connection, so a server that keeps doing it
//! can't make us reload forever.
void CollabSyncService::onSeqGap(int from, int to)
{
    LOGW() << "collab: room edits #" << from << "-#" << to << " never arrived";
    if (m_gapResyncs++ > 0) {
        return;
    }
    LOGI() << "collab: downloading the room's score again so ours matches everyone's";
    m_streamSeq = -1; // known again from the score that comes back
    m_caughtUp = false; // our changes wait in the outbox until then
    m_waitingForRoomScore = true;
    sendJson(QJsonObject { { "type", "sync" }, { "sinceSeq", -1 } });
}

void CollabSyncService::uploadSnapshot(int baseSeq)
{
    if (!m_serverKeepsScores || !m_connected) {
        return;
    }
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || project->path().empty()) {
        LOGI() << "collab: no saved score open yet; will share it once one is opened";
        return;
    }
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const muse::Ret ret = project->writeToDevice(&buffer);
    if (!ret) {
        LOGW() << "collab: could not save the score for the room: " << ret.toString();
        return;
    }
    const QByteArray mscz = buffer.data();
    sendJson(QJsonObject {
        { "type", "snapshot-put" },
        { "baseSeq", baseSeq },
        { "title", project->displayName() },
        { "data", QString::fromLatin1(mscz.toBase64()) },
    });
    m_roomHasSnapshot = true;
    LOGI() << "collab: shared the score with the room (" << mscz.size() << " bytes, as of edit #" << baseSeq << ")";
}

void CollabSyncService::sendStructureChange()
{
    if (!m_serverKeepsScores) {
        LOGW() << "collab: instrument/measure/signature changes only sync through the online server";
        showToast(ToastKind::Warning, "Not shared",
                  "Changes to instruments, measures or signatures only sync through the online server.");
        return;
    }
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (!project || project->path().empty()) {
        return;
    }
    QBuffer buffer;
    buffer.open(QIODevice::WriteOnly);
    const muse::Ret ret = project->writeToDevice(&buffer);
    if (!ret) {
        LOGW() << "collab: could not save the score to share a structural change: " << ret.toString();
        return;
    }
    m_structureEditId = m_nextEditId;
    sendJson(QJsonObject {
        { "type", "structure" },
        { "editId", m_nextEditId++ },
        { "title", project->displayName() },
        { "data", QString::fromLatin1(buffer.data().toBase64()) },
    });
    LOGI() << "collab: sent a structural change (whole score, " << buffer.data().size() << " bytes, "
           << (m_masterNotation ? m_masterNotation->masterScore()->nmeasures() : 0) << " measures)";
}

//! A score from the room (another member's upload) opens without asking: look inside it first.
//! Returns why it won't be opened, or an empty string. The zip reader also stops any entry that
//! expands past its limits while the score opens, whatever the entry claims here.
static QString roomScoreProblem(const QByteArray& mscz)
{
    static constexpr size_t MAX_ENTRIES = 1000;                       // a score has a handful
    static constexpr uint64_t MAX_ENTRY_BYTES = 256ULL * 1024 * 1024;   // as in the zip reader
    static constexpr uint64_t MAX_TOTAL_BYTES = 512ULL * 1024 * 1024;
    muse::io::Buffer buffer(reinterpret_cast<const uint8_t*>(mscz.constData()), static_cast<size_t>(mscz.size()));
    muse::ZipReader zip(&buffer);
    const std::vector<muse::ZipReader::FileInfo> files = zip.fileInfoList();
    if (files.empty() || files.size() > MAX_ENTRIES) {
        return QString("%1 files inside").arg(files.size());
    }
    uint64_t total = 0;
    bool hasScore = false;
    for (const muse::ZipReader::FileInfo& f : files) {
        if (f.size > MAX_ENTRY_BYTES) {
            return f.filePath.toQString() + " too big";
        }
        total += f.size;
        hasScore = hasScore || (!f.isDir && f.filePath.toQString().endsWith(".mscx", Qt::CaseInsensitive));
    }
    if (total > MAX_TOTAL_BYTES) {
        return "too big when unpacked";
    }
    return hasScore ? QString() : QString("no score inside");
}

static QString safeFileNamePart(const QString& s)
{
    QString out;
    for (const QChar c : s) {
        out += (c.isLetterOrNumber() || c == '-' || c == '_') ? c : QChar('_');
    }
    return out.left(64);
}

void CollabSyncService::openRoomScore(const QJsonObject& msg)
{
    static constexpr qsizetype MAX_SCORE_BYTES = 24 * 1024 * 1024;
    const QByteArray mscz = QByteArray::fromBase64(msg.value("data").toString().toLatin1());
    if (mscz.isEmpty() || mscz.size() > MAX_SCORE_BYTES || !mscz.startsWith("PK")) {
        LOGW() << "collab: room score missing or not an .mscz file";
        m_waitingForRoomScore = false;
        return;
    }
    if (const QString problem = roomScoreProblem(mscz); !problem.isEmpty()) {
        LOGW() << "collab: not opening the room's score: " << problem;
        m_waitingForRoomScore = false;
        showToast(ToastKind::Warning, "Score not opened",
                  "The shared score from this room doesn't look like a MuseScore file, so it wasn't opened.");
        return;
    }

    // Each person keeps their own copy of the room's score in the app's data
    // folder: rooms/<room>-<person>/<score title>.mscz. Named after the score so
    // the window title reads e.g. "Amazing Grace" (MuseScore titles the window
    // from the file name when opening into the current window).
    QString title = msg.value("title").toString().trimmed();
    if (title.isEmpty()) {
        title = "Shared score";
    }
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/collab/rooms/"
                        + safeFileNamePart(m_room) + "-" + safeFileNamePart(m_name);
    QDir().mkpath(dir);
    QString fileTitle = title;
    for (const QChar bad : QString("\\/:*?\"<>|")) {
        fileTitle.replace(bad, QChar('_'));
    }
    const QString path = dir + "/" + fileTitle.left(100) + ".mscz";

    m_roomFilePath = path;
    m_roomFileData = mscz; // written in openRoomFile, after the open score is closed
    m_roomFileTitle = title;
    m_waitingForRoomScore = true; // also for a catch-up snapshot (log compacted while we were away)
    m_lastSeq = msg.value("baseSeq").toInt();
    if (m_account && !m_projectId.isEmpty()) {
        // Reopening this copy later rejoins the project.
        m_account->rememberProject(path, { m_projectId, m_room, title });
    }
    LOGI() << "collab: opening the room's score \"" << title << "\" (" << mscz.size() << " bytes)";
    openRoomFile(0);
}

void CollabSyncService::openRoomFile(int attempt)
{
    if (!m_waitingForRoomScore || m_roomFilePath.isEmpty()) {
        return;
    }
    const auto isRoomFileOpen = [this]() {
        const project::INotationProjectPtr p = globalContext()->currentProject();
        return p && p->path().toQString() == m_roomFilePath;
    };
    const auto opened = [this]() {
        if (m_masterNotation) {
            LOGI() << "collab: room's score open (" << m_masterNotation->masterScore()->nmeasures() << " measures)";
        }
        // onMasterNotationChanged normally got here first; covers the case it didn't.
        m_waitingForRoomScore = false;
        processRemoteQueue();
        startQaScriptOnce();
    };
    // Done already - unless there's a new copy still to write: a reload has the
    // same path as the score that's open, and that one must be reopened.
    if (m_roomFileData.isEmpty() && isRoomFileOpen()) {
        opened();
        return;
    }
    static constexpr int MAX_ATTEMPTS = 40; // 20 s

    // Still starting up (e.g. opening the score the app was started with):
    // closing that half-opened score crashes, so wait until startup is done.
    if (startupScenario() && !startupScenario()->startupCompleted() && attempt + 1 < MAX_ATTEMPTS) {
        QTimer::singleShot(500, [this, attempt]() { openRoomFile(attempt + 1); });
        return;
    }

    const project::INotationProjectPtr current = globalContext()->currentProject();
    if (current && current->needSave().val && !current->path().empty()) {
        // Save quietly so closing doesn't stop to ask "save changes?" - the
        // shared copy we're about to open already contains everything.
        current->save();
    }
    if (current && !projectFilesController()->closeOpenedProject(false)) {
        LOGW() << "collab: kept the open score (closing it was cancelled); not joining the room's score";
        m_waitingForRoomScore = false;
        m_remoteQueue.clear();
        return;
    }

    // Only now: closing saved the open score, which may be this same file
    // (a reload) - writing first would let that save overwrite the room's copy.
    if (!m_roomFileData.isEmpty()) {
        QFile file(m_roomFilePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(m_roomFileData) != m_roomFileData.size()) {
            LOGW() << "collab: could not write " << m_roomFilePath;
            m_waitingForRoomScore = false;
            m_remoteQueue.clear();
            return;
        }
        file.close();
        m_roomFileData.clear();
    }

    const muse::Ret ret = projectFilesController()->openProject(
        project::ProjectFile(QUrl::fromLocalFile(m_roomFilePath), m_roomFileTitle));
    if (isRoomFileOpen()) {
        opened();
        return;
    }
    // Usually "busy": another score was still opening (e.g. the one the app was
    // started with), and MuseScore refuses a second open meanwhile. Without a
    // retry we'd wait for this score forever: no edits in or out.
    if (attempt + 1 < MAX_ATTEMPTS) {
        LOGI() << "collab: couldn't open the room's score yet (" << ret.toString() << "), retrying";
        QTimer::singleShot(500, [this, attempt]() { openRoomFile(attempt + 1); });
        return;
    }
    LOGW() << "collab: couldn't open the room's score: " << ret.toString();
    m_waitingForRoomScore = false;
    m_remoteQueue.clear();
    showToast(ToastKind::Error, "Couldn't open the shared score",
              "Close and reopen the invite link or the score to try again.");
}

// ---------------------------------------------------------------------------
// Last write wins (measure locks until 2026-11-25)
//
// Anyone can edit any measure. The server numbers every edit (seq) and
// everyone applies them in that order, so when two people change the same
// measure at once, the later-numbered one wins on every copy. Our own edits
// are already in our score, though, while the server may number them after
// a collaborator's edit we haven't applied yet. So when we apply that edit,
// ours that come after it go back on top (applyRemoteEdit).
// ---------------------------------------------------------------------------

static bool rangesOverlap(const MeasureRange& a, const MeasureRange& b)
{
    return a.tickFrom < b.tickTo && b.tickFrom < a.tickTo && a.staffFrom < b.staffTo && b.staffFrom < a.staffTo;
}

//! The newest room edit our score contains (edits still queued aren't in it yet).
int CollabSyncService::seenSeq() const
{
    return m_remoteQueue.empty() ? m_lastSeq : std::min(m_lastSeq, m_remoteQueue.front().seq - 1);
}

void CollabSyncService::onOwnEditAcked(int editId, int seq)
{
    for (OwnEdit& e : m_ownEdits) {
        if (e.editId == editId) {
            e.seq = seq;
            e.ackedAtMs = nowMs();
            break;
        }
    }
    pruneOwnEdits();
}

void CollabSyncService::pruneOwnEdits()
{
    // An acknowledged edit can be forgotten once no collaborator's edit still
    // to be applied comes before it (the server sends those before our ack)...
    // but it's kept a few seconds more to tell us if someone overwrote it.
    static constexpr qint64 KEEP_MS = 15000;
    static constexpr size_t MAX_KEPT = 64;
    const qint64 now = nowMs();
    const int firstQueued = m_remoteQueue.empty() ? INT_MAX : m_remoteQueue.front().seq;
    while (!m_ownEdits.empty()) {
        const OwnEdit& e = m_ownEdits.front();
        const bool settled = e.seq >= 0 && e.seq < firstQueued;
        if (!settled || (now - e.ackedAtMs < KEEP_MS && m_ownEdits.size() <= MAX_KEPT)) {
            break;
        }
        m_ownEdits.pop_front();
    }
}

void CollabSyncService::notifyReplaced(const RemoteEdit& edit, const OwnEdit& mine)
{
    const int tick = std::max(edit.range.tickFrom, mine.range.tickFrom);
    int measureNo = 0;
    if (const Measure* m = m_masterNotation ? m_masterNotation->masterScore()->tick2measure(Fraction::fromTicks(tick)) : nullptr) {
        measureNo = m->no() + 1;
    }
    const QString who = edit.from.isEmpty() ? QString("Someone") : edit.from;
    LOGI() << "collab: edit #" << edit.seq << " from " << who << " replaced our edit #" << mine.seq
           << " (measure " << measureNo << "; they hadn't seen it)";

    // At most one notice every 3 s, so a burst of edits doesn't stack them.
    const qint64 now = nowMs();
    if (now - m_lastReplacedToastMs < 3000) {
        return;
    }
    m_lastReplacedToastMs = now;
    const QString text = measureNo > 0
                         ? QString("%1 changed measure %2 at the same time as you. Their version was kept.").arg(who).arg(measureNo)
                         : QString("%1 changed the same measures at the same time as you. Their version was kept.").arg(who);
    showToast(ToastKind::Info, "Edited at the same time", text.toStdString());
}

// ---------------------------------------------------------------------------
// Outgoing: local edits
// ---------------------------------------------------------------------------

void CollabSyncService::onMasterNotationChanged()
{
    if (m_notation) {
        m_notation->undoStack()->changesChannel().disconnect(this);
        m_notation->interaction()->selectionChanged().disconnect(this);
    }
    if (!m_waitingForRoomScore) {
        m_remoteQueue.clear(); // edits for a score that's no longer open
    }
    dropOutbox(m_waitingForRoomScore ? "the room's score replaced yours" : "the score was closed or replaced");

    m_masterNotation = globalContext()->currentMasterNotation();
    m_notation = m_masterNotation ? m_masterNotation->notation() : nullptr;
    m_cursors.setScore(m_masterNotation ? m_masterNotation->masterScore() : nullptr);
    m_structure = scoreStructure(m_masterNotation ? m_masterNotation->masterScore() : nullptr);
    m_ownEdits.clear(); // a different score (or the room's copy of it) now

    if (m_notation) {
        m_notation->undoStack()->changesChannel().onReceive(this, [this](const notation::ScoreChanges& changes) {
            onLocalChanges(changes);
            sendSelection(); // layout may have moved what we have selected
        });
        m_notation->interaction()->selectionChanged().onNotify(this, [this]() {
            sendSelection();
        });
    }

    // Phase 6: the room's score just finished opening → apply the edits made since.
    const project::INotationProjectPtr project = globalContext()->currentProject();
    if (m_waitingForRoomScore && project && project->path().toQString() == m_roomFilePath) {
        m_waitingForRoomScore = false;
        processRemoteQueue();
        startQaScriptOnce();
    }
    startQaScriptOnce(); // QA runs: the welcome can come before the score has opened

    // Joined an empty room with no score open, and now opened one: share it.
    if (m_connected && m_serverKeepsScores && !m_roomHasSnapshot && m_synced && project) {
        uploadSnapshot(m_lastSeq);
    }

    // A test asked to share the score it opened, but isn't signed in: Share
    // signs in through the browser first (once).
    if (m_accountsMode && m_account && !m_account->isSignedIn() && m_hostRequested && !m_hostShareStarted
        && project && !project->path().empty()) {
        m_hostShareStarted = true;
        m_share->share(m_hostWithDialog);
    }

    // Phase 7: a score opened while signed in and not in a project yet.
    if (m_accountsMode && m_account && m_account->isSignedIn() && m_room.isEmpty() && !m_waitingForRoomScore
        && project && !project->path().empty()) {
        const CollabAccount::Project known = m_account->rememberedProject(project->path().toQString());
        if (!known.room.isEmpty() && !(m_roomRefused && known.projectId == m_projectId)) {
            LOGI() << "collab: rejoining \"" << known.title << "\" (shared before)";
            joinProject(known); // a score we shared or joined before
        } else if (m_hostRequested) {
            if (!m_hostShareStarted) {
                m_hostShareStarted = true;
                m_share->share(m_hostWithDialog); // launched to host (QA): share it and copy an invite link
            }
        }
    }
}

void CollabSyncService::sendSelection()
{
    if (!m_connected || !m_masterNotation) {
        return;
    }
    // While a collaborator's edit is being swapped in, pasteStaff briefly
    // selects the pasted measures: sending that would flash a bogus highlight
    // and cost 2 messages per received edit. applyRemoteEdit puts the real
    // selection back and notifies afterwards, which sends it if it changed.
    if (m_applyingRemote) {
        return;
    }
    m_cursors.sendSelection(selectionOutlines(m_masterNotation->masterScore()));
}

void CollabSyncService::onLocalChanges(const notation::ScoreChanges& changes)
{
    // Track the score's structure (instruments, measures, time/key signatures).
    Score* score = m_masterNotation ? m_masterNotation->masterScore() : nullptr;
    const QByteArray prevStructure = m_structure;
    m_structure = scoreStructure(score);

    // Our own swap-in of a remote edit also lands here: don't send that.
    if (m_applyingRemote || m_waitingForRoomScore || !m_masterNotation) {
        return;
    }
    const bool structureChanged = !prevStructure.isEmpty() && m_structure != prevStructure;

    // Can't send yet (offline, still catching up after a reconnect, or our
    // structural change has no number yet): remember the measures;
    // flushOutbox() sends them once we can.
    if (!m_connected || !m_caughtUp || m_structureEditId >= 0) {
        if (structureChanged) {
            m_outboxStructure = true; // measure positions may have moved: the whole score goes
        } else if (const MeasureRange range = measureRangeForChanges(score, changes); range.isValid()) {
            addToOutbox(range);
        }
        return;
    }

    // A structural change can't travel as a measure paste: send the whole score
    // (which also carries anything still in the outbox).
    if (structureChanged) {
        m_outboxStructure = true;
        flushOutbox();
        return;
    }

    const MeasureRange range = measureRangeForChanges(score, changes);
    if (!range.isValid()) {
        return;
    }

    if (!m_outbox.empty()) {
        addToOutbox(range); // older unsent edits go first
        flushOutbox();
        return;
    }
    sendMeasureEdit(score, range);
}

bool CollabSyncService::sendMeasureEdit(Score* score, const MeasureRange& range)
{
    const QByteArray xml = writeMeasures(score, range);
    if (xml.isEmpty()) {
        LOGW() << "collab: could not write measures " << range.tickFrom << "-" << range.tickTo;
        return false;
    }

    QJsonObject msg {
        { "type", "edit" },
        { "editId", m_nextEditId++ },
        { "tickFrom", range.tickFrom },
        { "tickTo", range.tickTo },
        { "staffFrom", static_cast<int>(range.staffFrom) },
        { "staffTo", static_cast<int>(range.staffTo) },
        { "sentAtMs", static_cast<double>(nowMs()) },
        { "structSeq", m_structureSeq },
        { "baseSeq", seenSeq() }, // lets whoever we overwrite tell that we hadn't seen theirs
        { "xml", QString::fromUtf8(xml) },
    };
    const QString text = QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact));
    // The server keeps each edit in one storage value (under 1.8 million characters, with what it adds)
    // and refuses bigger ones. A change this big goes as the whole score instead.
    static constexpr qsizetype MAX_EDIT_CHARS = 1'700'000;
    if (m_serverKeepsScores && text.size() > MAX_EDIT_CHARS) {
        LOGI() << "collab: measures " << range.tickFrom << "-" << range.tickTo << " too big for one edit ("
               << text.size() << " chars); sending the whole score";
        sendStructureChange();
        return true;
    }
    m_ownEdits.push_back(OwnEdit { msg.value("editId").toInt(), -1, range, xml, 0 });
    m_socket->sendTextMessage(text);
    return true;
}

// ---------------------------------------------------------------------------
// Outbox: edits made while we couldn't send
// ---------------------------------------------------------------------------

void CollabSyncService::addToOutbox(MeasureRange r)
{
    // Keep the ranges disjoint: merge any that overlap or touch this one.
    for (bool merged = true; merged;) {
        merged = false;
        for (auto it = m_outbox.begin(); it != m_outbox.end(); ++it) {
            const bool ticksTouch = r.tickFrom <= it->tickTo && it->tickFrom <= r.tickTo;
            const bool stavesTouch = r.staffFrom <= it->staffTo && it->staffFrom <= r.staffTo;
            if (ticksTouch && stavesTouch) {
                r.tickFrom = std::min(r.tickFrom, it->tickFrom);
                r.tickTo = std::max(r.tickTo, it->tickTo);
                r.staffFrom = std::min(r.staffFrom, it->staffFrom);
                r.staffTo = std::max(r.staffTo, it->staffTo);
                m_outbox.erase(it);
                merged = true;
                break;
            }
        }
    }
    m_outbox.push_back(r);

    // Every range is one message when we're back: past a handful, send one
    // range covering them all instead.
    static constexpr size_t MAX_OUTBOX_RANGES = 16;
    if (m_outbox.size() > MAX_OUTBOX_RANGES) {
        MeasureRange all = m_outbox.front();
        for (const MeasureRange& o : m_outbox) {
            all.tickFrom = std::min(all.tickFrom, o.tickFrom);
            all.tickTo = std::max(all.tickTo, o.tickTo);
            all.staffFrom = std::min(all.staffFrom, o.staffFrom);
            all.staffTo = std::max(all.staffTo, o.staffTo);
        }
        m_outbox = { all };
    }
}

void CollabSyncService::flushOutbox()
{
    if (m_outbox.empty() && !m_outboxStructure) {
        return;
    }
    // Only once we have everything the room has: what we send is the current
    // state of those measures, which then includes other people's edits to them.
    if (!m_connected || !m_caughtUp || m_waitingForRoomScore || !m_remoteQueue.empty() || !m_masterNotation
        || m_structureEditId >= 0) {
        return;
    }
    // The local relay keeps no history: sent to an empty room, they'd be lost
    // for whoever reconnects a moment later.
    if (!m_serverKeepsScores && m_othersInRoom == 0) {
        return;
    }
    if (isLocalUserBusy()) {
        m_flushTimer.start(); // mid-edit: try again shortly
        return;
    }
    Score* score = m_masterNotation->masterScore();

    if (m_outboxStructure) {
        // The whole score carries the measure edits too.
        m_outboxStructure = false;
        m_outbox.clear();
        LOGI() << "collab: sending the whole score (structure changed)";
        sendStructureChange();
        return;
    }

    size_t sent = 0;
    for (const MeasureRange& r : m_outbox) {
        if (sendMeasureEdit(score, r)) {
            ++sent;
        }
    }
    m_outbox.clear();
    LOGI() << "collab: sent " << sent << " held edit(s)";
}

void CollabSyncService::countOthers(const QJsonArray& peers)
{
    m_othersInRoom = 0;
    for (const QJsonValue& v : peers) {
        if (v.toObject().value("id").toInt(-1) != m_myClientId) {
            ++m_othersInRoom;
        }
    }
}

void CollabSyncService::dropOutbox(const char* why)
{
    if (m_outbox.empty() && !m_outboxStructure) {
        return;
    }
    LOGW() << "collab: " << m_outbox.size() << " unsent offline edit(s) dropped: " << why;
    m_outbox.clear();
    m_outboxStructure = false;
    showToast(ToastKind::Warning, "Offline edits not shared",
              std::string("Edits you made while offline weren't sent: ") + why + ".");
}

// ---------------------------------------------------------------------------
// Incoming: remote edits
// ---------------------------------------------------------------------------

bool CollabSyncService::isLocalUserBusy() const
{
    if (!m_notation || !m_masterNotation) {
        return false;
    }
    const Score* score = m_masterNotation->masterScore();
    if (score->undoStack()->hasActiveCommand() || m_notation->undoStack()->isLocked()) {
        return true; // an edit of ours is half-way through
    }
    const auto interaction = m_notation->interaction();
    return interaction->isDragStarted() || interaction->isTextEditingStarted();
}

void CollabSyncService::processRemoteQueue()
{
    while (!m_remoteQueue.empty()) {
        if (m_waitingForRoomScore) {
            return; // released by onMasterNotationChanged once the room's score is open
        }
        if (!m_masterNotation) {
            m_remoteQueue.clear();
            return;
        }
        if (isLocalUserBusy()) {
            m_retryApplyTimer.start(); // try again shortly
            return;
        }
        const RemoteEdit edit = std::move(m_remoteQueue.front());
        m_remoteQueue.pop_front();
        if (edit.structSeq >= 0 && edit.structSeq < m_structureSeq) {
            // Made before a structural change its sender hadn't seen yet: its
            // measure positions are out of date. Everyone drops it (the sender's
            // own copy lost it in the reload), so all copies stay the same.
            LOGW() << "collab: dropped edit #" << edit.seq << " from " << edit.from
                   << ": made before the structure change (#" << m_structureSeq << ")";
            continue;
        }
        m_structureSeq = std::max(m_structureSeq, edit.structSeq); // joiners learn it from later edits
        applyRemoteEdit(edit);
    }
    pruneOwnEdits();
    flushOutbox(); // offline edits wait until we have everyone else's
}

void CollabSyncService::applyRemoteEdit(const RemoteEdit& edit)
{
    Score* score = m_masterNotation->masterScore();
    if (!edit.range.isValid()) {
        return;
    }

    // Last write wins: our own edits the room puts after this one (or hasn't
    // numbered yet: it will, after this one) are already in our score, and
    // this one would wipe them. From the first that overlaps it, they go back
    // on top, in order - which is what everyone else ends up with.
    std::vector<const OwnEdit*> reapply;
    for (const OwnEdit& e : m_ownEdits) {
        const bool after = e.seq < 0 || e.seq > edit.seq;
        if (after && (!reapply.empty() || rangesOverlap(e.range, edit.range))) {
            reapply.push_back(&e);
        }
    }
    // Ours it replaces without its sender having seen them: say so.
    if (edit.baseSeq >= 0) {
        for (const OwnEdit& e : m_ownEdits) {
            if (e.seq > edit.baseSeq && e.seq < edit.seq && rangesOverlap(e.range, edit.range)) {
                notifyReplaced(edit, e);
                break;
            }
        }
    }

    // pasteStaff moves the selection to the pasted measures. Remember where
    // the local user was so we can put them back afterwards.
    const Selection savedSelection = score->selection();
    auto inAffectedArea = [&](const Fraction& tick, staff_idx_t staff) {
        const int t = tick.ticks();
        auto in = [&](const MeasureRange& r) {
            return t >= r.tickFrom && t < r.tickTo && staff >= r.staffFrom && staff < r.staffTo;
        };
        return in(edit.range) || std::any_of(reapply.begin(), reapply.end(), [&](const OwnEdit* e) { return in(e->range); });
    };

    bool listSelectionSurvives = true;
    for (const EngravingItem* e : savedSelection.elements()) {
        if (inAffectedArea(e->tick(), e->staffIdx())) {
            listSelectionSurvives = false; // those objects may be deleted by the swap
            break;
        }
    }
    const bool wasRange = savedSelection.isRange();
    const Fraction rangeTick1 = wasRange ? savedSelection.tickStart() : Fraction();
    const Fraction rangeTick2 = wasRange ? savedSelection.tickEnd() : Fraction();
    const staff_idx_t rangeStaff1 = savedSelection.staffStart();
    const staff_idx_t rangeStaff2 = savedSelection.staffEnd();

    InputState& is = score->inputState();
    const bool inNoteInput = is.noteEntryMode();
    const Fraction inputTick = (inNoteInput && is.segment()) ? is.tick() : Fraction(-1, 1);
    const bool inputInAffectedArea = inNoteInput && is.segment() && inAffectedArea(inputTick, is.staffIdx());

    // Swap the measures in as one undo step.
    m_applyingRemote = true;
    m_notation->undoStack()->prepareChanges(muse::TranslatableString::untranslatable("Edit by collaborator"));
    bool ok = applyMeasures(score, edit.range, edit.xml);
    for (const OwnEdit* e : reapply) {
        if (ok && !applyMeasures(score, e->range, e->xml)) {
            LOGW() << "collab: could not put our edit " << e->editId << " back on top of edit #" << edit.seq;
            ok = false;
        }
    }
    if (ok) {
        m_notation->undoStack()->commitChanges();
    } else {
        m_notation->undoStack()->rollbackChanges();
    }
    m_applyingRemote = false;

    // Put the local user's selection / input position back.
    if (wasRange && rangeStaff2 > rangeStaff1 && rangeStaff2 <= score->nstaves()) {
        Selection restored(score);
        restored.setRangeTicks(rangeTick1, rangeTick2, rangeStaff1, rangeStaff2);
        score->setSelection(restored);
        score->selection().updateSelectedElements();
    } else if (savedSelection.isList() && listSelectionSurvives) {
        score->setSelection(savedSelection);
    } else {
        score->deselectAll();
    }
    if (inputInAffectedArea) {
        if (Segment* seg = score->tick2segment(inputTick, true, SegmentType::ChordRest)) {
            is.setSegment(seg);
        }
    }

    m_notation->interaction()->selectionChanged().notify();
    m_notation->notationChanged().notify(); // endCmd alone doesn't trigger a redraw

    m_lastSeq = std::max(m_lastSeq, edit.seq);
    m_structure = scoreStructure(score); // a remote edit can't change it, but keep the baseline exact
    if (ok) {
        const qint64 latency = edit.sentAtMs > 0 ? nowMs() - edit.sentAtMs : -1;
        LOGI() << "collab: applied edit #" << edit.seq << " from " << edit.from
               << " (ticks " << edit.range.tickFrom << "-" << edit.range.tickTo
               << ", staves " << edit.range.staffFrom << "-" << edit.range.staffTo
               << "), latency " << latency << " ms"
               << (reapply.empty() ? "" : QString(", %1 of ours put back on top").arg(reapply.size()).toStdString());
    } else {
        LOGW() << "collab: could not apply edit #" << edit.seq << " from " << edit.from;
    }
}
