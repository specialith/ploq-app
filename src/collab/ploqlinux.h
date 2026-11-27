/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq on Linux (our code, not upstream): the pieces that Windows does with
 * the registry, named pipes and BCrypt. Built on Linux only (CMakeLists.txt).
 */
#pragma once

#include <QByteArray>

namespace mu::collab::ploq {
//! ploq:// links on the command line, before anything else starts up (main.cpp).
//! A link launch is either `ploq --ploq-link <link>` or `ploq <link>` with the link as the
//! only argument (the desktop entry's `Exec=... %u`). The link must have one of the exact
//! shapes the Windows build accepts. Returns like Windows' ploqHandleLink: -1 not a link
//! launch, start normally; -2 an invite / "Open in Ploq" link: start normally with
//! COLLAB_INVITE set and argc cut to 1; 0 / 1 a sign-in link handed over / a link refused: exit.
int linuxHandleLinkArgs(int& argc, char** argv);

//! Make ploq:// links open this AppImage: writes a hidden desktop entry to
//! ~/.local/share/applications and makes it the x-scheme-handler/ploq default. Only when
//! running from an AppImage ($APPIMAGE); redone when the AppImage moves.
void linuxRegisterLinkHandler();

//! ECDSA P-256 / SHA-256 check of the update description (libgcrypt). `signature` is
//! r||s (64 bytes); the key is the uncompressed point (x, y) as hex.
bool linuxEcdsaP256Verify(const QByteArray& message, const QByteArray& signature, const char* keyXHex, const char* keyYHex);
}
