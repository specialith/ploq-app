/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — edit sync (Phase 3).
 *
 * Listens for finished edits on the open score, sends the changed measures
 * to the relay as XML, and swaps measures received from others into our
 * score as one undoable step. Protocol: docs/collab/PROTOCOL.md.
 *
 * Off unless the COLLAB_SERVER environment variable is set, e.g.
 *   COLLAB_SERVER=ws://127.0.0.1:8787  COLLAB_ROOM=myscore  COLLAB_NAME=Alice
 */
#pragma once

#include <deque>
#include <memory>

#include <QByteArray>
#include <QString>
#include <QTimer>

#include "async/asyncable.h"
#include "modularity/ioc.h"
#include "context/iglobalcontext.h"
#include "notation/imasternotation.h"
#include "notation/notationtypes.h"
#include "toast/itoastservice.h"
#include "actions/iactionsdispatcher.h"
#include "actions/actiontypes.h"
#include "project/iprojectfilescontroller.h"

#include <functional>
#include <map>
#include <set>
#include <vector>

#include <QJsonArray>

#include <QShortcut>

#include "ui/imainwindow.h"
#include "appshell/internal/istartupscenario.h"

#include "muse_framework_config.h"
#ifdef MUSE_MODULE_AUTOBOT
#include "autobot/iautobot.h"
#endif

#include "account.h"
#include "cursorlayer.h"
#include "measurexml.h"
#include "selectionrects.h"
#include "sharecontroller.h"

class QWebSocket;

namespace mu::collab {
class CollabSyncService : public muse::async::Asyncable, public muse::Contextable
{
    muse::ContextInject<context::IGlobalContext> globalContext = { this };
    muse::GlobalInject<muse::toast::IToastService> toastService;
    muse::ContextInject<project::IProjectFilesController> projectFilesController = { this };
    muse::ContextInject<muse::actions::IActionsDispatcher> dispatcher = { this };
    muse::ContextInject<muse::ui::IMainWindow> mainWindow = { this };
    muse::ContextInject<appshell::IStartupScenario> startupScenario = { this }; // to not open a score mid-startup
#ifdef MUSE_MODULE_AUTOBOT
    muse::ContextInject<muse::autobot::IAutobot> autobot = { this }; // QA only: COLLAB_AUTOBOT
#endif

public:
    explicit CollabSyncService(const muse::modularity::ContextPtr& iocCtx);
    ~CollabSyncService();

    void init();
    void deinit();

    //! The running service when collaboration is on, else nullptr.
    //! Used by the upstream hook functions (collabhooks.h).
    static CollabSyncService* active();

    CursorLayer& cursors() { return m_cursors; }

    // For ShareController (sharecontroller.h)
    QString projectId() const { return m_projectId; }
    //! The open score is the score of the room we're in (remembered when it was
    //! shared, joined or downloaded): only then do its edits go to that room.
    bool isRoomScoreOpen() const;
    //! The shared project the open score belongs to: projectId() if the open
    //! score is its room's score (or it's frozen, with no room), else "".
    QString sharedProjectId() const;
    //! The shared project we're on the way into (asking the server, or its
    //! score still opening), else "".
    QString joiningProjectId() const;
    bool isConnected() const { return m_connected; }
    bool isAccountsMode() const { return m_accountsMode; }
    void joinSharedProject(const CollabAccount::Project& project) { joinProject(project); }
    //! Stop live editing (signed out): the score stays open as a local file.
    void leaveRoom();
    //! The open score is about to close (or the app to quit): if it's the room's
    //! score, we're the last one in the room and it has edits the server's copy
    //! lacks, save it there in full first (waits up to 3 s for the server).
    //! Then leave the room: the next score opened is not this room's.
    void onScoreAboutToClose();
    void onSignedIn();
    void notifySignedOut();
    void notify(const QString& text, bool info = false);

    //! A ploq:// invite / "Open in Ploq" link that arrived after start-up (macOS sends links to the
    //! running app). Joins right here if no score is open and we are in no room (Ploq was opened by
    //! that link); otherwise returns false and the caller opens a new Ploq for it.
    bool openLinkIfIdle(const QString& link);

private:
    struct RemoteEdit {
        int seq = 0;
        QString from;
        MeasureRange range;
        QByteArray xml;
        qint64 sentAtMs = 0;
        int structSeq = -1;   // structure version it was made on (-1: sender didn't say)
        int baseSeq = -1;     // newest room edit the sender's score had (-1: an older app)
    };

    // No measure locks: the room's edit order decides who wins a measure. Our own
    // edits are kept here until no collaborator's edit can come before them.
    struct OwnEdit {
        int editId = 0;
        int seq = -1;         // the room's number for it; -1 until acknowledged
        MeasureRange range;
        QByteArray xml;
        qint64 ackedAtMs = 0;
    };

    void connectToServer();
    void onConnected();
    void startQaScriptOnce();
    void onTextMessage(const QString& text);

    void onMasterNotationChanged();
    void onLocalChanges(const notation::ScoreChanges& changes);
    void sendSelection();

    // Joining late / reconnecting (Phase 6)
    void sendJson(const QJsonObject& msg);
    void onWelcomeSync(const QJsonObject& welcome);
    void uploadSnapshot(int baseSeq);
    void openRoomScore(const QJsonObject& msg);
    void openRoomFile(int attempt);

    // Last write wins (replaced measure locks on 2026-11-25)
    int seenSeq() const;
    void onOwnEditAcked(int editId, int seq);
    void pruneOwnEdits();
    void notifyReplaced(const RemoteEdit& edit, const OwnEdit& mine);

    // Structural changes (instruments, measures, time/key signatures)
    void sendStructureChange();

    // Outbox: edits made while offline (or before catching up after a
    // reconnect) are remembered as measure ranges and sent, as they are then,
    // once we're back and caught up.
    bool sendMeasureEdit(engraving::Score* score, const MeasureRange& range);
    void addToOutbox(MeasureRange range);
    void flushOutbox();
    void dropOutbox(const char* why);
    void countOthers(const QJsonArray& peers);

    enum class ToastKind {
        Warning, Info, Success, Error
    };
    void showToast(ToastKind kind, const std::string& title, const std::string& message);

    // Accounts, projects, invite links (Phase 7)
    void startAccountFlow();
    void signInAndContinue();
    void joinProject(const CollabAccount::Project& project);
    void connectToProject(const CollabAccount::Project& project);
    void onRoomRefused();
    void scheduleReconnect();
    void onSignInRejected();
    void saveRoomScoreIfLast();
    void announceFrozen(const CollabAccount::ProjectInfo& info);
    void installInviteShortcut();

    void processRemoteQueue();
    bool isLocalUserBusy() const;
    void applyRemoteEdit(const RemoteEdit& edit);

    bool m_enabled = false;
    QString m_serverUrl;
    QString m_room;
    QString m_name;
    QString m_color;

    std::unique_ptr<QWebSocket> m_socket;
    QTimer m_reconnectTimer;
    QTimer m_retryApplyTimer;
    bool m_connected = false;
    bool m_welcomedThisConnection = false;
    bool m_qaScriptStarted = false;    // COLLAB_AUTOBOT script launched (once per run)

    notation::IMasterNotationPtr m_masterNotation;
    notation::INotationPtr m_notation;

    CursorLayer m_cursors;

    int m_myClientId = -1;
    std::set<int> m_myClientIds;       // every id the server gave us (one per connection)
    bool m_serverKeepsScores = false;  // Cloudflare server: yes; local relay: no
    bool m_roomHasSnapshot = false;
    bool m_synced = false;             // caught up with the room at least once
    bool m_waitingForRoomScore = false; // downloading/opening the room's score
    int m_lastSeq = 0;                 // newest room edit our score contains
    int m_savedSeq = -1;               // newest room edit the server's saved copy contains
    std::function<void()> m_onSnapshotReply; // onScoreAboutToClose waiting for snapshot-saved/-refused
    QString m_roomFilePath;
    QString m_roomFileTitle;
    QByteArray m_roomFileData;         // the room's score, until written to m_roomFilePath
    std::deque<OwnEdit> m_ownEdits;    // ours, in the order sent (= the room's order)
    qint64 m_lastReplacedToastMs = 0;

    QByteArray m_structure;            // scoreStructure() after the last change we saw

    // Phase 7: COLLAB_SERVER=https://... -> accounts; ws://... -> plain rooms (local relay).
    bool m_accountsMode = false;
    QString m_wsBase;                  // wss://host for /room/<room>
    std::unique_ptr<CollabAccount> m_account;
    QString m_projectId;
    QString m_inviteCode;              // from COLLAB_INVITE (a texted link)
    QString m_loginCode;               // from COLLAB_INVITE (website "Open in Ploq")
    bool m_hostRequested = false;      // COLLAB_HOST=1|2: share the score we open
    bool m_hostWithDialog = false;     // COLLAB_HOST=2: through the Share dialog
    bool m_hostShareStarted = false;
    std::unique_ptr<ShareController> m_share; // Share button + dialog, browser sign-in
    std::unique_ptr<QShortcut> m_inviteShortcut;
    bool m_roomRefused = false;        // frozen / removed: don't reconnect
    int m_failedConnects = 0;          // in a row, without getting in
    QString m_frozenShownFor;          // project we already said "lapsed" for
    QString m_joiningRoom;             // asked the server about this room, not connected yet
    QString m_joiningProjectId;        // ...and its project
    int m_roomGeneration = 0;          // bumped by leaveRoom(): a join still asking the server is dropped

    bool m_applyingRemote = false;
    int m_nextEditId = 1;
    std::deque<RemoteEdit> m_remoteQueue;

    bool m_caughtUp = false;           // this connection has everything the room had when we joined
    std::vector<MeasureRange> m_outbox; // measures changed while we couldn't send (disjoint)
    bool m_outboxStructure = false;    // ...and the structure changed too: send the whole score
    QTimer m_flushTimer;
    int m_othersInRoom = 0;            // people in the room besides us (from presence)

    // Structure versions (online server): a structural change is room edit #N
    // and everyone reloads the score from it. Edits carry the number of the
    // structure they were made on; ones made on an older structure point at the
    // wrong measures, and everyone drops them.
    int m_structureSeq = 0;
    int m_structureEditId = -1;        // our structural change waiting for the server's ack
};
}
