/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — share what each person has selected (Phase 4 polish).
 *
 * A selection travels as outlines on the score: [page, x, y, w, h] per box,
 * relative to the page's top-left, in score units — like cursors, so it
 * lines up at any zoom. Range selections become one box per system (line of
 * music); single/list selections one box per selected item.
 */
#pragma once

#include <QJsonArray>

namespace mu::engraving {
class Score;
}

namespace mu::collab {
QJsonArray selectionOutlines(const engraving::Score* score);
}
