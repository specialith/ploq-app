/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — network guard (our code, not upstream).
 */
#pragma once

#include <QString>
#include <QUrl>

namespace mu::collab::ploq {
//! True for Muse Group's servers (MuseScore.com/.org, MuseHub, Audio.com, their
//! update/extension feeds). Ploq never talks to them.
bool isMuseGroupHost(const QString& host);
bool isMuseGroupUrl(const QUrl& url);

//! Installs an app-wide Qt proxy rule that makes every Qt network connection
//! to a Muse Group server fail immediately (HTTP requests, WebSockets, plain
//! sockets). Everything else connects exactly as before. A safety net under
//! the build switches that remove those features (src/collab/PloqOptions.cmake).
//! Does not cover code outside Qt's network stack (e.g. the Muse Sounds
//! library, a separate DLL from MuseHub).
void installNetworkGuard();

//! How many connections the guard has refused so far (for logs/tests).
int blockedConnectionCount();
}
