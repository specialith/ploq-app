/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq on macOS (our code, not upstream): ploq:// links. macOS doesn't start a new process with
 * the link as an argument (Windows: Ploq.exe --ploq-link, Linux: the desktop entry's %u); it
 * sends the link to Ploq.app as an "open URL" event (Info.plist registers the ploq scheme).
 * Built on macOS only (CMakeLists.txt).
 */
#pragma once

#include <functional>

#include <QString>

namespace mu::collab::ploq {
//! Catch ploq:// "open URL" events (QFileOpenEvent). Call from main() right after the QApplication
//! exists, before the app's modules start: a link that launched Ploq arrives while the modules
//! start up (AppShellModule processes it), before the collab service reads COLLAB_INVITE.
//! The link must have one of the exact shapes the Windows and Linux builds accept:
//! - sign-in (ploq://signin?state=...&code=...): handed to the waiting sign-in (BrowserSignIn).
//! - invite / "Open in Ploq": before the collab service has started, it becomes COLLAB_INVITE for
//!   this Ploq; after that, a new Ploq process joins with it (as on Windows, where every link
//!   starts a new Ploq.exe).
//! - anything else: refused with a message.
void macInstallLinkHandler();

//! Called by the collab service when it starts (it reads COLLAB_INVITE then). Later invite links go to
//! `openIfIdle` (CollabSyncService::openLinkIfIdle): it joins in this Ploq when no score is open, else
//! returns false and a new Ploq process joins instead.
void macCollabStarted(std::function<bool(const QString&)> openIfIdle);
}
