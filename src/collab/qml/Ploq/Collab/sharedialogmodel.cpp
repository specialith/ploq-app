/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Ploq — the Share dialog's view of ShareController.
 */
#include "sharedialogmodel.h"

#include "internal/sharecontroller.h"

using namespace mu::collab;

static ShareController* ctrl()
{
    return ShareController::instance();
}

ShareDialogModel::ShareDialogModel(QObject* parent)
    : QObject(parent)
{
    if (ctrl()) {
        connect(ctrl(), &ShareController::changed, this, &ShareDialogModel::changed);
    }
}

ShareDialogModel::~ShareDialogModel()
{
    closed();
}

QString ShareDialogModel::status() const { return ctrl() ? ctrl()->statusName() : QString("idle"); }
QString ShareDialogModel::scoreTitle() const { return ctrl() ? ctrl()->scoreTitle() : QString(); }
QString ShareDialogModel::message() const { return ctrl() ? ctrl()->message() : QString(); }
QString ShareDialogModel::inviteLink() const { return ctrl() ? ctrl()->inviteLink() : QString(); }
bool ShareDialogModel::linkCopied() const { return ctrl() && ctrl()->linkCopied(); }
bool ShareDialogModel::canInvite() const { return ctrl() && ctrl()->canInvite(); }
bool ShareDialogModel::hasUpgrade() const { return ctrl() && ctrl()->hasUpgrade(); }
QString ShareDialogModel::accountName() const { return ctrl() ? ctrl()->accountName() : QString(); }
bool ShareDialogModel::signedIn() const { return ctrl() && ctrl()->isSignedIn(); }
bool ShareDialogModel::shared() const { return ctrl() && ctrl()->isShared(); }
QVariantList ShareDialogModel::members() const { return ctrl() ? ctrl()->members() : QVariantList(); }

void ShareDialogModel::copyLink() { if (ctrl()) { ctrl()->copyLink(); } }
void ShareDialogModel::openUpgrade() { if (ctrl()) { ctrl()->openUpgrade(); } }
void ShareDialogModel::manageOnWeb() { if (ctrl()) { ctrl()->manageOnWeb(); } }
void ShareDialogModel::signOut() { if (ctrl()) { ctrl()->signOut(); } }
void ShareDialogModel::retry() { if (ctrl()) { ctrl()->retry(); } }
void ShareDialogModel::reopenBrowser() { if (ctrl()) { ctrl()->reopenBrowser(); } }
void ShareDialogModel::cancelSignIn() { if (ctrl()) { ctrl()->cancelSignIn(); } }

void ShareDialogModel::closed()
{
    if (ctrl()) {
        ctrl()->dialogClosed();
    }
}
