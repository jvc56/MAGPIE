# cross-tables.com search — endpoints and integration notes

Reverse-engineered URL surface for adding player and tournament search
to the MAGPIE TUI's load-game flow. cross-tables.com has no documented
JSON API; everything below is server-rendered HTML scraped by trying
queries against the live site (2026-05).

## Endpoint map

### Player search

| URL                                  | Purpose                                             |
| ------------------------------------ | --------------------------------------------------- |
| `players.php?query=<name>`           | Free-text player search. Returns an HTML list of hits.|
| `results.php?playerid=<id>`          | A player's tournament history.                      |
| `anno.php?p=<id>`                    | A player's annotated games. **Has the gid links.**  |

### Tournament search

| URL                                                         | Purpose                                                 |
| ----------------------------------------------------------- | ------------------------------------------------------- |
| `tourneys.php?type=name&query=<q>&submitted=1&go=Go`        | Tournament search.                                      |
| `tourney.php?t=<id>`                                        | Tournament results page.                                |
| `tourneyanno.php?t=<id>`                                    | Tournament's annotated games. **Has the gid links.**    |
| `texttable.php?t=<id>`                                      | Tournament cross-table in plain-ish text — easier parse.|

### Game download (already wired in `get_xt_gcg`)

```
https://cross-tables.com/annotated/selfgcg/<first-3-of-gid>/anno<gid>.gcg
```

For `gid = 52913`, that's `.../selfgcg/529/anno52913.gcg`.

### Bonus endpoints

| URL                              | Purpose                                  |
| -------------------------------- | ---------------------------------------- |
| `profile.php?p=<id>`             | Player bio / rating panel.               |
| `games.php?p=<id>`               | All games (not just annotated).          |
| `graph.php?p=<id>`               | Rating history.                          |
| `histratings.php?p=<id>`         | Historical ratings table.                |
| `bystate.php?st=<XX>`            | US players by state.                     |
| `bycountry.php?ctry=<XX>`        | Players by country.                      |
| `leaders.php` / `leaves.php`     | Leaderboards.                            |

## Anchor patterns to scrape

Five regexes cover the full flow. Names alongside each link come from
the anchor text (the `<a>...</a>` content) on the same line.

```python
# Player query → list of player hits
re.findall(r'href="results\.php\?playerid=(\d+)"', body)

# Tournament query → list of tournament hits
re.findall(r'href="tourney\.php\?t=(\d+)"', body)

# Player annotated-games page → list of game ids
re.findall(r'href="annotated\.php\?u=(\d+)"', body)

# Tournament annotated-games page → list of game ids
re.findall(r'href="annotated\.php\?u=(\d+)"', body)

# Player / tournament name extraction (from surrounding anchor text)
re.findall(r'href="results\.php\?playerid=\d+">([^<]+)</a>', body)
re.findall(r'href="tourney\.php\?t=\d+">([^<]+)</a>',     body)
```

## HTTP gotchas

- **`User-Agent` must look like curl or a browser.** `Python-urllib/3.x`
  trips mod_security and gets a 406. `User-Agent: curl/8.0` works.
- **`Accept: */*` is required** on most endpoints, otherwise 406.
- **No documented rate limit.** Reuse `fetch_games.py`'s convention of
  ~300 ms between fetches.
- **No JSON, no API key.** Everything is server-rendered HTML; no
  authentication needed.
- **No directory listings.** `annotated/`, `annotated/selfgcg/`, etc.
  return 403 — paths only work for fully-qualified game URLs.

## What you get vs. Woogles

| Capability                       | cross-tables                  | Woogles                      |
| -------------------------------- | ----------------------------- | ---------------------------- |
| GCG download                     | Static URL, basic format      | `GetGCG` JSON wrapper        |
| Structured per-move data         | **No**                        | `GetGameHistory` JSON        |
| Per-move clock (`millis_remaining`) | **No**                     | Yes, on every event          |
| Player listing                   | `anno.php?p=<id>` (HTML)      | `GetRecentGames` JSON        |
| Tournament listing               | `tourneyanno.php?t=<id>` (HTML) | Tournament service, JSON   |
| Search by name                   | `players.php?query=` / `tourneys.php?query=` (HTML) | (User search service) |

Cross-tables is strictly GCG-and-only-GCG: no clocks, no decoded
row/col/direction, no `words_formed`. If a game is reachable from both
sources, prefer the Woogles endpoint.

## TUI integration sketch

A new startup-menu item ("Browse cross-tables") or a tab inside the
existing Load Game modal, opening this flow:

```
[search]   ( ) player   ( ) tournament
[query]    ____________________________
            (Enter to search)
              ↓
[hits]     list of matches, selectable
              ↓
[games]    annotated games for the selected hit
              ↓
[load]     existing get_xt_gcg path with the gid
```

### Implementation outline

1. **`src/impl/xtables_search.c`** — pure C, mirrors `get_xt_gcg`:
   - `xtables_search_players(query, ...)` shells out to `curl` to fetch
     `players.php?query=<q>` and regex-extracts `(playerid, name)`
     pairs.
   - `xtables_search_tournaments(query, ...)` does the same for
     `tourneys.php?...`.
   - `xtables_list_player_games(player_id, ...)` fetches `anno.php?p=`
     and extracts `(game_id, opponent_or_round_label)` pairs.
   - `xtables_list_tournament_games(tourney_id, ...)` fetches
     `tourneyanno.php?t=`.

2. **`tui/xtables_search.[ch]`** — modal state + rendering:
   - A 4-state modal: search mode → query input → hits list → games list.
   - Reuses the readline helper, the modal text-input layout, and the
     existing list-of-rows pattern from the lexicon picker.
   - Loading commits via the existing `get_gcg` path with the selected
     `annotated.php?u=<gid>` URL (which `get_xt_gcg` already recognizes).

3. **Background fetches.** Network calls block; do them on the bot
   worker thread so the TUI stays responsive. A spinner where the hits
   list would render covers the latency.

### Size

~50 LOC of scraping helpers, ~200 LOC of modal state + rendering, plus
the menu wiring. Smaller if we lean on a host `curl` like `get_xt_gcg`
already does instead of pulling in `libcurl`.

## Open questions

- **Which annotated-game listing field do we surface as a label?**
  `anno.php`'s rows show opponent + tournament + date. Tournament's
  `tourneyanno.php` rows show round + both players + result.
- **Do we want to also reach un-annotated games?** `games.php?p=` lists
  every tournament game; only a subset have annotated GCG uploads. For
  V1, restrict to annotated.
- **Caching.** A naive flow refetches the player or tournament page on
  every keystroke. Debounce the query input (re-fetch only on Enter or
  500 ms of idle), and cache the last 5 player/tournament pages in
  memory for the session.
