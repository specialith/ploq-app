/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — upstream hook entry points. Each forwards to the running
 * sync service, and does nothing when collaboration is off.
 */
#include "collabhooks.h"

#include "internal/collabsyncservice.h"

namespace mu::collab {
bool wantsHoverEvents()
{
    return CollabSyncService::active() != nullptr;
}

void localPointerMoved(const muse::PointF& logicalPos)
{
    if (CollabSyncService* s = CollabSyncService::active()) {
        s->cursors().localMoved(logicalPos);
    }
}

void localPointerLeft()
{
    if (CollabSyncService* s = CollabSyncService::active()) {
        s->cursors().localLeft();
    }
}

void paintRemoteCursors(muse::draw::Painter* painter, QQuickPaintedItem* view)
{
    if (CollabSyncService* s = CollabSyncService::active()) {
        s->cursors().paint(painter, view);
    }
}

void scoreAboutToClose()
{
    if (CollabSyncService* s = CollabSyncService::active()) {
        s->onScoreAboutToClose();
    }
}
}
