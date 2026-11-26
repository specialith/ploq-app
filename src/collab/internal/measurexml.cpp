/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — measure-level XML snapshots.
 */
#include "measurexml.h"

#include <algorithm>
#include <climits>
#include <set>
#include <utility>

#include <QCryptographicHash>
#include <QXmlStreamReader>

#include "engraving/dom/part.h"
#include "engraving/dom/staff.h"

#include "engraving/dom/chord.h"
#include "engraving/dom/engravingitem.h"
#include "engraving/dom/measure.h"
#include "engraving/dom/score.h"
#include "engraving/dom/note.h"
#include "engraving/dom/segment.h"
#include "engraving/dom/tie.h"
#include "engraving/dom/select.h"
#include "engraving/dom/selectionfilter.h"
#include "engraving/rw/xmlreader.h"

#include "log.h"

using namespace mu::engraving;

namespace mu::collab {
static MeasureRange wholeScore(Score* score)
{
    MeasureRange r;
    Measure* first = score->firstMeasure();
    Measure* last = score->lastMeasure();
    if (!first || !last) {
        return r;
    }
    r.tickFrom = first->tick().ticks();
    r.tickTo = last->endTick().ticks();
    r.staffFrom = 0;
    r.staffTo = score->nstaves();
    return r;
}

MeasureRange measureRangeForChanges(Score* score, const ScoreChanges& changes)
{
    int minTick = INT_MAX;
    int maxTick = -1;
    staff_idx_t minStaff = muse::nidx;
    staff_idx_t maxStaff = 0;

    auto addTick = [&](int t) {
        if (t >= 0) {
            minTick = std::min(minTick, t);
            maxTick = std::max(maxTick, t);
        }
    };
    auto addStaff = [&](staff_idx_t s) {
        if (s != muse::nidx && s < score->nstaves()) {
            minStaff = (minStaff == muse::nidx) ? s : std::min(minStaff, s);
            maxStaff = std::max(maxStaff, s);
        }
    };

    // The edit's own range (can be -1 / nidx, e.g. after undo — see notes).
    addTick(changes.tickFrom);
    addTick(changes.tickTo);

    // Plus every object the edit touched — reliable for edits and undo/redo.
    // Staves come only from objects that live on a staff (notes, chords,
    // rests, ...). Bar-wide bookkeeping objects - the measure, its time
    // segments, systems, pages - report staff 0 even when you edited the
    // bottom staff, which made a snare edit look like it touched the flute
    // staff above and collide with someone else's lock there.
    for (const auto& pair : changes.changedObjects) {
        if (!pair.first || !pair.first->isEngravingItem()) {
            continue;
        }
        const EngravingItem* item = toEngravingItem(pair.first);
        addTick(item->tick().ticks());
        const bool barWide = item->isMeasureBase() || item->isSegment() || item->isSystem() || item->isPage();
        if (!barWide && item->track() != muse::nidx) {
            addStaff(item->staffIdx());
        }
    }

    // No staff-bound object at all: fall back to the command's own staff
    // range (and, below, to all staves if that's unknown too).
    if (minStaff == muse::nidx) {
        addStaff(changes.staffIdxFrom);
        addStaff(changes.staffIdxTo);
    }

    if (maxTick < 0) {
        return wholeScore(score);
    }

    Measure* m1 = score->tick2measure(Fraction::fromTicks(minTick));
    Measure* m2 = score->tick2measure(Fraction::fromTicks(maxTick));
    if (!m1) {
        m1 = score->firstMeasure();
    }
    if (!m2) {
        m2 = score->lastMeasure();
    }
    if (!m1 || !m2) {
        return MeasureRange();
    }

    MeasureRange r;
    r.tickFrom = m1->tick().ticks();
    r.tickTo = std::max(m1->endTick().ticks(), m2->endTick().ticks());
    if (minStaff == muse::nidx) {
        r.staffFrom = 0;
        r.staffTo = score->nstaves();
    } else {
        r.staffFrom = minStaff;
        r.staffTo = std::min(maxStaff + 1, score->nstaves());
    }
    extendOverTies(score, r);
    return r;
}

//! Widen r so no tie crosses its edges. The receiver clears and re-pastes the
//! measures in the range: a tie to a note outside it was dropped there (edit a
//! measure where a tie starts -> everyone else lost the tie). Repeats until
//! stable so whole tie chains come along.
void extendOverTies(Score* score, MeasureRange& r)
{
    for (int pass = 0; pass < 64; ++pass) {
        int from = r.tickFrom;
        int to = r.tickTo;
        for (Measure* m = score->tick2measure(Fraction::fromTicks(r.tickFrom)); m && m->tick().ticks() < r.tickTo;
             m = m->nextMeasure()) {
            for (Segment* s = m->first(SegmentType::ChordRest); s; s = s->next(SegmentType::ChordRest)) {
                for (staff_idx_t staff = r.staffFrom; staff < r.staffTo; ++staff) {
                    for (voice_idx_t v = 0; v < VOICES; ++v) {
                        EngravingItem* e = s->element(staff * VOICES + v);
                        if (!e || !e->isChord()) {
                            continue;
                        }
                        for (const Note* n : toChord(e)->notes()) {
                            if (n->tieFor() && n->tieFor()->endNote()) {
                                const Measure* em = n->tieFor()->endNote()->chord()->measure();
                                to = std::max(to, em->endTick().ticks());
                            }
                            if (n->tieBack() && n->tieBack()->startNote()) {
                                const Measure* sm = n->tieBack()->startNote()->chord()->measure();
                                from = std::min(from, sm->tick().ticks());
                            }
                        }
                    }
                }
            }
        }
        if (from == r.tickFrom && to == r.tickTo) {
            return;
        }
        r.tickFrom = from;
        r.tickTo = to;
    }
}

QByteArray writeMeasures(Score* score, const MeasureRange& range)
{
    IF_ASSERT_FAILED(score && range.isValid() && range.staffTo <= score->nstaves()) {
        return QByteArray();
    }

    Measure* first = score->tick2measure(Fraction::fromTicks(range.tickFrom));
    Measure* last = score->tick2measure(Fraction::fromTicks(range.tickTo - 1));
    if (!first || !last) {
        return QByteArray();
    }

    Segment* startSeg = first->first(SegmentType::ChordRest);
    Measure* afterLast = last->nextMeasure();
    Segment* endSeg = afterLast ? afterLast->first(SegmentType::ChordRest) : nullptr;
    if (!startSeg) {
        return QByteArray();
    }

    // Build a throwaway range selection and reuse upstream's copy writer
    // (Selection::mimeData -> staffMimeData for a range). Building it flags
    // "selection changed" on the score, and the writer honours the user's
    // selection filter — undo both.
    const bool selectionChangedFlag = score->selectionChanged();
    const SelectionFilter userFilter = score->selectionFilter();
    score->selectionFilter() = SelectionFilter(); // default = let everything through

    Selection sel(score);
    sel.setRange(startSeg, endSeg, range.staffFrom, range.staffTo);

    // The writer also asks the *score's* selection about multi-note chords
    // (WriteContext::canWriteNoteIdx), which asserts unless it is a range.
    // Briefly install ours as the score's selection object — a plain object
    // swap, no redraw or element "selected" flags touched — then put the
    // user's back.
    const Selection userSelection = score->selection();
    score->selection() = sel;
    const muse::ByteArray data = sel.mimeData();
    score->selection() = userSelection;

    score->selectionFilter() = userFilter;
    score->setSelectionChanged(selectionChangedFlag);

    return data.toQByteArray();
}

QByteArray scoreStructure(const Score* score)
{
    if (!score) {
        return QByteArray();
    }
    QByteArray s;
    s += QByteArray::number(static_cast<qulonglong>(score->nstaves())) + "|";
    s += QByteArray::number(static_cast<qulonglong>(score->nmeasures())) + "|";
    for (const Part* part : score->parts()) {
        s += part->instrumentId().toQString().toUtf8() + ":" + part->partName().toQString().toUtf8() + ":"
             + QByteArray::number(static_cast<qulonglong>(part->nstaves())) + ";";
    }
    for (const Measure* m = score->firstMeasure(); m; m = m->nextMeasure()) {
        s += "|" + QByteArray::number(m->timesig().numerator()) + "/" + QByteArray::number(m->timesig().denominator());
        for (staff_idx_t i = 0; i < score->nstaves(); ++i) {
            s += "k" + QByteArray::number(static_cast<int>(score->staff(i)->key(m->tick())));
        }
    }
    return QCryptographicHash::hash(s, QCryptographicHash::Sha1).toHex();
}

//! Does the <StaffList> describe exactly `range`? The message's range is what
//! the server checked against locks; the XML carries its own start staff,
//! staff count, staff ids and length, and pasteStaff follows those. If they
//! disagreed, a member could claim one area and write into another (e.g.
//! someone else's locked measures), so anything that doesn't match is refused.
static bool xmlMatchesRange(const QByteArray& xml, const MeasureRange& range)
{
    QXmlStreamReader r(xml);
    int staffLists = 0;
    bool ok = true;
    while (!r.atEnd() && ok) {
        r.readNext();
        if (!r.isStartElement()) {
            continue;
        }
        if (r.name() == u"StaffList") {
            ++staffLists;
            const auto a = r.attributes();
            bool okStaff = false;
            bool okStaves = false;
            const int staff = a.value(u"staff").toInt(&okStaff);
            const int staves = a.value(u"staves").toInt(&okStaves);
            const int len = Fraction::fromString(muse::String::fromQString(a.value(u"len").toString())).ticks();
            ok = okStaff && okStaves && staff == static_cast<int>(range.staffFrom)
                 && staves == static_cast<int>(range.staffTo - range.staffFrom)
                 && len == range.tickTo - range.tickFrom;
        } else if (r.name() == u"Staff") {
            bool okId = false;
            const int id = r.attributes().value(u"id").toInt(&okId);
            ok = okId && id >= static_cast<int>(range.staffFrom) && id < static_cast<int>(range.staffTo);
        }
    }
    return ok && staffLists == 1 && !r.hasError();
}

//! Which (staff, voice) pairs carry content in a <StaffList> document.
static std::set<std::pair<int, int> > voicesInXml(const QByteArray& xml)
{
    std::set<std::pair<int, int> > present;
    QXmlStreamReader r(xml);
    int staff = -1;
    while (!r.atEnd()) {
        r.readNext();
        if (!r.isStartElement()) {
            continue;
        }
        if (r.name() == u"Staff") {
            staff = r.attributes().value(u"id").toInt();
        } else if (r.name() == u"voice" && staff >= 0) {
            present.insert({ staff, r.attributes().value(u"id").toInt() });
        }
    }
    return present;
}

bool applyMeasures(Score* score, const MeasureRange& range, const QByteArray& xml)
{
    IF_ASSERT_FAILED(score && range.isValid()) {
        return false;
    }
    if (range.staffTo > score->nstaves()) {
        LOGW() << "collab: incoming staves " << range.staffFrom << "-" << range.staffTo
               << " but this score has " << score->nstaves();
        return false;
    }

    const Fraction tickFrom = Fraction::fromTicks(range.tickFrom);
    const Fraction len = Fraction::fromTicks(range.tickTo - range.tickFrom);

    // The range comes from someone else: it must be whole measures that exist
    // in *this* score before we hand anything to the paste code.
    const Measure* firstM = score->tick2measure(tickFrom);
    const Measure* lastM = score->lastMeasure();
    if (!firstM || firstM->tick() != tickFrom || !lastM || range.tickTo > lastM->endTick().ticks()) {
        LOGW() << "collab: incoming ticks " << range.tickFrom << "-" << range.tickTo
               << " don't match this score's measures (scores out of sync?)";
        return false;
    }
    if (!xml.startsWith("<?xml") || !xml.contains("<StaffList")) {
        LOGW() << "collab: incoming edit is not a StaffList document";
        return false;
    }
    if (!xmlMatchesRange(xml, range)) {
        LOGW() << "collab: incoming edit's XML doesn't match the measures/staves it claims; ignored";
        return false;
    }

    Segment* dst = score->tick2segment(tickFrom, true, SegmentType::ChordRest);
    if (!dst) {
        LOGW() << "collab: no measure at tick " << range.tickFrom << " (scores out of sync?)";
        return false;
    }

    // 1. Paste. For every voice present in the XML, pasteStaff first clears
    //    that voice in the range (makeGap1), then reads the content in.
    XmlReader reader(xml);
    if (!score->pasteStaff(reader, dst, range.staffFrom)) {
        LOGW() << "collab: pasteStaff failed";
        return false;
    }

    // 2. pasteStaff leaves voices that are empty in the XML untouched, so a
    //    voice the other person emptied would survive here. Clear those.
    const std::set<std::pair<int, int> > present = voicesInXml(xml);
    for (staff_idx_t staff = range.staffFrom; staff < range.staffTo; ++staff) {
        Fraction offsets[VOICES];
        bool anyToClear = false;
        for (voice_idx_t v = 0; v < VOICES; ++v) {
            const bool inXml = present.count({ static_cast<int>(staff), static_cast<int>(v) }) > 0;
            // Voice 1 (index 0) always holds notes or rests, so never clear it here.
            if (v > 0 && !inXml) {
                offsets[v] = Fraction(0, 1);
                anyToClear = true;
            } else {
                offsets[v] = Fraction(1, 0); // invalid = skip this voice
            }
        }
        if (anyToClear) {
            score->makeGap1(tickFrom, staff, len, offsets);
        }
    }

    return true;
}
}
