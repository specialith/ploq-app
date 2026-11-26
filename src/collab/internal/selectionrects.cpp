/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — share what each person has selected.
 */
#include "selectionrects.h"

#include <map>

#include "engraving/dom/engravingitem.h"
#include "engraving/dom/page.h"
#include "engraving/dom/score.h"
#include "engraving/dom/select.h"
#include "engraving/dom/system.h"

using namespace mu::engraving;

namespace mu::collab {
static constexpr size_t MAX_OUTLINES = 300; // keep huge list selections cheap to send

static void addOutline(QJsonArray& out, const Score* score, const Page* page, const RectF& canvasRect)
{
    if (!page || canvasRect.isEmpty()) {
        return;
    }
    const RectF r = canvasRect.translated(-page->pos());
    out.append(QJsonArray { static_cast<int>(score->pageIdx(page)), r.x(), r.y(), r.width(), r.height() });
}

QJsonArray selectionOutlines(const Score* score)
{
    QJsonArray out;
    if (!score) {
        return out;
    }
    const Selection& sel = score->selection();
    if (sel.isNone()) {
        return out;
    }

    if (sel.isRange()) {
        // One box per system: union of everything selected on that line.
        std::map<const System*, RectF> perSystem;
        for (const EngravingItem* e : sel.elements()) {
            const EngravingItem* sys = e->findAncestor(ElementType::SYSTEM);
            if (!sys) {
                continue;
            }
            RectF& r = perSystem[toSystem(sys)];
            const RectF er = e->canvasBoundingRect();
            r = r.isEmpty() ? er : r.united(er);
        }
        for (const auto& pair : perSystem) {
            addOutline(out, score, pair.first->page(), pair.second);
        }
        return out;
    }

    for (const EngravingItem* e : sel.elements()) {
        if (static_cast<size_t>(out.size()) >= MAX_OUTLINES) {
            break;
        }
        const EngravingItem* page = e->findAncestor(ElementType::PAGE);
        addOutline(out, score, page ? toPage(page) : nullptr, e->canvasBoundingRect());
    }
    return out;
}
}
