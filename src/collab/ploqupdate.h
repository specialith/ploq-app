/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — verified auto-update (our code, not upstream).
 *
 * Ploq reuses MuseScore's updater (src/framework/update: check, download,
 * "restart and install" dialog) but points it at Ploq's server and adds
 * verification through the COLLAB: hooks in appupdateservice.cpp:
 *
 * 1. The update description (latest.json) must carry a valid ECDSA P-256
 *    signature from Ploq's release key (public half built in below; the
 *    private half never leaves the release machine, see
 *    tools/collab/installer/README.md). Unsigned or altered -> ignored.
 * 2. The downloaded installer must have exactly the size and SHA-256 listed
 *    in that signed description. Otherwise it is deleted, never run.
 * 3. (Later) Windows code signing: verifyAuthenticode() is the one place to
 *    turn it on once the certificate arrives.
 */
#pragma once

#include <QByteArray>
#include <QString>

namespace mu::collab::ploq {
//! The update feed address. Default: https://collab.ploq.app/updates/latest.json.
//! PLOQ_UPDATE_URL overrides it for testing (https://, or http:// to this computer only).
//! Signatures are checked either way.
QString updateFeedUrl();

//! latest.json is {"signed": "<release JSON text>", "signature": "<base64>"}.
//! Returns the inner release JSON (MuseScore's release format plus "sha256"
//! and "size" per asset) if the signature is valid, else an empty array and
//! a reason in *error. Remembers the expected size/hash of each asset.
QByteArray unwrapSignedRelease(const QByteArray& feed, QString* error);

//! Checks a downloaded installer against the signed release description:
//! file name shape, size, SHA-256 (and Authenticode once enabled).
bool verifyDownloadedUpdate(const QString& fileName, const QByteArray& data, QString* error);

//! Windows code signing check of an installer on disk. Not enforced until
//! PLOQ_REQUIRE_AUTHENTICODE is defined (see ploqupdate.cpp).
bool verifyAuthenticode(const QString& path, QString* error);

//! Runs a downloaded update once Ploq is quitting: checks the file on disk
//! again (size + SHA-256 against the signed description this Ploq verified,
//! then Authenticode), then starts Windows Installer with a progress bar only
//! (msiexec /passive) and Ploq reopens when it's done. Returns false and runs
//! nothing if any check fails.
bool launchVerifiedInstaller(const QString& path, QString* error);
}
