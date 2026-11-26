# Collab sync protocol

JSON text messages over one WebSocket per person. The server holds one **room per score**
(`ws://host:port/<room>`) and decides the order everyone applies edits in.

Servers:
- **`wss://collab.ploq.app/room/<room>`** — Cloudflare Worker + one Durable Object per room, in the
  **`ploq-server`** repo (moved out of this repo; it was `tools/collab/server/`). Room names must be 16–128 chars of `A-Z a-z 0-9 _ -` (launchers use 32
  random hex chars). Keeps the room's score and edit log.
- `ws://127.0.0.1:8787/<room>` — local Node relay, `ploq-server/relay/server.mjs` (Phases 3–5;
  no score storage). Both refuse connections that carry an `Origin` header (web pages).

| Message | Direction | Sent when | Fields |
|---|---|---|---|
| `join` | client → server | right after connecting | `name`, `color` |
| `welcome` | server → joiner | reply to `join` | `clientId`, `seq` (edits so far), `peers` |
| `presence` | server → others | someone joins or leaves | `peers: [{id, name, color}]` |
| `edit` | client → server | an edit, undo or redo finishes | `editId`, `tickFrom`, `tickTo`, `staffFrom`, `staffTo`, `xml`, `sentAtMs`, `structSeq`, `baseSeq` |
| `edit` | server → others | forwarded | same + `seq` (server order), `from` (client id), `name` |
| `ack` | server → sender | edit accepted | `seq`, `editId` |
| `cursor` | client → others (relayed) | mouse moves (≤ ~120/s) | `page`, `x`, `y` (page-relative score units), `color`; or `hidden` |
| `selection` | client → others | selection changes | `outlines: [[page, x, y, w, h], …]`, `color` |
| `edit-rejected` | server → sender | 2000 edits stored without a fresh snapshot | `editId`, `reason` |
| `snapshot-put` | client → server | joining an empty room; when asked | `baseSeq`, `title`, `data` (base64 .mscz) |
| `snapshot-saved` / `snapshot-refused` | server → sender | reply | `baseSeq` |
| `snapshot-request` | server → a client | edit log ≥ 300 since the snapshot | — |
| `sync` | client → server | after `welcome` | `sinceSeq` (-1 = I have nothing) |
| `snapshot` | server → client | `sync` from before the stored score | `baseSeq`, `title`, `data` |
| `synced` / `no-snapshot` | server → client | end of a `sync` | `seq` |

`welcome` from the Cloudflare server also carries `snapshot: {exists, baseSeq, title}`.

Until 2026-11-25 there were measure locks (`lock`, `locks`, `lock-denied`, and `edit-rejected` with
`by`). Servers now ignore `lock` from older apps and never send the others.

## `edit` fields

- `tickFrom` / `tickTo` — start of the first changed measure / end of the last one (exclusive),
  in MuseScore ticks. Always whole measures.
- `staffFrom` / `staffTo` — first staff / one past the last staff (0 = top staff).
- `xml` — those measures in MuseScore's clipboard format (`<StaffList>`), produced by
  `Selection::mimeData()` with an all-pass selection filter.
- `sentAtMs` — sender's clock when sent; receivers log `now - sentAtMs` as latency (only meaningful
  when both sides share a clock, i.e. the same machine).
- `baseSeq` — the newest room edit the sender's score contained when it made this one.

## Two people, one measure: last write wins

There are no locks: anyone can edit any measure, and everyone's selection shows as a highlight with
their name. The server numbers edits (`seq`) and everyone applies them in that order, so when two
people change the same measure at once, the later-numbered edit wins on every copy.

Each app already has its own edits in its score before the server numbers them. So when it applies
a collaborator's edit #N, it puts its own edits back on top that the server numbered after N (or
hasn't acknowledged yet: they will be), from the first one that overlaps #N, in order. The server
sends every edit numbered before an app's own edit ahead of that edit's `ack` on the same
connection, so an app knows which of its edits come after.

If #N overlaps an edit of yours numbered between its `baseSeq` and N, its sender hadn't seen yours
when they made theirs: the app shows "<name> changed measure M at the same time as you. Their
version was kept." Edits that have been sent but not acknowledged when the connection drops are
sent again (as the measures are then) after reconnecting.

The tradeoff: two different changes in the same measure and staff within the same moment keep only
the later one.

The receiver swaps the measures in with `Score::pasteStaff()` as **one undo step** named
"Edit by collaborator", then clears any voice that is empty in the XML.

## Known limits (Phase 3)

- Measure-level changes (adding/removing measures, time signature changes, instruments) aren't
  carried by a staff paste. Both scores must have the same measures and staves.
- Late join (Phase 6): joiners start MuseScore with **no score open**; the room's score is
  downloaded and opened, then the edits since are replayed. Replays are safe because every edit
  replaces whole measures (applying one twice gives the same result).
- No authentication until Phase 7: anyone who knows a room name can join it.

## Relaying rule (security)

The Cloudflare server passes on **only** `cursor` and `selection` from one client to the others.
Every other message type a client sends is either handled by the server (`join`, `edit`,
`structure`, `snapshot-put`, `sync`) or dropped, so members can't forge server messages such as
`snapshot`, `welcome` or `presence`. Edits are refused (`edit-rejected`) only when 2000 edits are
stored without a fresh snapshot.
