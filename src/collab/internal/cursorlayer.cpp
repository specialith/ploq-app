/*
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * Collab fork — live cursors (Phase 4).
 */
#include "cursorlayer.h"
#include "account.h" // CollabAccount::cleanText for peer names

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

#include <QColor>
#include <QDateTime>
#include <QFile>
#include <QScreen>
#include <QQuickWindow>

#include "draw/fontmetrics.h"
#include "draw/painter.h"
#include "draw/types/brush.h"
#include "draw/types/font.h"
#include "draw/types/pen.h"

#include "engraving/dom/page.h"
#include "engraving/dom/score.h"

using namespace mu::collab;
using namespace mu::engraving;
using namespace muse;
using namespace muse::draw;

// ---- Tweak me ------------------------------------------------------------
// How fast a collaborator's cursor glides to where they moved: the fraction
// of the remaining distance it covers each frame (at 60 fps). 0 < value <= 1.
// Higher = snappier (1 = jump straight there), lower = floatier.
static constexpr double CURSOR_GLIDE = 0.12;
// Cursor moves are recorded, then sent as ONE message per this many ms (the
// path inside it), and replayed on the other side. Every message we send costs
// money on the server (Cloudflare bills each 20 incoming WebSocket messages as
// a request), so: higher = cheaper but others see you later; 500 = 2 msgs/s.
static constexpr int CURSOR_BATCH_MS = 500;
// ---------------------------------------------------------------------------

static constexpr double SAMPLE_MS = 33.0;          // path points recorded at most ~30 times a second
static constexpr double MAX_BACKLOG_MS = 1.5 * CURSOR_BATCH_MS; // replay this far behind: skip ahead
// What we take from another member's cursor or selection message (the server checks the same; these
// hold for any server). We send about 16 points per batch, each timed within the batch.
static constexpr int MAX_PATH_POINTS = 64;
static constexpr double MAX_PATH_MS = 4 * CURSOR_BATCH_MS;
static constexpr size_t MAX_QUEUED_POINTS = 256;   // per person: about 8 batches
static constexpr double MAX_COORD = 1e6;           // page units; a page is a few thousand across
static constexpr int MAX_REMOTE_OUTLINES = 300;

static bool usableCoord(double v)
{
    return std::isfinite(v) && std::abs(v) <= MAX_COORD;
}
static constexpr double SNAP_DISTANCE_PX = 400.0;  // farther than this on screen: jump, don't glide
static constexpr double SETTLE_DISTANCE_PX = 0.3;  // close enough: stop gliding
static constexpr qint64 IDLE_FADE_AFTER_MS = 5000; // fade a cursor that hasn't moved for this long
static constexpr qint64 FADE_DURATION_MS = 400;
// Animation frame rate follows the monitor (240 Hz screen -> ~240 fps), never below 120.
static constexpr double MIN_ANIMATION_FPS = 120.0;
static constexpr double MAX_ANIMATION_FPS = 360.0;
static constexpr int REPAINT_INTERVAL_MS = 0;      // next event-loop pass: merges messages that arrived together

// Cursor graphic (Google Slides style), in item coordinates (multiplied by the
// device pixel ratio when drawing, so it's the same physical size on any screen).
static constexpr double ARROW_W = 13.0;
static constexpr double ARROW_H = 13.0;
static constexpr double TAG_X = 9.0;   // name tag sits just below-right of the arrow
static constexpr double TAG_Y = 11.0;
static constexpr double MARGIN = 3.0;

static qint64 cursorNowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

CursorLayer::CursorLayer()
{
    m_batchTimer.setSingleShot(true);
    m_batchTimer.setInterval(CURSOR_BATCH_MS);
    m_batchTimer.setTimerType(Qt::PreciseTimer); // default timers on Windows round to ~15 ms
    QObject::connect(&m_batchTimer, &QTimer::timeout, [this]() { flushBatch(); });

    m_animation.setTimerType(Qt::PreciseTimer);
    m_clock.start();
    m_fpsLogPath = qEnvironmentVariable("COLLAB_FPS_LOG"); // measuring only: paints/s per second
    QObject::connect(&m_animation, &QTimer::timeout, [this]() { animationFrame(); });

    m_repaint.setSingleShot(true);
    m_repaint.setInterval(REPAINT_INTERVAL_MS);
    m_repaint.setTimerType(Qt::PreciseTimer);
    QObject::connect(&m_repaint, &QTimer::timeout, [this]() { flushRepaint(); });
}

void CursorLayer::setSend(SendFn send)
{
    m_send = std::move(send);
}

void CursorLayer::setScore(Score* score)
{
    m_score = score;
    clear();
}

void CursorLayer::setLocalColor(const QString& color)
{
    m_localColor = color;
}

// ---------------------------------------------------------------------------
// Colours: never blue — blue is MuseScore's own selection colour, so a blue
// collaborator would be indistinguishable from your own selection.
// ---------------------------------------------------------------------------

static const char* const SAFE_PALETTE[] = { "#e6194b", "#f58231", "#3cb44b", "#911eb4", "#f032e6", "#9a6324" };

static bool isBlueish(const QColor& c)
{
    // Hue 180-260 is cyan → blue → indigo; ignore greys (low saturation).
    const int hue = c.hsvHue();
    return c.isValid() && hue >= 180 && hue <= 260 && c.hsvSaturation() > 60;
}

QString CursorLayer::safeColor(const QString& wanted, qint64 seed)
{
    const QColor c(wanted);
    if (c.isValid() && !isBlueish(c)) {
        return c.name();
    }
    const size_t n = std::size(SAFE_PALETTE);
    return QString(SAFE_PALETTE[static_cast<size_t>(seed < 0 ? -seed : seed) % n]);
}

void CursorLayer::updateIdentity(int id, Remote& r, const QJsonObject& msg)
{
    r.name = CollabAccount::cleanText(msg.value("name").toString(), 40); // a peer's name: no control/bidi tricks
    if (msg.contains("color")) {
        r.color = Color::fromString(safeColor(msg.value("color").toString(), id).toStdString());
    }
}

// ---------------------------------------------------------------------------
// Selections
// ---------------------------------------------------------------------------

void CursorLayer::sendSelection(const QJsonArray& outlines)
{
    if (outlines == m_lastSentSelection) {
        return; // unchanged: don't spam everyone
    }
    m_lastSentSelection = outlines;
    sendNow(QJsonObject {
        { "type", "selection" },
        { "outlines", outlines },
        { "color", m_localColor },
    });
}

void CursorLayer::resendSelection()
{
    const QJsonArray last = m_lastSentSelection;
    m_lastSentSelection = QJsonArray { QJsonValue("force") };
    sendSelection(last);
}

void CursorLayer::onRemoteSelection(const QJsonObject& msg)
{
    const int id = msg.value("from").toInt(-1);
    if (id < 0) {
        return;
    }
    Remote& r = m_remotes[id];
    updateIdentity(id, r, msg);
    r.selection.clear();
    for (const QJsonValue& v : msg.value("outlines").toArray()) {
        if (static_cast<int>(r.selection.size()) >= MAX_REMOTE_OUTLINES) {
            break;
        }
        const QJsonArray a = v.toArray();
        if (a.size() != 5) {
            continue;
        }
        const double x = a.at(1).toDouble(), y = a.at(2).toDouble(), w = a.at(3).toDouble(), h = a.at(4).toDouble();
        if (!usableCoord(x) || !usableCoord(y) || !usableCoord(w) || !usableCoord(h) || w < 0 || h < 0) {
            continue;
        }
        r.selection.push_back({ a.at(0).toInt(-1), RectF(x, y, w, h) });
    }
    markAllDirty(); // selections change rarely; a full repaint is fine
}

void CursorLayer::sendNow(const QJsonObject& msg)
{
    if (m_send) {
        m_send(msg);
    }
}

// ---------------------------------------------------------------------------
// Local mouse → "page + position on the page"
// ---------------------------------------------------------------------------

void CursorLayer::localMoved(const PointF& logicalPos)
{
    // Alone in the room: nobody to show it to, so don't spend server requests on it.
    if (!m_othersPresent || !m_score || m_score->pages().empty()) {
        return;
    }

    // The page under the mouse, or the nearest one when between pages.
    Page* page = m_score->searchPage(logicalPos);
    if (!page) {
        double best = std::numeric_limits<double>::max();
        for (Page* p : m_score->pages()) {
            const RectF r = p->ldata()->bbox().translated(p->pos());
            const double dx = std::max({ r.left() - logicalPos.x(), 0.0, logicalPos.x() - r.right() });
            const double dy = std::max({ r.top() - logicalPos.y(), 0.0, logicalPos.y() - r.bottom() });
            const double d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                page = p;
            }
        }
    }
    if (!page) {
        return;
    }

    const PointF rel(std::round((logicalPos.x() - page->pos().x()) * 10) / 10,
                     std::round((logicalPos.y() - page->pos().y()) * 10) / 10);
    const int pageIdx = static_cast<int>(m_score->pageIdx(page));
    const double t = m_clock.nsecsElapsed() / 1e6;

    // A batch is one page: moving to another page sends what we have first.
    if (!m_batchPath.isEmpty() && pageIdx != m_batchPage) {
        flushBatch();
    }

    if (m_batchPath.isEmpty()) {
        if (pageIdx == m_lastSentPage && rel == m_lastSentPos) {
            return; // hasn't actually moved
        }
        // First move of a batch: record it, send the batch CURSOR_BATCH_MS later.
        m_batchPage = pageIdx;
        m_batchStartT = t;
        addSample(rel, t);
        m_batchTimer.start();
    } else if (t - m_lastSampleT >= SAMPLE_MS) {
        addSample(rel, t);
    } else {
        // Too soon for a new point; keep it so the batch still ends where the mouse stopped.
        m_unsampledPos = rel;
        m_unsampledT = t;
        m_hasUnsampled = true;
    }
}

void CursorLayer::addSample(const PointF& rel, double t)
{
    m_batchPath.append(rel.x());
    m_batchPath.append(rel.y());
    m_batchPath.append(static_cast<int>(std::round(t - m_batchStartT)));
    m_lastSampleT = t;
    m_hasUnsampled = false;
}

void CursorLayer::flushBatch()
{
    m_batchTimer.stop();
    if (m_hasUnsampled) {
        addSample(m_unsampledPos, m_unsampledT);
    }
    if (m_batchPath.isEmpty()) {
        return;
    }
    const int n = m_batchPath.size();
    const PointF last(m_batchPath.at(n - 3).toDouble(), m_batchPath.at(n - 2).toDouble());
    // `x`/`y` = where the batch ends (what an older client shows); `path` =
    // [x, y, ms since the batch started, ...] for a smooth replay.
    sendNow(QJsonObject {
        { "type", "cursor" },
        { "page", m_batchPage },
        { "x", last.x() },
        { "y", last.y() },
        { "color", m_localColor },
        { "path", m_batchPath },
    });
    m_lastSentPage = m_batchPage;
    m_lastSentPos = last;
    m_shownToOthers = true;
    m_batchPath = QJsonArray();
    m_batchPage = -1;
}

void CursorLayer::dropBatch()
{
    m_batchTimer.stop();
    m_batchPath = QJsonArray();
    m_batchPage = -1;
    m_hasUnsampled = false;
}

void CursorLayer::localLeft()
{
    if (!m_othersPresent) {
        dropBatch();
        return;
    }
    flushBatch(); // others replay the path, then hide the cursor
    m_lastSentPage = -1;
    // Only if they can see it: leave events also fire over popups and panels,
    // and each hide would otherwise be another billed message.
    if (m_shownToOthers) {
        m_shownToOthers = false;
        sendNow(QJsonObject { { "type", "cursor" }, { "hidden", true } });
    }
}

// ---------------------------------------------------------------------------
// Remote cursors
// ---------------------------------------------------------------------------

QRectF CursorLayer::rectFor(const Remote& r) const
{
    if (!m_haveTransform || !m_score || r.hidden || r.drawnPage < 0
        || static_cast<size_t>(r.drawnPage) >= m_score->pages().size()) {
        return QRectF();
    }
    const PointF p = m_lastToPainter.map(m_score->pages()[r.drawnPage]->pos() + r.drawnPos);
    const double x = p.x() / m_lastDpr;
    const double y = p.y() / m_lastDpr;
    const double w = std::max(ARROW_W, TAG_X + r.tagWidth);
    const double h = std::max(ARROW_H, TAG_Y + r.tagHeight);
    return QRectF(x - MARGIN, y - MARGIN, w + 2 * MARGIN, h + 2 * MARGIN);
}

double CursorLayer::screenDistance(const PointF& a, const PointF& b) const
{
    // Score units -> screen pixels: our current zoom (the transform's scale).
    const double zoom = std::hypot(m_lastToPainter.m11(), m_lastToPainter.m12()) / m_lastDpr;
    return std::hypot(a.x() - b.x(), a.y() - b.y()) * zoom;
}

void CursorLayer::onRemoteCursor(const QJsonObject& msg)
{
    const int id = msg.value("from").toInt(-1);
    if (id < 0) {
        return;
    }
    Remote& r = m_remotes[id];
    const QRectF before = r.drawnRect.isEmpty() ? rectFor(r) : r.drawnRect;
    updateIdentity(id, r, msg);

    if (msg.value("hidden").toBool(false)) {
        if (!r.queue.empty()) {
            r.hideAfterQueue = true; // finish replaying their path first
            return;
        }
        r.hidden = true; // mouse left their score: remove it now
        markDirty(before);
        return;
    }
    r.hideAfterQueue = false;

    const int page = msg.value("page").toInt(-1);
    const double endX = msg.value("x").toDouble();
    const double endY = msg.value("y").toDouble();
    if (!usableCoord(endX) || !usableCoord(endY)) {
        return;
    }
    const QJsonArray path = msg.value("path").toArray();
    if (path.size() < 3 || path.size() > MAX_PATH_POINTS * 3) {
        // Older client (or a path no app sends): just the latest spot.
        r.queue.clear();
        moveRemoteTo(r, page, PointF(endX, endY));
        return;
    }

    // Replay the path with its original timing, right after the previous batch
    // (which arrived ~CURSOR_BATCH_MS ago and should be just finishing).
    const double now = m_clock.nsecsElapsed() / 1e6;
    double base = std::max(now, r.queueEnd);
    if (base - now > MAX_BACKLOG_MS) {
        // Fallen behind (network hiccup): jump to the end of what's queued.
        while (!r.queue.empty()) {
            moveRemoteTo(r, r.queue.front().page, r.queue.front().pos);
            r.queue.pop_front();
        }
        base = now;
    }
    double last = 0.0;
    for (int i = 0; i + 2 < path.size(); i += 3) {
        const double x = path.at(i).toDouble();
        const double y = path.at(i + 1).toDouble();
        // Times in order and within the batch: one far ahead would hold everything behind it in the queue.
        const double offset = path.at(i + 2).toDouble();
        if (!usableCoord(x) || !usableCoord(y) || !std::isfinite(offset)) {
            continue;
        }
        last = std::clamp(offset, last, MAX_PATH_MS);
        const double at = base + last;
        r.queue.push_back({ page, PointF(x, y), at });
        r.queueEnd = at;
    }
    while (r.queue.size() > MAX_QUEUED_POINTS) {
        moveRemoteTo(r, r.queue.front().page, r.queue.front().pos);
        r.queue.pop_front();
    }
    playDue(r, now);
    startAnimation();
}

void CursorLayer::playDue(Remote& r, double now)
{
    while (!r.queue.empty() && r.queue.front().at <= now) {
        moveRemoteTo(r, r.queue.front().page, r.queue.front().pos);
        r.queue.pop_front();
    }
    if (r.queue.empty() && r.hideAfterQueue) {
        r.hideAfterQueue = false;
        const QRectF before = r.drawnRect.isEmpty() ? rectFor(r) : r.drawnRect;
        r.hidden = true;
        markDirty(before);
    }
}

void CursorLayer::moveRemoteTo(Remote& r, int page, const PointF& pos)
{
    const QRectF before = r.drawnRect.isEmpty() ? rectFor(r) : r.drawnRect;
    const bool wasShowing = !r.hidden && r.drawnPage >= 0 && r.opacity > 0.0;

    // Don't glide from nowhere, across pages, or across half the screen: snap.
    const bool snap = !wasShowing || !m_haveTransform || page != r.drawnPage
                      || screenDistance(r.drawnPos, pos) > SNAP_DISTANCE_PX;

    r.page = page;
    r.pos = pos;
    r.hidden = false;
    r.opacity = 1.0;
    r.lastMoveMs = cursorNowMs();
    if (snap) {
        r.drawnPage = page;
        r.drawnPos = pos;
    }

    if (!m_haveTransform) {
        markAllDirty(); // never painted yet: no idea where things are
    } else {
        markDirty(before);
        markDirty(rectFor(r));
    }
    startAnimation();
}

void CursorLayer::startAnimation()
{
    if (!m_animation.isActive()) {
        m_animation.setInterval(frameIntervalMs());
        m_lastFrameT = m_clock.nsecsElapsed() / 1e6;
        m_animation.start();
    }
}

int CursorLayer::frameIntervalMs() const
{
    // One tick per screen refresh: the glide then moves every frame the
    // monitor shows, instead of every 4th frame on a 240 Hz screen.
    double hz = 0.0;
    for (const QPointer<QQuickPaintedItem>& view : m_views) {
        if (view && view->window() && view->window()->screen()) {
            hz = std::max(hz, view->window()->screen()->refreshRate());
        }
    }
    hz = std::clamp(hz, MIN_ANIMATION_FPS, MAX_ANIMATION_FPS);
    return std::max(1, static_cast<int>(1000.0 / hz));
}

void CursorLayer::animationFrame()
{
    const qint64 now = cursorNowMs();
    // Sub-millisecond clock: at 240 fps a frame is ~4 ms, so whole-ms steps would judder.
    const double t = m_clock.nsecsElapsed() / 1e6;
    const double frames = std::clamp((t - m_lastFrameT) / (1000.0 / 60.0), 0.0, 10.0);
    m_lastFrameT = t;
    if (const int interval = frameIntervalMs(); interval != m_animation.interval()) {
        m_animation.setInterval(interval); // window known now, or moved to another screen
    }
    // Same glide whatever the real frame rate: CURSOR_GLIDE is "per 60 fps frame".
    const double step = 1.0 - std::pow(1.0 - std::clamp(CURSOR_GLIDE, 0.01, 1.0), frames);

    bool stillNeeded = false;
    for (auto& pair : m_remotes) {
        Remote& r = pair.second;
        playDue(r, t); // next points of their replayed path
        if (!r.queue.empty()) {
            stillNeeded = true;
        }
        if (r.hidden || r.drawnPage < 0) {
            continue;
        }
        const QRectF before = r.drawnRect.isEmpty() ? rectFor(r) : r.drawnRect;
        bool changed = false;

        // Glide: cover `step` of the remaining distance (eases in as it nears).
        if (r.drawnPage == r.page && !(r.drawnPos == r.pos)) {
            if (screenDistance(r.drawnPos, r.pos) <= SETTLE_DISTANCE_PX) {
                r.drawnPos = r.pos;
            } else {
                r.drawnPos = r.drawnPos + (r.pos - r.drawnPos) * step;
            }
            changed = true;
        }

        // Fade out after a while without moving.
        const qint64 idle = now - r.lastMoveMs;
        const double opacity = idle <= IDLE_FADE_AFTER_MS
                               ? 1.0 : std::max(0.0, 1.0 - double(idle - IDLE_FADE_AFTER_MS) / FADE_DURATION_MS);
        if (opacity != r.opacity) {
            r.opacity = opacity;
            changed = true;
        }

        if (changed) {
            markDirty(before);
            markDirty(rectFor(r));
        }
        if (r.opacity > 0.0) {
            stillNeeded = true; // gliding, or waiting to fade
        }
    }
    if (!stillNeeded) {
        m_animation.stop();
    }
}

void CursorLayer::onPresence(const QJsonArray& peers)
{
    // The list includes us, so anyone else means more than one entry.
    const bool othersPresent = peers.size() > 1;
    if (othersPresent && !m_othersPresent) {
        m_lastSentPage = -1; // newcomer: next move is sent even if we haven't moved since
    } else if (!othersPresent) {
        dropBatch();
        m_shownToOthers = false;
    }
    m_othersPresent = othersPresent;

    std::map<int, Remote> kept;
    for (const QJsonValue& v : peers) {
        const int id = v.toObject().value("id").toInt(-1);
        auto it = m_remotes.find(id);
        if (it != m_remotes.end()) {
            kept.insert(*it);
        }
    }
    for (const auto& pair : m_remotes) {
        if (kept.find(pair.first) == kept.end()) {
            markAllDirty(); // erase cursor and selection of people who left
        }
    }
    m_remotes.swap(kept);
}

void CursorLayer::clear()
{
    m_remotes.clear();
    m_othersPresent = false; // until the next welcome/presence says otherwise
    m_lastSentPage = -1;
    m_shownToOthers = false;
    dropBatch();
    markAllDirty();
}

// ---------------------------------------------------------------------------
// Repainting
// ---------------------------------------------------------------------------

void CursorLayer::markDirty(const QRectF& rect)
{
    if (rect.isEmpty()) {
        return;
    }
    m_dirty = m_dirty.isEmpty() ? rect : m_dirty.united(rect);
    if (!m_repaint.isActive()) {
        m_repaint.start();
    }
}

void CursorLayer::markAllDirty()
{
    m_fullRepaint = true;
    if (!m_repaint.isActive()) {
        m_repaint.start();
    }
}

void CursorLayer::flushRepaint()
{
    m_views.erase(std::remove_if(m_views.begin(), m_views.end(),
                                 [](const QPointer<QQuickPaintedItem>& v) { return v.isNull(); }), m_views.end());

    // Partial repaint only when we know exactly which canvas we drew on.
    const bool partial = !m_fullRepaint && m_views.size() == 1;
    for (const QPointer<QQuickPaintedItem>& view : m_views) {
        if (partial) {
            view->update(m_dirty.toAlignedRect());
        } else {
            view->update();
        }
    }
    m_dirty = QRectF();
    m_fullRepaint = false;
}

void CursorLayer::paint(Painter* painter, QQuickPaintedItem* view)
{
    // Remember this canvas so later cursor moves can repaint it.
    if (view && std::none_of(m_views.begin(), m_views.end(),
                             [view](const QPointer<QQuickPaintedItem>& v) { return v == view; })) {
        m_views.emplace_back(view);
    }

    // The painter currently maps score units → painter coords (our zoom and
    // scroll). Keep it so cursor moves can work out which strip to repaint.
    m_lastToPainter = painter->worldTransform();
    // MuseScore's GUI scale (UiConfiguration::guiScaling): the screen's device pixel ratio. Not the window's:
    // MuseScore paints in device pixels and its window reports 1, so on a Retina Mac (2) name tags, arrows
    // and padding came out at half size.
    const QScreen* screen = (view && view->window()) ? view->window()->screen() : nullptr;
    m_lastDpr = screen ? screen->devicePixelRatio() : 1.0;
    m_haveTransform = true;

    if (!m_fpsLogPath.isEmpty()) {
        ++m_fpsPaints;
        const qint64 now = cursorNowMs();
        if (m_fpsWindowStart == 0) {
            m_fpsWindowStart = now;
        } else if (now - m_fpsWindowStart >= 1000) {
            QFile f(m_fpsLogPath);
            if (f.open(QIODevice::Append | QIODevice::Text)) {
                const double hz = (view && view->window() && view->window()->screen()) ? view->window()->screen()->refreshRate() : 0.0;
                f.write(QString("%1 paints/s (timer %2 ms, screen %3 Hz)\n")
                        .arg(m_fpsPaints * 1000.0 / (now - m_fpsWindowStart), 0, 'f', 0)
                        .arg(m_animation.interval()).arg(hz, 0, 'f', 0).toUtf8());
            }
            m_fpsPaints = 0;
            m_fpsWindowStart = now;
        }
    }

    if (!m_score || m_remotes.empty()) {
        return;
    }

    const std::vector<Page*>& pages = m_score->pages();
    const double s = m_lastDpr;

    // Name tags: Inter Light (bundled, see collabmodule.cpp), thin and clean.
    Font font;
    font.setFamily(Font::FontFamily(u"Inter"), Font::Type::Text);
    // 12 px (times the GUI scale), given in points the way MuseScore's painter expects: Painter::drawText
    // rescales the point size by 1200 / the device's DPI (applyFontSizeScaling) and FontMetrics measures at
    // 1200 DPI, so P * 72 / 1200 points come out as P pixels in both, on any screen. A pixel size was wiped
    // by that rescale and the text fell back to the default UI size: fine on Windows at 100%, tiny on a
    // Retina Mac, and no longer the size the tag boxes were measured for.
    font.setPointSizeF(12.0 * s * 72.0 / 1200.0);
    font.setWeight(Font::Light);
    const FontMetrics fm(font);

    // Draw in plain painter pixels so arrows and name tags stay the same
    // size at any zoom.
    painter->save();
    painter->setWorldTransform(Transform());
    painter->setAntialiasing(true);
    painter->setFont(font);

    // Collaborators' selections first, so cursors are drawn on top. Boxes
    // scale with zoom (they outline music), the border stays 2px.
    for (const auto& pair : m_remotes) {
        const Remote& r = pair.second;
        if (r.selection.empty()) {
            continue;
        }
        Color fill = r.color;
        fill.setAlpha(60);
        PointF firstBoxTopLeft;
        bool haveFirst = false;
        for (const auto& [pageIdx, rel] : r.selection) {
            if (pageIdx < 0 || static_cast<size_t>(pageIdx) >= pages.size()) {
                continue;
            }
            const PointF pagePos = pages[pageIdx]->pos();
            const PointF tl = m_lastToPainter.map(pagePos + rel.topLeft());
            const PointF br = m_lastToPainter.map(pagePos + rel.bottomRight());
            const double pad = 2 * s;
            const RectF box(tl.x() - pad, tl.y() - pad, br.x() - tl.x() + 2 * pad, br.y() - tl.y() + 2 * pad);
            painter->setPen(Pen(r.color, 2 * s));
            painter->setBrush(Brush(fill));
            painter->drawRoundedRect(box, 3 * s, 3 * s);
            if (!haveFirst) {
                firstBoxTopLeft = box.topLeft();
                haveFirst = true;
            }
        }

        // Whose selection it is: a name tag sitting on top of the first box.
        if (haveFirst) {
            const String name = String::fromQString(r.name.isEmpty() ? QString("Guest") : r.name);
            const double h = fm.height() + 6 * s;
            const RectF tag(firstBoxTopLeft.x(), firstBoxTopLeft.y() - h - 2 * s, 6 * s + fm.width(name) + 6 * s, h);
            painter->setNoPen();
            painter->setBrush(Brush(r.color));
            painter->drawRoundedRect(tag, 4 * s, 4 * s);
            painter->setPen(Pen(Color(255, 255, 255)));
            painter->drawText(PointF(tag.left() + 6 * s, tag.top() + 3 * s + fm.ascent()), name);
        }
    }

    // Cursor name tags: same font, tight padding (Google Slides style).
    const Font tagFont = font;
    const FontMetrics tagFm(tagFont);
    painter->setFont(tagFont);

    for (auto& pair : m_remotes) {
        Remote& r = pair.second;
        if (r.hidden || r.opacity <= 0.0 || r.drawnPage < 0 || static_cast<size_t>(r.drawnPage) >= pages.size()) {
            r.drawnRect = QRectF();
            continue;
        }
        const PointF at = m_lastToPainter.map(pages[r.drawnPage]->pos() + r.drawnPos);
        const double x = at.x(); // not rounded: rounding makes a glide step in whole pixels
        const double y = at.y();
        const int alpha = static_cast<int>(std::round(255 * r.opacity));
        Color fill = r.color;
        fill.setAlpha(alpha);
        const Color white(255, 255, 255, alpha);

        // Arrowhead pointing up-left, tip exactly at the cursor spot, with a
        // thin white edge so it shows on dark notes too.
        const PointF arrow[] = {
            PointF(x, y), PointF(x + 12.5 * s, y + 5.2 * s), PointF(x + 6.2 * s, y + 6.2 * s), PointF(x + 5.2 * s, y + 12.5 * s),
        };
        painter->setPen(Pen(white, 1.2 * s));
        painter->setBrush(Brush(fill));
        painter->drawPolygon(arrow, std::size(arrow));

        // Name tag in their colour, just below-right of the arrow.
        const String name = String::fromQString(r.name.isEmpty() ? QString("Guest") : r.name);
        const RectF tag(x + TAG_X * s, y + TAG_Y * s, tagFm.width(name) + 12 * s, tagFm.height() + 5 * s);
        painter->setNoPen();
        painter->setBrush(Brush(fill));
        painter->drawRoundedRect(tag, 3 * s, 3 * s);
        painter->setPen(Pen(white));
        painter->drawText(PointF(tag.left() + 6 * s, tag.top() + 2.5 * s + tagFm.ascent()), name);

        r.tagWidth = tag.width() / s;
        r.tagHeight = tag.height() / s;
        r.drawnRect = rectFor(r);
    }

    painter->restore();
}
