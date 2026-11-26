/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — crash reports (our code, not upstream).
 */
#pragma once

#include <QString>

class QObject;

namespace mu::collab::ploq {
//! Where Ploq.exe writes a minidump when it crashes: %LOCALAPPDATA%\Ploq\crashes.
//! src/app/main.cpp builds the same path with plain Win32 calls (it installs the
//! handler before Qt starts); keep the two in step.
QString crashDumpFolder();

//! Once the app is up: if Ploq crashed since the last time it said so, tell the user
//! that a crash report was saved and offer to open its folder. Also keeps only the
//! newest few dumps. Does nothing in QA runs (COLLAB_AUTOBOT, PLOQ_NO_ACTIVATE, or a
//! COLLAB_CRASH_DUMP folder of their own), where a dialog would block the run.
void offerCrashReportIfAny(QObject* ctx);
}
