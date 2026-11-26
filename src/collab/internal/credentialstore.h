/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — keep the sign-in token in the OS credential store (Windows
 * Credential Manager), not in MuseScore's plain-text settings file.
 */
#pragma once

#include <QString>

namespace mu::collab::credentials {
//! True if this platform has a credential store we use.
bool available();

//! The secret saved under `target`, or empty.
QString read(const QString& target);

//! Save `secret` under `target` (an empty secret deletes it). True on success.
bool write(const QString& target, const QString& secret);
}
