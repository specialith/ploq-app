/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — measure-level XML snapshots.
 *
 * Turns "what an edit touched" into whole measures, writes those measures as
 * XML (MuseScore's own clipboard format), and swaps received XML back in.
 * See docs/collab/ARCHITECTURE_NOTES.md section 3.
 */
#pragma once

#include <QByteArray>

#include "engraving/types/types.h"

namespace mu::engraving {
class Score;
struct ScoreChanges;
}

namespace mu::collab {
//! A block of whole measures on a run of staves.
struct MeasureRange {
    int tickFrom = -1;              // start of the first measure
    int tickTo = -1;                // end of the last measure (exclusive)
    engraving::staff_idx_t staffFrom = 0;
    engraving::staff_idx_t staffTo = 0; // exclusive

    bool isValid() const { return tickFrom >= 0 && tickTo > tickFrom && staffTo > staffFrom; }
};

//! Widens the area an edit (or undo/redo) changed to whole measures.
//! Falls back to the whole score when the change has no usable position.
MeasureRange measureRangeForChanges(engraving::Score* score, const engraving::ScoreChanges& changes);
//! Widens r so no tie crosses its edges (ties to notes outside it would be lost on paste).
void extendOverTies(engraving::Score* score, MeasureRange& r);

//! Writes the measures in `range` as clipboard-format XML (<StaffList>),
//! ignoring the user's selection filter so nothing is left out.
QByteArray writeMeasures(engraving::Score* score, const MeasureRange& range);

//! A short fingerprint of the score's *structure*: its instruments/staves,
//! number of measures, and each measure's time and key signatures. Edits
//! that change it can't travel as a measure paste — the whole score must.
QByteArray scoreStructure(const engraving::Score* score);

//! Replaces the measures in `range` with `xml`. Must be called between
//! startCmd/endCmd (prepareChanges/commitChanges) so it is one undo step.
bool applyMeasures(engraving::Score* score, const MeasureRange& range, const QByteArray& xml);
}
