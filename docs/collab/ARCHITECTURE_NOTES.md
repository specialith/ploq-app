# How MuseScore works (notes for the collab fork)

Plain-English notes on the parts of MuseScore Studio that collaboration hooks into.
Traced on branch `collab` = upstream tag **v4.7.7**. Every class/function/line number here was
found by searching the repo, not guessed. Line numbers will drift when we merge upstream; the
function names are the stable reference.

## Repo layout (top level)

- `src/` — the app.
  - `src/engraving/` — the score itself: the data model (`dom/`), editing commands
    (`editing/`), file reading/writing (`rw/`), layout.
  - `src/notation/` — the "editing session" layer that sits between the UI and the score
    (note input, undo stack wrapper, selection, interaction).
  - `src/notationscene/` — the score canvas you see and the keyboard/menu action handlers.
  - `src/context/` — "what's open right now" (current project / score).
  - `src/app/appfactory.cpp` — the list of modules the app loads at startup.
  - `src/framework/` — shared "Muse" framework (actions, shortcuts, UI, network...).
- `build.cmake` — the official one-command build script.
- `tools/codestyle/uncrustify_musescore.cfg` — code style config (rule 4).

A few words you'll see everywhere:
- **Tick** — a position in time in the score (like a timestamp in music units). Measures, notes,
  everything has a tick. Converting tick → measure: `Score::tick2measure(tick)`.
- **Staff index** — which staff (line of music) counting from the top, starting at 0.
- **Track** — staff index × 4 + voice (each staff has 4 voices).
- **Logical coordinates** — positions on the score canvas itself, independent of zoom/scroll.

---

## 1. Adding a note: keypress → redraw

What happens when you press **C** in note-input mode, in order:

| # | Step | Where |
|---|---|---|
| 1 | Qt sees the key; the QML shortcut handler fires | `src/framework/shortcuts/qml/Muse/Shortcuts/Shortcuts.qml:58` → `ShortcutsInstanceModel::activate()` (`shortcutsinstancemodel.cpp:54`) |
| 2 | Key sequence is turned into an action name (`"note-c"`) and dispatched | `ShortcutsController::activate()` — `src/framework/shortcuts/internal/shortcutscontroller.cpp:37` → `dispatcher()->dispatch(actionCode)` |
| 3 | The notation controller has registered `"note-c"` | `NotationActionController::init` — `src/notationscene/internal/notationactioncontroller.cpp:127` (`registerNoteAction("note-c", NoteName::C)`) |
| 4 | Works out which pitch/where, then hands off | `NotationActionController::handleNoteAction()` — same file, lines 787 and 802 → `noteInput->addNote(params, addingMode)` |
| 5 | **The edit itself** | `NotationNoteInput::addNote()` — `src/notation/internal/notationnoteinput.cpp:438` |
| 5a | opens an undo step | `startEdit("Enter note")` → `NotationUndoStack::prepareChanges()` → **`Score::startCmd()`** |
| 5b | changes the score | `score()->cmdAddPitch(params, ...)` |
| 5c | closes the undo step | `apply()` → `NotationUndoStack::commitChanges()` → **`Score::endCmd()`** |
| 5d | asks for a redraw | `notifyNoteAddedChanged()` → `Notation` (`src/notation/internal/notation.cpp:64`) → `notationChanged()` |
| 6 | Canvas schedules a repaint | `AbstractNotationPaintView` subscribes to `notationChanged()` (`abstractnotationpaintview.cpp:257`) → `scheduleRedraw()` |
| 7 | Repaint | `AbstractNotationPaintView::paint()` (`abstractnotationpaintview.cpp:662`) — see section 4 |

Every other kind of edit (delete, paste, palette drop, inspector change...) follows the same
pattern: *something* calls `startCmd`, changes the score, calls `endCmd`.

## 2. Edits and undo: where one edit starts and ends

All in `src/engraving/editing/cmd.cpp`:

- **`Score::startCmd(actionName)`** (line 344) — opens one user-visible undo step
  (`undoStack()->beginMacro(...)`). Everything changed until `endCmd` lands in that one step.
- **`Score::endCmd(rollback, layoutAllParts)`** (line 406) — re-lays-out the score
  (`update()`), closes the undo step (`undoStack()->endMacro(...)`), and then — **if something
  actually changed** — sends a `ScoreChanges` message on **`changesChannel()`** (line 445).
- **`Score::undoRedo(undo, editData)`** (line 372) — runs undo or redo and **also sends a
  `ScoreChanges` on `changesChannel()`** (line 397).

**`ScoreChanges`** (`src/engraving/dom/score.h:267`) describes one finished edit:
`tickFrom`, `tickTo` (time range), `staffIdxFrom`, `staffIdxTo` (staff range), `isTextEditing`,
plus the set of changed objects / element types / properties / styles. The tick range is the
min/max over every object the edit touched (`buildScoreChanges`, `cmd.cpp:125`).

How the UI layer wraps it: `NotationUndoStack` (`src/notation/internal/notationundostack.cpp`)
— `prepareChanges()` (line 106) = `startCmd`, `commitChanges()` (line 132) = `endCmd`,
`rollbackChanges()` = `endCmd(true)`, and `changesChannel()` (line 250) forwards the score's
channel. Reached from a notation via `INotation::undoStack()` (`src/notation/inotation.h:100`).

Other code already listens to this channel, e.g. `MasterNotation` (`masternotation.cpp:140`)
and `NotationParts` (`notationparts.cpp:330`). **That's our edit hook:** subscribe to
`changesChannel()` from our own module — no upstream file has to change to *detect* edits, and
undo/redo arrive through the same channel ("undo syncs like any other edit").

**Gotchas for Phase 3:**
- When *we* apply a remote edit we'll call `startCmd`/`endCmd` too, so our own listener will fire
  again. We need an "applying remote edit" flag to avoid echoing it back.
- `endCmd` does **not** by itself trigger a redraw; each caller notifies `notationChanged()`
  (see `Notation` constructor, `notation.cpp:64-90`). After applying a remote edit we must call
  `notation->notationChanged().notify()` ourselves.
- `startCmd` is ignored if an undo step is already open (`hasActiveCommand()`), and both are
  no-ops while the undo stack is locked. Remote edits must wait until the local user isn't
  mid-edit.

## 3. Saving measures as XML

There are two XML writers. Both are measure/tick-range based.

**a) Clipboard format (best fit for "send changed measures"):**
- **Write:** `Selection::staffMimeData()` — `src/engraving/dom/select.cpp:1077`. Produces
  `<StaffList tick=".." len=".." staff=".." staves="..">` with one `<Staff>` per staff, and uses
  `RWRegister::writer()->writeSegments(...)` for the content. It's what copy (Ctrl+C) produces.
- **Read back:** `Score::pasteStaff(XmlReader&, Segment* dst, staff_idx_t, Fraction scale)` —
  `src/engraving/dom/paste.cpp:75` → `Read460::pasteStaff()` —
  `src/engraving/rw/read460/read460.cpp:335` (the current file-format reader; picked in
  `src/engraving/rw/rwregister.cpp:56`).
- `pasteStaff` **replaces** content: for each voice present in the XML it first clears that
  range with `Score::makeGap1()` (read460.cpp:450), then reads the notes in. Running it
  between `startCmd`/`endCmd` makes it one undoable step — exactly what Phase 3 wants.
- **Caveats to handle in Phase 3:**
  - A voice that is *empty* in the incoming XML is not listed in `<voiceOffset>`, so it is
    **not cleared** on the receiving side. (E.g. remote user deletes everything in voice 2.)
    We'll need to clear all 4 voices of the range ourselves first.
  - `staffMimeData()` honours the user's **selection filter** (`selectionFilter()`); our code
    must build the XML with a full, unfiltered selection.
  - The reader rejects XML whose `version` attribute differs from `Constants::MSC_VERSION_STR`
    — both sides must run the same build (they will).
  - Measure-level things (time signature changes, adding/removing measures) aren't covered by a
    staff paste; v1 will need a separate path or a full-snapshot fallback for those.

**b) File format (what Save writes):**
- `MeasureWrite::writeMeasure(measure, xml, ctx, staff, ...)` —
  `src/engraving/rw/write/measurewrite.cpp:36`, called per staff by `StaffWrite::writeStaff()`
  (`src/engraving/rw/write/staffwrite.cpp`). Overall save: `MscSaver` (`rw/mscsaver.cpp`),
  `Writer` (`rw/write/writer.cpp`).
- Reading one measure: `MeasureRead::readMeasure()` — `src/engraving/rw/read460/measureread.cpp:67`.
- Verified in Phase 1 that a full load → save round-trip works on our build (74 `<Measure>`s,
  175 `<Note>`s from `demos/Amazing_grace.mscz`).

## 4. How the canvas draws (and where our overlay goes)

The canvas is `NotationPaintView` → `AbstractNotationPaintView`
(`src/notationscene/qml/MuseScore/NotationScene/abstractnotationpaintview.cpp`).

`AbstractNotationPaintView::paint(QPainter*)` (line 662), in order:
1. `paintBackground()` — the wallpaper.
2. `painter->setWorldTransform(m_matrix * guiScalingCompensation)` (line 682) — **`m_matrix` is
   the zoom + scroll.** After this, everything is drawn in logical (score) coordinates.
3. `notation()->painting()->paintView(...)` — the score itself.
4. Overlays, in the same logical coordinates: note-input cursor / ruler (lines 694-696), loop
   markers (`m_loopInMarker->paint`, `m_loopOutMarker->paint`, lines 700-701), continuous-view
   panel.

Existing overlay objects are members created in the constructor (lines 88-93: `PlaybackCursor`,
`NoteInputCursor`, `NotationRuler`, `LoopMarker`). **A remote-cursor / lock layer fits the same
pattern:** our own object, painted right after the loop markers. That needs one small
`// COLLAB:` hook in `paint()`.

Coordinate helpers on the same class:
- `toLogical(screenPoint)` (line 1455) = `m_matrix.inverted().map(p)` — screen → score.
- `fromLogical(scorePoint)` (line 1470) = `m_matrix.map(p)` — score → screen.

So a cursor sent as a logical point lines up automatically at any zoom/scroll, because the
painter's transform converts it. For the doc's "page + position" format:
`Score::searchPage(const PointF&)` (`src/engraving/dom/score.h:813`) finds the page under a
logical point and `Score::pageIdx(page)` (`score.h:843`) gives its number; position = point minus
the page's position. (To confirm when building Phase 4.)

Mouse movement: `AbstractNotationPaintView::hoverMoveEvent()` (line 1280) and
`mouseMoveEvent()` (line 1255) forward to `NotationViewInputController`; convert with
`toLogical(event->position())`.

## 5. Plugging in our own module

- Modules are registered in `src/app/appfactory.cpp`. GUI app: `AppFactory::newGuiApp()`
  (line 288), e.g. `app->addModule(new mu::notation::NotationSceneModule());` at line 410. The
  command-line converter app (`newConsoleApp`, line 544) has its own list — we don't need to be
  there. Adding our module = one `// COLLAB:` line here plus an `add_subdirectory` in
  `src/CMakeLists.txt` (notation/notationscene are added at lines 60/66).
- Finding the open score from inside our module: `IGlobalContext`
  (`src/context/iglobalcontext.h`) — `currentMasterNotation()` / `currentNotation()` and their
  `...Changed()` notifications.
- For Phase 5 (locks): `INotationInteraction::selectionChanged()` / `selection()`
  (`src/notation/inotationinteraction.h:84-86`) tells us which measures the user is in.

## 6. WebSocket support that already exists (checked, not assumed)

- **What it is:** two JavaScript-facing API objects for plugins/scripts, not C++ services:
  `MuseApi.Websocket` (a client: `open`, `send`, `onMessage`, `close`) in
  `src/framework/network/api/websocketapi.h/.cpp`, and `MuseApi.WebsocketServer`
  (`listen`, `send`, `onMessage`) in `websocketserverapi.h/.cpp`. They wrap Qt's `QWebSocket` /
  `QWebSocketServer`, and are registered in `NetworkModule::registerApi()`
  (`src/framework/network/networkmodule.cpp:56`).
- **Switch:** CMake option `MUSE_MODULE_NETWORK_WEBSOCKET` (default **OFF**,
  `src/framework/cmake/MuseDeclareOptions.cmake:73`). When ON it sets `QT_ADD_WEBSOCKET`
  (`SetupConfigure.cmake:369`), which makes `buildscripts/cmake/SetupQt6.cmake:79` link
  `Qt::WebSockets`. Upstream CI turns it on via `MUSESCORE_BUILD_WEBSOCKET=ON` →
  `ninja_build.sh:104`. **Our local build has it OFF** (`CMakeCache.txt`:
  `MUSE_MODULE_NETWORK_WEBSOCKET:BOOL=OFF`).
- **What it means for Phase 3:** we don't reuse the JS API objects. Our C++ module uses
  `QWebSocket` / `QWebSocketServer` directly (Qt 6.10.2 has the `qtwebsockets` module
  installed). We need `Qt::WebSockets` linked — either turn `MUSE_MODULE_NETWORK_WEBSOCKET` on
  in `build_overrides.cmake`, or have our module's CMake `find_package(Qt6 COMPONENTS WebSockets)`
  itself (preferred: doesn't depend on an upstream option).
