/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — the only collab header upstream files include.
 *
 * Each function is called from one "// COLLAB:" hook line in upstream code
 * (git grep -n "COLLAB:" lists them). They are safe to call when
 * collaboration is off: they do nothing.
 */
#pragma once

#include "draw/types/geometry.h"

class QQuickPaintedItem;

namespace muse::draw {
class Painter;
}

namespace mu::collab {
//! Score canvas: should it receive hover (mouse-move without a button held)
//! events? Upstream only enables them in note-input mode; collaboration needs
//! them all the time to share the cursor.
bool wantsHoverEvents();

//! Score canvas: the local mouse moved to `logicalPos` (score coordinates,
//! i.e. after undoing zoom/scroll — AbstractNotationPaintView::toLogical()).
void localPointerMoved(const muse::PointF& logicalPos);

//! Score canvas: the local mouse left the canvas.
void localPointerLeft();

//! Score canvas: draw collaborators' cursors. Call at the end of paint(),
//! while the painter still has the zoom/scroll transform set. `view` is
//! remembered so cursor moves can repaint just that canvas.
void paintRemoteCursors(muse::draw::Painter* painter, QQuickPaintedItem* view);

//! Closing the score (also on quit), after the person agreed to close it and
//! while it's still open: the last one in a shared score's room saves it on
//! the server in full. Blocks for at most 3 s.
void scoreAboutToClose();
}
