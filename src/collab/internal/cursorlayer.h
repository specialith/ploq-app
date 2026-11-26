/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — live cursors (Phase 4).
 *
 * Cursors travel as a spot on the score — page number + position on that
 * page, in score units — never screen pixels, so they line up whatever each
 * person's zoom, scroll or page arrangement. Each side converts back through
 * its own zoom/scroll when drawing.
 *
 * Performance: a cursor move repaints only the small strips where that
 * cursor was and now is (QQuickPaintedItem::update(rect)), never the whole
 * score — a full-score repaint per mouse move is what made v1 feel slow.
 */
#pragma once

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <vector>

#include <QJsonArray>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QRectF>
#include <QString>
#include <QTimer>

#include "draw/types/geometry.h"
#include "draw/types/transform.h"
#include "global/types/color.h"

namespace mu::engraving {
class Score;
}

namespace muse::draw {
class Painter;
}

namespace mu::collab {
class CursorLayer
{
public:
    using SendFn = std::function<void (const QJsonObject&)>;

    CursorLayer();

    void setSend(SendFn send);
    void setScore(engraving::Score* score);
    void setLocalColor(const QString& color);

    // Local mouse (from the canvas hooks).
    void localMoved(const muse::PointF& logicalPos);
    void localLeft();

    //! `wanted` if it's a valid, non-blue colour; otherwise a non-blue
    //! palette colour picked by `seed`. Blue is MuseScore's own selection colour.
    static QString safeColor(const QString& wanted, qint64 seed);

    // Local selection (outlines from selectionOutlines()); sent only if changed.
    void sendSelection(const QJsonArray& outlines);
    void resendSelection(); // e.g. after reconnecting


    // From the server.
    void onRemoteSelection(const QJsonObject& msg);
    void onRemoteCursor(const QJsonObject& msg);
    void onPresence(const QJsonArray& peers);
    void clear();

    void paint(muse::draw::Painter* painter, QQuickPaintedItem* view);

private:
    struct Waypoint {
        int page = -1;
        muse::PointF pos;
        double at = 0.0; // when to move there, ms on m_clock
    };

    struct Remote {
        QString name;
        muse::Color color;
        int page = -1;
        muse::PointF pos;     // latest position received: relative to the page's top-left, score units
        int drawnPage = -1;
        muse::PointF drawnPos; // where it's drawn now: glides toward `pos` each frame
        double opacity = 0.0;  // fades to 0 after IDLE_FADE_AFTER_MS without movement
        qint64 lastMoveMs = 0;
        bool hidden = true;
        QRectF drawnRect;     // where it was last painted, canvas item coords
        double tagWidth = 160; // name tag size in item coords (measured at paint)
        double tagHeight = 22;
        std::vector<std::pair<int, muse::RectF> > selection; // page index, box relative to page
        std::deque<Waypoint> queue; // their recorded path, replayed with its original timing
        double queueEnd = 0.0;      // when the last queued point plays
        bool hideAfterQueue = false; // they left the score: hide once the replay ends
    };

    void sendNow(const QJsonObject& msg);
    void addSample(const muse::PointF& rel, double t);
    void flushBatch(); // send the recorded path now
    void dropBatch();  // forget it (nobody to send it to)
    //! Play the queued path points that are due; hide if it was their last.
    void playDue(Remote& r, double now);
    //! Point `r` at a new spot: glides there, or snaps if it's far / another page.
    void moveRemoteTo(Remote& r, int page, const muse::PointF& pos);
    void updateIdentity(int id, Remote& r, const QJsonObject& msg);

    //! Where `r` would be drawn now, in canvas item coordinates (empty if unknown).
    QRectF rectFor(const Remote& r) const;
    //! Distance between two spots on the same page, in screen pixels.
    double screenDistance(const muse::PointF& a, const muse::PointF& b) const;
    void startAnimation();
    int frameIntervalMs() const; // ~1000 / screen refresh rate, 120..360 fps
    void animationFrame(); // glide + fade, once per screen refresh (>= 120/s) while anything is visible
    void markDirty(const QRectF& rect);
    void markAllDirty();
    void flushRepaint();

    SendFn m_send;
    engraving::Score* m_score = nullptr;
    QString m_localColor;

    // Our cursor, sent in batches (see CURSOR_BATCH_MS) and only when someone else is here.
    bool m_othersPresent = false;
    QTimer m_batchTimer;
    QJsonArray m_batchPath; // x, y, ms since m_batchStartT, repeated
    int m_batchPage = -1;
    double m_batchStartT = 0.0;
    double m_lastSampleT = 0.0;
    muse::PointF m_unsampledPos; // latest move too soon after the last point
    double m_unsampledT = 0.0;
    bool m_hasUnsampled = false;
    int m_lastSentPage = -1;
    muse::PointF m_lastSentPos;
    bool m_shownToOthers = false; // sent a position since the last "hidden"

    QTimer m_animation;
    QElapsedTimer m_clock;
    double m_lastFrameT = 0.0; // ms on m_clock

    QString m_fpsLogPath; // COLLAB_FPS_LOG: append paints/s here (measuring only)
    int m_fpsPaints = 0;
    qint64 m_fpsWindowStart = 0;

    std::map<int, Remote> m_remotes; // by server client id
    QJsonArray m_lastSentSelection;

    // From the last paint: score units → painter coords, and the canvas's
    // device pixel ratio (painter coords = item coords × dpr).
    muse::draw::Transform m_lastToPainter;
    double m_lastDpr = 1.0;
    bool m_haveTransform = false;

    QTimer m_repaint;
    QRectF m_dirty;
    bool m_fullRepaint = false;
    std::vector<QPointer<QQuickPaintedItem> > m_views;
};
}
