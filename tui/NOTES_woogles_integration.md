# Woogles integration — login & live play, bot API

Two related features, with very different cost profiles. Findings
based on the public liwords + macondo source trees (2026-05).

## TL;DR

| Feature                                  | Difficulty | Where the cost lives                       |
| ---------------------------------------- | ---------- | ------------------------------------------ |
| Browse / load Woogles GCGs               | Low        | Already done (`GetGCG` / `GetGameHistory`) |
| Login + read user's games as that user   | Low-Med    | One auth call + cookie/header plumbing     |
| **MAGPIE as a Woogles bot** (option 2)   | **Med**    | NATS or Lambda client + protobuf C codegen |
| **Play live from the TUI** (option 1)    | **High**   | Websocket + protobuf-over-binary-frames + a lobby/seek UI + chat surface + realtime state machine |

Recommendation: do the **bot** flow first. Bots talk over a clean
request/response protocol (`GameHistory → GameEvent`); the human-player
flow is a full realtime client.

---

## Option 1: TUI user logs in and plays live games

### Auth

`api/proto/user_service/user_service.proto` has the surface:

```
rpc Login(UserLoginRequest) returns (LoginResponse);
rpc GetSocketToken(SocketTokenRequest) returns (SocketTokenResponse);
rpc GetSignedCookie / InstallSignedCookie
rpc GetAPIKey(GetAPIKeyRequest) returns (GetAPIKeyResponse);
```

Login returns a session cookie. The TUI would then call
`GetSocketToken` to obtain the JWT used to authenticate the websocket.
All these endpoints are ConnectRPC over HTTPS — same shape as `GetGCG`
that we already call, just with credentials.

### Realtime protocol

Once authed, the client opens a websocket to woogles.io. Messages are
**protobuf bytes prefixed with a one-byte `MessageType`** (defined in
`api/proto/ipc/ipc.proto`):

```
enum MessageType {
  SEEK_REQUEST = 0;          // I want to play
  MATCH_REQUEST = 1;         // direct challenge
  SOUGHT_GAME_PROCESS_EVENT,
  CLIENT_GAMEPLAY_EVENT = 3, // my move
  SERVER_GAMEPLAY_EVENT,     // opponent move
  GAME_ENDED_EVENT,
  GAME_HISTORY_REFRESHER,
  NEW_GAME_EVENT,
  SERVER_CHALLENGE_RESULT_EVENT,
  CHAT_MESSAGE,
  TIMED_OUT,
  READY_FOR_GAME,
  LAG_MEASUREMENT,
  PROFILE_UPDATE_EVENT,
  ...  (~50 types total)
}
```

The move payload (`ClientGameplayEvent`):

```protobuf
message ClientGameplayEvent {
  enum EventType { TILE_PLACEMENT, PASS, EXCHANGE, CHALLENGE_PLAY, RESIGN }
  EventType type = 1;
  string    game_id = 2;
  string    position_coords = 3;   // "H8"
  bytes     machine_letters = 5;   // binary; .=playthrough, lowercase=blank
  repeated uint32 challenged_word_indices = 6;
}
```

### What we'd have to build in C

1. **Proto codegen.** liwords ships protobuf message definitions but no
   C plugin in their repo. We'd need `protoc-c` (or `nanopb`) to
   generate the message structs. Adds a build-time dep on `protoc`.
2. **Websocket client.** Notcurses has no networking; we'd need a small
   ws library (e.g. `libwebsockets` or a few hundred lines of hand-
   rolled framing). TLS via OpenSSL.
3. **HTTP client for auth.** We already shell out to `curl` in
   `get_xt_gcg` / `get_woogles_gcg` — fine for the login round-trips.
4. **State machine.** Receive `SERVER_GAMEPLAY_EVENT` / `GAME_HISTORY_REFRESHER`,
   apply to local game state, render in the existing TUI panels, accept
   user keystrokes and emit `CLIENT_GAMEPLAY_EVENT`. Handle clocks,
   challenges, time-outs, disconnects, reconnect.
5. **Lobby UI.** New modal/panel to browse open seeks, send a seek of
   your own, or accept a direct match. The lobby state itself streams
   over the same websocket as `SEEK_REQUESTS` / `MATCH_REQUESTS` /
   `SOUGHT_GAME_PROCESS_EVENT`.
6. **Chat surface.** `CHAT_MESSAGE` flows over the same socket and the
   web client surfaces it next to the game; minimum-viable would be a
   one-line ticker in the status bar, fully-featured would be a chat
   panel.
7. **Edge cases.** Reconnect token handling, lag pings, the
   `READY_FOR_GAME` handshake, draw offers / adjudications, abort
   requests, server time-outs, the post-game rematch flow.

### Effort estimate

~3-6 weeks of focused work for a "playable" v1 (single rated game,
no chat, no rematch). Probably 2-3 months for parity with the web
client (lobby + chat + draws + tournaments + correspondence games).

The realtime protocol surface is large and changes — pinning to a
specific liwords version is realistic; tracking head is not.

---

## Option 2: MAGPIE as a Woogles bot

### Architecture

From `cmd/bot/main.go` and `bot/interactions.md` in the macondo repo:

```
                NATS topic                       NATS topic
liwords  ── BotRequest{GameHistory} ──>  MAGPIE bot
liwords  <── BotResponse{GameEvent} ──   MAGPIE bot
```

The bot is **stateless**. Each turn the server sends a full
`GameHistory` proto + the bot's current rack; the bot answers with one
`GameEvent` (TILE_PLACEMENT / PASS / EXCHANGE / CHALLENGE).

Newer path (`pkg/bus/meseeks.go`): some bot invocations now go through
**AWS Lambda** instead of NATS. The Lambda event is:

```go
type LambdaEvent struct {
    CGP          string  // CGP encoding of the current position
    BotType      int     // SIMMING_BOT, HASTY_BOT, etc.
    ReplyChannel string  // NATS subject to reply to
    GameID       string
}
```

So a Lambda-deployed bot receives just a CGP string — even simpler.

The Macondo doc says the protocol is language-agnostic:

> while this particular bot happens to share a lot of board and game
> representation code with the liwords server it should be possible to
> write a bot in whatever language you choose, using the same messages
> for communication.

### What we'd have to build in C

1. **Proto messages** (`macondo.GameHistory`, `macondo.GameEvent`).
   MAGPIE already has the conceptual equivalents (`GameHistory`,
   `GameEvent`) — we'd need a thin proto-encode/decode layer to
   marshal them to/from the wire format. Could be hand-rolled (the
   `BotRequest` schema is small) or use `protoc-c`.
2. **NATS client** (or AWS Lambda runtime). NATS C client exists
   (`cnats`); a Lambda runtime in C is rarer but doable
   (`aws-lambda-cpp` or shell-wrap into Python).
3. **Bot entry point.** A new binary `bin/magpie_bot` that subscribes
   to a NATS subject (e.g. `bot.commands.magpie`), decodes the request,
   converts the macondo `GameHistory` into a MAGPIE `Game`, runs sim /
   endgame to pick a move, encodes the `GameEvent` reply, publishes.
4. **Registration with Woogles.** Bots are user accounts with
   `is_bot=true`. We'd need an account, an admin to flip the bit, and
   a NATS subject (or Lambda function) registered on Woogles' side.
   Not a code task — a coordination one with Cesar / the liwords
   maintainers.

### Effort estimate

~1-2 weeks for a Lambda or NATS bot that picks static-eval-best moves.
Add ~1 week for sim integration (use the bot worker's sim path
internally before responding). The hard parts (engine, lexicons, sim,
endgame) are already done.

### Why this is the right first step

- Reuses MAGPIE's existing strengths (engine quality, sim, endgame).
- Stateless request/response = no socket lifecycle, no reconnect, no
  chat, no lobby UI.
- Bots already exist in liwords' user model; we plug in beside
  BestBot/HastyBot/STEEBot.
- A working bot demonstrates everything needed for human play (auth,
  message encoding, game representation interop) at much lower stakes
  — the TUI human-play work can build on it.

---

## Shared prerequisites

Both options need:

- **Protobuf in C.** Either `protoc-c` codegen (clean but new build
  dep) or hand-rolled encoders for the handful of messages we touch
  (cheaper for the bot, painful for the realtime client).
- **Macondo ↔ MAGPIE GameHistory adapter.** Same conversion I sketched
  in `NOTES_xtables_search.md` for the `GetGameHistory` JSON path. The
  bot needs it on every request; the human client needs it on game
  start and on `GAME_HISTORY_REFRESHER`.
- **Account on Woogles.** For option 1, an end-user account. For
  option 2, a bot account that Woogles' team has registered for us.

---

## Recommendation

Ship in this order:

1. **`GetGameHistory` adapter for loaded games.** Already half-built —
   refactor the GCG-walker to optionally consume the JSON form.
   No new account, no new build deps. Buys real clocks for any
   Woogles-loaded GCG. **1-2 days.**
2. **MAGPIE bot.** Implement the `BotRequest → BotResponse` loop
   against a local NATS or via AWS Lambda. Register with Woogles.
   Cesar plays it; everyone plays it. **2-3 weeks.**
3. **TUI live player flow.** Build on the bot's proto layer + auth
   plumbing; add websocket, lobby, chat. **4-8 weeks for a polished
   v1.**

Each step is a usable artifact on its own and the work compounds.
