# MAGPIE × Woogles Integration Plan

Forward-looking plan for three Woogles-integration features. **Work begins
June 2026** — Phase 1 (Causeway commentary, May 29) and Phase 2 of the
existing `PLAN.md` (human-vs-computer + streaming readiness, through Jul 3)
take priority until then. Activity here is background-priority through
Jul 3 and primary after that.

See `tui/NOTES_woogles_integration.md` for the technical investigation
this plan is built on, and `tui/NOTES_xtables_search.md` for the related
cross-tables search work.

## Three tracks

| # | Track                                | Cost     | Ship target            |
| - | ------------------------------------ | -------- | ---------------------- |
| 1 | `GetGameHistory` adapter (loaded games) | 1-2 days | Mid-June 2026          |
| 2 | MAGPIE as a Woogles bot              | 2-4 wks  | Late July / August 2026|
| 3 | TUI live-play as a human user        | 4-8 wks  | Q4 2026                |

Each track is independently shippable. Each later track reuses the
plumbing from the earlier one — protobuf scaffolding from track 1, the
auth + bot infra from track 2, etc.

## Sequencing rationale

- **Causeway (Phase 1)** must ship first (May 29). No Woogles work
  starts before then.
- **Phase 2** in `PLAN.md` (human-vs-computer + stream readiness,
  May 30 → Jul 3) is unrelated and stays on its own track. It does NOT
  depend on any of the Woogles tracks below. Treat Phase 2 as priority
  through the Japan travel cutoff.
- **Japan travel: Jul 4 → mid-Jul.** No active development.
- Real Woogles work ramps mid-July, after travel.
- **Track 1** can land any time in June as a side project — it's
  small and self-contained.

---

## Track 1 — `GetGameHistory` adapter for loaded games

**Goal:** when loading a Woogles GCG, route through `GetGameHistory`
(structured JSON) instead of `GetGCG` (text). Frees us from regex move
parsing, gives us per-event `millis_remaining` so the History panel's
clocks reflect reality on loaded games.

### Done criteria

- Load a Woogles game by URL or game ID: TUI shows real per-move
  clocks (using `clock_at_start` / `clock_at_end` already plumbed).
- Loaded GCGs from `.gcg` files still work via the existing path.
- Source detection: alphanumeric Woogles ID → JSON path;
  all-digits cross-tables ID → GCG path; local file → GCG path.

### Day-by-day (target week: **Jun 8-12**)

#### Mon Jun 8 — home eve

- [ ] Add a small JSON parser dependency or hand-roll one for the
      ~10 fields we touch (`events[*]`, `players[*]`, `lexicon`,
      `final_scores`, etc.).
- [ ] Write `get_woogles_game_history(...)` mirroring
      `get_woogles_gcg` in `get_gcg.c`, hitting
      `GameMetadataService/GetGameHistory`.

#### Tue Jun 9 — phone-only (background agent)

- [ ] Agent: write the macondo-event → TUI-history-entry converter.
      Input: parsed JSON event. Output: `TuiHistoryEntry` with
      `move_str`, `score`, `total_after`, `clock_at_start`,
      `clock_at_end` (derived from sibling event's
      `millis_remaining`), `rack_str`, `leave_str`.

#### Wed Jun 10 — home eve

- [ ] Wire the LOAD_GAME modal to detect Woogles IDs vs paths and
      pick the right backend.
- [ ] Verify per-turn snapshots still work — the three-pass replay
      from the GCG path needs the same `parsed_meta` array; can
      we share?

#### Thu Jun 11 — phone-only

- [ ] Agent: 20-GCG regression test. Load each via both paths
      (GCG and GameHistory) when possible; assert TUI state
      matches (move list, scores, final racks).

#### Fri Jun 12 — buffer

- [ ] Polish; ship behind a settings toggle initially.

---

## Track 2 — MAGPIE as a Woogles bot

**Goal:** register MAGPIE as a Woogles bot user. Receive `BotRequest`
{ macondo.GameHistory + rack }, return `BotResponse` { GameEvent }.
Plays alongside BestBot / HastyBot / STEEBot in the Woogles ecosystem.

### Done criteria

- A new binary `bin/magpie_bot` that connects to NATS (or AWS Lambda)
  and serves bot requests.
- Picks moves using MAGPIE's existing static-eval + sim path.
- Configurable bot type (sim depth, time per move).
- Logs every game it plays, makes the GCG/GameHistory archive
  recoverable for later study.
- Registered with the Woogles team; "MagpieBot" account live and
  playable.

### Phase 2A — protobuf + adapter (target: **Jul 15-22**)

#### Week of Jul 13 — post-Japan ramp

- [ ] Decide: `protoc-c` codegen vs. hand-rolled encoders. The
      schemas we touch are small (`BotRequest`, `BotResponse`,
      `macondo.GameHistory`, `macondo.GameEvent`); hand-rolling
      avoids a build-time dep, but generated code is more robust.
- [ ] Implement the macondo `GameHistory` → MAGPIE `Game` adapter.
      Same converter as track 1, refactored to a shared helper.
- [ ] Implement the reverse: MAGPIE chosen `Move` → macondo
      `GameEvent` proto.

### Phase 2B — bot loop (target: **Jul 23 - Aug 5**)

#### Week of Jul 20

- [ ] Set up a NATS subject locally. Stand up a local liwords (docker)
      if needed to test the wire format.
- [ ] Write `cmd/magpie_bot/main.c`: subscribe to
      `bot.commands.magpie`, decode `BotRequest`, run sim with a
      time budget, encode `BotResponse`, publish.
- [ ] Stateless verification: send the same request twice, ensure
      identical (or seeded-distinct) responses.

#### Week of Jul 27

- [ ] AWS Lambda variant: package as a Lambda function, receive
      `{ CGP, BotType, GameID, ReplyChannel }`. Same internal flow,
      different transport.
- [ ] Time-control awareness: the bot has only ~5-30s per move in
      typical games. Cap sim iterations, fall back to static eval
      under deadline pressure.

### Phase 2C — registration + production (target: **Aug 6-15**)

- [ ] Coordinate with liwords team for a bot account
      ("MagpieBot"?). Need someone with admin rights to set
      `is_bot=true` and provision the bot's NATS subject or
      Lambda ARN.
- [ ] Self-play test against a Woogles staging instance if one
      exists; otherwise live-test with a friendly opponent.
- [ ] Public ship. Announce.

---

## Track 3 — TUI live-play as a human user

**Goal:** TUI user logs into their own Woogles account; seeks or
accepts games against humans; plays them through the TUI.

### Done criteria

- Login from the TUI with username + password (or token).
- Lobby panel: list of open seeks, ability to create a seek.
- Accept a seek; play the game through to completion.
- Opponent moves arrive in real time and update the TUI.
- Time controls honored.
- Game ends cleanly — score adjustment, end-rack penalty, "back to
  lobby."

### Out of scope for v1

- Tournament play.
- Correspondence games.
- Chat UI (status-bar ticker is the v1 surface).
- Rematch flow.
- Draw / abort negotiation.
- Direct match requests (lobby-only).

### Phase 3A — auth + websocket (target: **Sep 1-14**)

- [ ] `Login(UserLoginRequest)` round-trip; store cookie / token.
- [ ] `GetSocketToken` round-trip; obtain JWT.
- [ ] Websocket client: TLS, framing, reconnect-on-disconnect with
      cached token. Library choice: `libwebsockets`? Hand-rolled?
      Decide here.
- [ ] Hello round-trip with the lobby realm: send
      `RegisterRealmRequest`, receive `SEEK_REQUESTS` /
      `MATCH_REQUESTS` / `USER_PRESENCES`.

### Phase 3B — lobby UI (target: **Sep 15-28**)

- [ ] New modal: lobby panel. Two columns: open seeks, your seeks.
- [ ] Create-seek modal: time control, lexicon, rated/casual.
      Posts `SEEK_REQUEST`.
- [ ] Accept a seek: subscribe to that game's realm.
- [ ] On `NEW_GAME_EVENT`, initialize a game state mirroring the
      bot worker's "live game" mode.

### Phase 3C — playing a game (target: **Sep 29 - Oct 19**)

- [ ] Receive `GAME_HISTORY_REFRESHER` on game start, populate the
      TUI's history.
- [ ] Submit moves via `CLIENT_GAMEPLAY_EVENT` (`TILE_PLACEMENT`,
      `PASS`, `EXCHANGE`, `CHALLENGE_PLAY`, `RESIGN`).
- [ ] Apply received `SERVER_GAMEPLAY_EVENT` / `SERVER_OMGWORDS_EVENT`
      to local state. Hide opponent rack until end-of-game.
- [ ] Server clocks: subscribe to clock-tick events (or compute
      locally between `LAG_MEASUREMENT`s).
- [ ] Challenge: on opponent move, surface a "Challenge?" prompt
      with a countdown matching the server's challenge timer.
- [ ] `GAME_ENDED_EVENT` flow: surface result, prompt to return to
      lobby.

### Phase 3D — edge cases + polish (target: **Oct 20 - Nov 9**)

- [ ] Reconnect handling: if the websocket drops mid-game, fetch a
      fresh `SocketToken`, resubscribe to the game realm, replay
      `GAME_HISTORY_REFRESHER`.
- [ ] `LAG_MEASUREMENT` pings (server probes every ~5s).
- [ ] `TIMED_OUT` event handling.
- [ ] Status-bar chat ticker: render incoming `CHAT_MESSAGE`s; send
      via a slash command (`/chat <text>`).
- [ ] Streaming polish: same "stream mode" layout the existing
      Phase 2 added, with opponent's name + rating visible.

---

## Risk + decision log (pre-flight)

### Decisions to make before track 2 starts

1. **NATS vs. Lambda.** Lambda is the newer path Woogles uses; NATS
   is what `cmd/bot` in macondo uses. The Lambda payload is simpler
   (just CGP); NATS gives the full `GameHistory`. NATS keeps the bot
   resident, Lambda is per-invocation. Probably **Lambda** — smaller
   bot, no daemon, easier to deploy.
2. **Bot account naming + provisioning.** "MagpieBot"? Who owns it on
   the Woogles side? Need to talk to Cesar early.
3. **Static eval vs. sim per move.** Sim is the differentiator —
   even a 1-2-second sim per move would be a big upgrade over the
   `equity` static eval. Decision lives in the move-loop budget.

### Decisions to make before track 3 starts

1. **Library choices.** Websocket: `libwebsockets` (cleanest, ~big),
   `libwebsockets-tiny`, or hand-rolled (RFC 6455 isn't huge but
   does have several gotchas). Protobuf: `protoc-c`, `nanopb`, or
   hand-rolled.
2. **Bracketed-paste-style problems.** macOS Terminal may interfere
   with the TUI's TLS websocket reconnect logic when the screen
   redraws on focus events — keep notes.
3. **Server-side rate limits.** No docs; assume the equivalent of
   "a normal-paced human player" and don't open multiple sockets.
4. **Concurrency model.** Should the websocket run on the bot worker
   thread, or a new IO thread? Probably new thread; we don't want
   sim contention with network I/O.

### Risks

- **liwords protocol evolves.** Pinning to a specific version is
  realistic; tracking head is not.
- **NATS auth.** Bot subject access needs credentials Woogles
  provides. Long-tail coordination.
- **Mac/Linux build differences.** Both protobuf and websocket libs
  have C library variants; pick ones that build cleanly on both.

---

## Open questions

- **Login UX in the TUI.** Modal with username + password is the
  natural shape — but storing the password is dicey. Probably keep a
  session token on disk under `~/.config/magpie/`. Refresh via
  `GetSignedCookie` when stale.
- **Single-user vs. multi-account.** First version assumes one
  account per TUI install. Account switching would be a v2 feature.
- **Game-end disposition for the bot.** If MAGPIE-the-bot crashes
  mid-game, the server presumably times the bot out. Make sure crash
  recovery on next start handles that gracefully and we don't
  re-enter a broken game state.
- **Public deployment.** Where does the bot run? Macondo runs theirs
  on AWS. We'd want similar — possibly piggybacking on Woogles' own
  Lambda infra if they'd host it.

---

## Cut list (if behind schedule)

In order of what gets dropped first:

1. **Stream-mode polish** for the human-play TUI (track 3D).
2. **Chat surface** entirely (track 3D).
3. **Lambda variant** of the bot (track 2B) — NATS-only is fine.
4. **GameHistory adapter for cross-tables games** (track 1, scope
   reduction) — keep cross-tables on the GCG path indefinitely.

If both tracks 2 and 3 slip, **track 2 wins** — a working bot is the
higher-leverage deliverable.

---

## Pre-June checklist

Items to complete before June 1 so track 1 can start cleanly:

- [ ] Confirm Phase 1 (Causeway) ships May 29.
- [ ] Skim the macondo `GameHistory` proto into MAGPIE's `Game`
      adapter so the converter idea is concrete.
- [ ] Open a Woogles account if we don't have one (for testing).
- [ ] Send Cesar a heads-up that MagpieBot is on the way and ask
      about deployment options.
