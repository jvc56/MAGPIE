#!/usr/bin/env python3
"""Render inneragree CSV + details JSONL into a single self-contained HTML.

Each card shows the standard metrics, the board, racks, scores, bag, the
nested pick (and flat pick if present), plus an expandable "details" section
that renders heatmaps and a per-arm score table from inlined JSON.

Inputs:
  CSV   = INNERAGREE_CSV   (default /tmp/gamepairbai/inneragree_perroot_v2.csv)
  JSONL = INNERAGREE_JSONL (default /tmp/gamepairbai/inneragree_perroot_v2_details.jsonl)
Output:
  HTML  = INNERAGREE_HTML  (default /tmp/gamepairbai/inneragree_perroot_v2.html)
"""
import csv
import html
import json
import os
import sys
from collections import Counter
from pathlib import Path

CSV_PATH   = Path(os.environ.get("INNERAGREE_CSV",
                                  "/tmp/gamepairbai/inneragree_perroot_v2.csv"))
JSONL_PATH = Path(os.environ.get("INNERAGREE_JSONL",
                                  "/tmp/gamepairbai/inneragree_perroot_v2_details.jsonl"))
OUT_PATH   = Path(os.environ.get("INNERAGREE_HTML",
                                  "/tmp/gamepairbai/inneragree_perroot_v2.html"))

BONUS = {}
TWS = [(0,0),(0,7),(0,14),(7,0),(7,14),(14,0),(14,7),(14,14)]
DWS = [(1,1),(2,2),(3,3),(4,4),(1,13),(2,12),(3,11),(4,10),
       (10,4),(11,3),(12,2),(13,1),(10,10),(11,11),(12,12),(13,13),(7,7)]
TLS = [(1,5),(1,9),(5,1),(5,5),(5,9),(5,13),
       (9,1),(9,5),(9,9),(9,13),(13,5),(13,9)]
DLS = [(0,3),(0,11),(2,6),(2,8),(3,0),(3,7),(3,14),(6,2),(6,6),(6,8),(6,12),
       (7,3),(7,11),(8,2),(8,6),(8,8),(8,12),(11,0),(11,7),(11,14),
       (12,6),(12,8),(14,3),(14,11)]
for r,c in TWS: BONUS[(r,c)] = "3W"
for r,c in DWS: BONUS[(r,c)] = "2W"
for r,c in TLS: BONUS[(r,c)] = "3L"
for r,c in DLS: BONUS[(r,c)] = "2L"

DIST = dict(A=9, B=2, C=2, D=4, E=12, F=2, G=3, H=2, I=9, J=1, K=1, L=4, M=2,
            N=6, O=8, P=2, Q=1, R=6, S=4, T=6, U=4, V=2, W=2, X=1, Y=2, Z=1)
DIST["?"] = 2

def parse_cgp_board(rows_str):
    rows = rows_str.split("/")
    board = [[None]*15 for _ in range(15)]
    for r, row_str in enumerate(rows):
        c = 0; i = 0
        while i < len(row_str):
            ch = row_str[i]
            if ch.isdigit():
                j = i
                while j < len(row_str) and row_str[j].isdigit():
                    j += 1
                c += int(row_str[i:j]); i = j
            else:
                board[r][c] = ch; c += 1; i += 1
    return board

def parse_cgp(cgp):
    parts = cgp.split(" ")
    board = parse_cgp_board(parts[0])
    racks_str = parts[1] if len(parts) > 1 else "/"
    scores_str = parts[2] if len(parts) > 2 else "0/0"
    racks = racks_str.split("/")
    rack_on_turn = racks[0] if racks else ""
    rack_other   = racks[1] if len(racks) >= 2 else ""
    sp = scores_str.split("/")
    try:
        s_on = int(sp[0]) if sp[0] else 0
        s_other = int(sp[1]) if len(sp) > 1 else 0
    except ValueError:
        s_on = s_other = 0
    used = Counter()
    for row in board:
        for tile in row:
            if tile is None: continue
            if tile.islower(): used["?"] += 1
            else: used[tile.upper()] += 1
    for r in (rack_on_turn, rack_other):
        for ch in r:
            if ch == "?": used["?"] += 1
            else: used[ch.upper()] += 1
    bag = Counter()
    for letter, total in DIST.items():
        rem = total - used.get(letter, 0)
        if rem > 0:
            bag[letter] = rem
    return dict(board=board, rack_on_turn=rack_on_turn, rack_other=rack_other,
                score_on_turn=s_on, score_other=s_other, bag=bag)

def render_board_html(board):
    cells = []
    for r in range(15):
        for c in range(15):
            tile = board[r][c]
            bonus = BONUS.get((r,c), "")
            if tile is not None:
                blank = tile.islower()
                cls = "tile" + (" blank" if blank else "")
                content = tile.upper()
            else:
                cls = "empty " + (f"b-{bonus.lower()}" if bonus else "plain")
                content = bonus if bonus else ""
            cells.append(f'<div class="{cls}">{html.escape(content)}</div>')
    return f'<div class="board">{"".join(cells)}</div>'

def render_rack_html(rack_str):
    tiles = []
    for ch in rack_str:
        cls = "tile blank" if ch == "?" else "tile"
        disp = "?" if ch == "?" else ch.upper()
        tiles.append(f'<div class="{cls}">{html.escape(disp)}</div>')
    return f'<div class="rack">{"".join(tiles) or "&nbsp;"}</div>'

def render_bag_html(bag):
    parts = []
    for letter in sorted(bag.keys(), key=lambda x: ("z" if x == "?" else x)):
        for _ in range(bag[letter]):
            cls = "tile blank" if letter == "?" else "tile small"
            disp = "?" if letter == "?" else letter
            parts.append(f'<div class="{cls}">{disp}</div>')
    total = sum(bag.values())
    return (f'<div class="bag-meta">bag: {total} tiles</div>'
            f'<div class="rack bag">{"".join(parts) or "&nbsp;"}</div>')

def loss_color(loss):
    t = min(loss / 0.3, 1.0)
    if t < 0.5:
        r = int(255 * (t*2)); g = 200; b = 80
    else:
        r = 255; g = int(200*(1-(t-0.5)*2)); b = 80
    return f"rgb({r},{g},{b})"

def agree_color(rate):
    r = int(255*(1-rate)); g = int(180*rate+60); b = 80
    return f"rgb({r},{g},{b})"

def main():
    rows = list(csv.DictReader(CSV_PATH.open()))
    rows = [r for r in rows if int(r["inner_calls"]) > 0]
    rows.sort(key=lambda r: -float(r["inner_loss_mean"]))
    print(f"loaded {len(rows)} CSV positions", file=sys.stderr)

    # Load JSONL details, index by pos id.
    details = {}
    if JSONL_PATH.exists():
        with JSONL_PATH.open() as f:
            for line in f:
                obj = json.loads(line)
                key = f"g{obj['game']}_t{obj['turn']}_p{obj['on_turn_player']}"
                details[key] = obj
        print(f"loaded {len(details)} details entries from {JSONL_PATH.name}",
              file=sys.stderr)
    else:
        print(f"no JSONL file at {JSONL_PATH} — cards will lack details",
              file=sys.stderr)

    parts = []
    parts.append("""<!doctype html>
<html><head><meta charset="utf-8">
<title>Inner-pick agreement / loss per root position</title>
<style>
  body { font-family: -apple-system,Segoe UI,Helvetica,Arial,sans-serif;
         background: #1a1a1a; color: #e0e0e0; margin: 20px; }
  h1 { font-size: 1.4rem; margin: 0 0 0.5rem 0; }
  .summary { color: #aaa; margin-bottom: 1.5rem; font-size: 0.9rem; }
  .card { background: #262626; border-radius: 8px; padding: 14px;
          margin-bottom: 14px; display: grid;
          grid-template-columns: max-content 1fr; gap: 18px; }
  .card-header { grid-column: 1 / -1; display: flex;
                 justify-content: space-between; align-items: baseline;
                 gap: 12px; flex-wrap: wrap; }
  .pos-id { font-weight: 600; font-size: 1.0rem; }
  .scores { font-family: monospace; font-size: 0.9rem; color: #ccc; }
  .scores .on { color: #ffd078; font-weight: 600; }
  .metrics { display: grid; grid-template-columns: repeat(6, max-content);
             gap: 8px 16px; font-size: 0.85rem; margin-top: 6px;
             grid-column: 1 / -1; }
  .m-label { color: #888; font-size: 0.75rem; }
  .m-val { font-variant-numeric: tabular-nums; }
  .pill { display: inline-block; padding: 2px 6px; border-radius: 3px;
          color: #000; font-weight: 600; }
  .board-col { display: flex; flex-direction: column; gap: 8px; }
  .board { display: grid; grid-template-columns: repeat(15, 22px);
           grid-template-rows: repeat(15, 22px); gap: 0;
           background: #444; border: 1px solid #666; position: relative; }
  .board > div { display: flex; align-items: center; justify-content: center;
                 font-size: 11px; font-weight: 600; font-family: monospace;
                 position: relative; }
  .tile { background: #f0d49a; color: #000; border: 1px solid #b89968; }
  .tile.blank { background: #e0c080; color: #555; }
  .empty.plain { background: #d8d0b8; color: transparent; }
  .empty.b-3w { background: #c84030; color: #fff; font-size: 8px; }
  .empty.b-2w { background: #e8a0a0; color: #500; font-size: 8px; }
  .empty.b-3l { background: #4080c0; color: #fff; font-size: 8px; }
  .empty.b-2l { background: #a0c8e0; color: #003; font-size: 8px; }
  .rack { display: flex; gap: 2px; flex-wrap: wrap; }
  .rack .tile { width: 22px; height: 22px; display: flex;
                align-items: center; justify-content: center;
                font-family: monospace; font-size: 13px; font-weight: 600; }
  .rack.bag .tile.small, .rack.bag .tile.blank {
    width: 18px; height: 18px; font-size: 10px;
    background: #d8c898; border: 1px solid #a89868;
  }
  .rhs { display: flex; flex-direction: column; gap: 10px; min-width: 360px; }
  .pick-line { font-size: 0.9rem; }
  .pick-line strong { color: #ffd078; font-family: monospace; }
  .pick-line .label { color: #888; }
  .same-tag { color: #80c080; font-size: 0.75rem; margin-left: 6px; }
  .ctx { color: #888; font-size: 0.75rem; text-transform: uppercase;
         letter-spacing: 0.5px; }
  .ctx-row { display: grid; grid-template-columns: max-content 1fr;
             gap: 8px; align-items: center; }
  .bag-meta { color: #888; font-size: 0.8rem; margin-top: 4px; }
  details { margin-top: 6px; }
  details summary { cursor: pointer; color: #888; font-size: 0.8rem; }
  details pre { background: #1a1a1a; padding: 8px; border-radius: 4px;
                font-size: 0.75rem; overflow-x: auto; color: #aaa; }
  .det-area { grid-column: 1 / -1; margin-top: 8px;
              border-top: 1px solid #333; padding-top: 10px; }
  .det-controls { display: flex; gap: 16px; align-items: center;
                  flex-wrap: wrap; margin-bottom: 12px; font-size: 0.85rem; }
  .det-controls label { color: #aaa; }
  .det-controls select { background: #333; color: #eee;
                         border: 1px solid #555; padding: 3px 6px; }
  .det-controls .btn { background: #404a60; color: #cde;
                       padding: 6px 12px; border: none; border-radius: 4px;
                       cursor: pointer; font-size: 0.85rem; }
  .det-controls .btn:hover { background: #506080; }
  .hm-row { display: grid;
            grid-template-columns: repeat(auto-fit, minmax(360px, 1fr));
            gap: 16px; width: 100%; }
  .hm-col { display: flex; flex-direction: column; align-items: stretch;
            gap: 6px; min-width: 0; }
  .hm-col .lbl { font-size: 0.78rem; color: #aaa; text-transform: uppercase;
                 text-align: center; }
  .hm-board { display: grid; grid-template-columns: repeat(15, minmax(0,1fr));
              grid-template-rows: repeat(15, 1fr); gap: 0;
              background: #333; border: 1px solid #555;
              width: 100%; aspect-ratio: 1; }
  .hm-board > div { display: flex; align-items: center; justify-content: center;
                    font-size: clamp(7px, 1.1cqw * 0.9, 14px);
                    font-family: monospace; aspect-ratio: 1; min-width: 0;
                    border-right: 1px solid rgba(0,0,0,0.06);
                    border-bottom: 1px solid rgba(0,0,0,0.06);
                    overflow: hidden; }
  /* container queries for responsive font scaling on the heatmap cells */
  .hm-board { container-type: inline-size; }
  .hm-board > div { font-size: clamp(7px, 1.1cqw, 14px); }
  .hm-tile { background: #cdb88a; color: #000;
             font-size: clamp(10px, 1.6cqw, 18px) !important; }
  .exch-strip { display: grid;
                grid-template-columns: repeat(8, minmax(0,1fr));
                gap: 2px; margin-top: 4px; }
  .exch-cell { background: #2a2a2a; border: 1px solid #444;
               display: flex; flex-direction: column; align-items: center;
               justify-content: center; padding: 4px 2px;
               font-family: monospace; }
  .exch-cell .lbl-sm { font-size: 0.6rem; color: #aaa;
                       text-transform: uppercase; }
  .exch-cell .val { font-size: 0.85rem; color: #fff; }
  .table-controls { display: flex; gap: 12px; align-items: center;
                    margin: 10px 0 4px; font-size: 0.8rem; }
  .table-controls input { background: #1a1a1a; color: #eee;
                          border: 1px solid #555; padding: 4px 8px;
                          font-family: monospace; min-width: 220px; }
  .table-controls .table-summary { color: #888; }
  .table-scroll { max-height: 480px; overflow: auto;
                  border: 1px solid #333; }
  .arm-table { font-size: 0.78rem; border-collapse: collapse;
               width: 100%; }
  .arm-table th, .arm-table td { padding: 3px 8px; text-align: right;
                                  border-bottom: 1px solid #333;
                                  white-space: nowrap; }
  .arm-table thead th { position: sticky; top: 0; background: #1f1f1f;
                        color: #aaa; font-weight: 600; text-align: center;
                        cursor: pointer; user-select: none;
                        border-bottom: 1px solid #555; }
  .arm-table thead th.col-group { background: #262626; color: #888;
                                   font-size: 0.7rem; cursor: default;
                                   text-transform: uppercase;
                                   letter-spacing: 0.6px; }
  .arm-table thead th .sort-arrow { color: #ffd078; margin-left: 4px; }
  .arm-table td.move { text-align: left; font-family: monospace;
                       color: #ffd078; }
  .arm-table td.us { color: #d88; }
  .arm-table td.them { color: #8bd; }
  .arm-table td.n-cell { color: #cad8ff; }
  .arm-table td.f-cell { color: #e0bfbf; }
  .arm-table td.dim { color: #555; }
  .var-leg { display: flex; gap: 12px; font-size: 0.75rem;
             color: #888; margin: 4px 0; }
  .var-leg .n { color: #cad8ff; }
  .var-leg .f { color: #e0bfbf; }
  .variant-tabs { display: flex; gap: 4px; margin-bottom: 8px; }
  .variant-tabs button { background: #2a2a2a; color: #ccc;
                          border: 1px solid #444; padding: 4px 10px;
                          cursor: pointer; }
  .variant-tabs button.active { background: #404a60; color: #fff; }
</style></head><body>""")

    parts.append('<h1>Inner-pick agreement / loss per root position</h1>')
    parts.append(f'<div class="summary">'
                 f'{len(rows)} root positions from <code>{CSV_PATH.name}</code> + '
                 f'{len(details)} detail entries from <code>{JSONL_PATH.name}</code>. '
                 f'Cards sorted by mean inner-call loss (spiciest first). '
                 f'Click "Show details" on any card to render heatmaps and per-arm scores.'
                 f'</div>')

    # Inline all per-position board parses + details JSON.
    inline_data = {}
    for r in rows:
        pos_id = f"g{r['game']}_t{r['turn']}_p{r['on_turn_player']}"
        inline_data[pos_id] = {
            "csv": r,
            "details": details.get(pos_id),
        }
    # Heatmap-related JS data is the biggest part; details may be None for
    # positions that weren't augmented.
    # We'll just inline the whole thing as one big JS literal.
    parts.append('<script>')
    parts.append('window.INNERAGREE = ')
    parts.append(json.dumps(inline_data))
    parts.append(';</script>')

    parts.append("""<script>
const BONUS_TWS = new Set([[0,0],[0,7],[0,14],[7,0],[7,14],[14,0],[14,7],[14,14]].map(p=>p[0]*15+p[1]));
const BONUS_DWS = new Set([[1,1],[2,2],[3,3],[4,4],[1,13],[2,12],[3,11],[4,10],
  [10,4],[11,3],[12,2],[13,1],[10,10],[11,11],[12,12],[13,13],[7,7]].map(p=>p[0]*15+p[1]));
const BONUS_TLS = new Set([[1,5],[1,9],[5,1],[5,5],[5,9],[5,13],
  [9,1],[9,5],[9,9],[9,13],[13,5],[13,9]].map(p=>p[0]*15+p[1]));
const BONUS_DLS = new Set([[0,3],[0,11],[2,6],[2,8],[3,0],[3,7],[3,14],[6,2],[6,6],[6,8],[6,12],
  [7,3],[7,11],[8,2],[8,6],[8,8],[8,12],[11,0],[11,7],[11,14],
  [12,6],[12,8],[14,3],[14,11]].map(p=>p[0]*15+p[1]));

function parseBoard(cgp) {
  const rowsStr = cgp.split(' ')[0].split('/');
  const board = Array.from({length:15}, () => Array(15).fill(null));
  for (let r=0; r<15; r++) {
    let c=0, i=0;
    const s = rowsStr[r] || '';
    while (i < s.length) {
      const ch = s[i];
      if (ch >= '0' && ch <= '9') {
        let j=i;
        while (j<s.length && s[j]>='0' && s[j]<='9') j++;
        c += parseInt(s.substring(i,j),10);
        i = j;
      } else {
        board[r][c] = ch; c++; i++;
      }
    }
  }
  return board;
}

function heatColor(frac) {
  if (frac <= 0) return null;
  const t = Math.min(1, Math.sqrt(frac));
  let r, g, b;
  if (t < 0.5) {
    r = Math.round(2*t*255);
    g = Math.round(2*t*200);
    b = Math.round(200 - 2*t*100);
  } else {
    r = 255;
    g = Math.round(200 - (t-0.5)*2*180);
    b = Math.round(100 - (t-0.5)*2*100);
  }
  return `rgb(${r},${g},${b})`;
}

// Divergent color for diff-mode cells. Positive = nested places more (warm),
// negative = flat places more (cool).
function diffColor(diffPct, maxAbsPct) {
  if (Math.abs(diffPct) < 0.05) return null;
  const t = Math.min(1, Math.abs(diffPct) / Math.max(0.5, maxAbsPct));
  const intensity = Math.sqrt(t);
  if (diffPct > 0) {
    // warm red for nested-more
    const r = Math.round(220*intensity + 35);
    const g = Math.round(60 - 30*intensity);
    const b = Math.round(60 - 30*intensity);
    return `rgb(${r},${g},${b})`;
  } else {
    // cool cyan-blue for flat-more
    const r = Math.round(60 - 30*intensity);
    const g = Math.round(90 + 60*intensity);
    const b = Math.round(220*intensity + 35);
    return `rgb(${r},${g},${b})`;
  }
}

// renderHm modes:
//   { kind: 'count', vals: counts[225], total: int }  - standard heatmap
//   { kind: 'diff',  vals: pctDiffs[225] }            - signed diff, divergent
function renderHm(container, cgp, spec) {
  container.innerHTML = '';
  const board = parseBoard(cgp);
  if (spec.kind === 'diff') {
    let maxAbs = 0;
    for (let i=0; i<225; i++) {
      const a = Math.abs(spec.vals[i]);
      if (a > maxAbs) maxAbs = a;
    }
    for (let r=0; r<15; r++) {
      for (let c=0; c<15; c++) {
        const cell = document.createElement('div');
        const tile = board[r][c];
        const k = r*15 + c;
        if (tile !== null) {
          cell.className = 'hm-tile';
          cell.textContent = tile.toUpperCase();
        } else {
          const dv = spec.vals[k];
          const col = diffColor(dv, maxAbs);
          if (col) {
            cell.style.background = col;
            const aabs = Math.abs(dv);
            if (aabs >= 0.05) {
              const sign = dv > 0 ? '+' : '-';
              cell.textContent = sign + aabs.toFixed(1).padStart(4, '0');
            }
          } else {
            if (BONUS_TWS.has(k)) cell.style.background = '#c84030';
            else if (BONUS_DWS.has(k)) cell.style.background = '#e8a0a0';
            else if (BONUS_TLS.has(k)) cell.style.background = '#4080c0';
            else if (BONUS_DLS.has(k)) cell.style.background = '#a0c8e0';
            else cell.style.background = '#d8d0b8';
          }
        }
        container.appendChild(cell);
      }
    }
    return;
  }
  // count mode
  let plyMax = 0;
  for (let i=0; i<225; i++) if (spec.vals[i] > plyMax) plyMax = spec.vals[i];
  const plyTotal = spec.total != null ? spec.total :
                   spec.vals.reduce((s, v) => s + v, 0);
  for (let r=0; r<15; r++) {
    for (let c=0; c<15; c++) {
      const cell = document.createElement('div');
      const tile = board[r][c];
      const k = r*15 + c;
      if (tile !== null) {
        cell.className = 'hm-tile';
        cell.textContent = tile.toUpperCase();
      } else {
        const count = spec.vals[k];
        const frac = plyMax > 0 ? (count / plyMax) : 0;
        const col = heatColor(frac);
        if (col) {
          cell.style.background = col;
          if (count > 0 && plyTotal > 0) {
            const pct = (count / plyTotal) * 100;
            if (pct >= 0.1) {
              cell.textContent = pct.toFixed(1).padStart(4, '0');
            }
          }
        } else {
          if (BONUS_TWS.has(k)) cell.style.background = '#c84030';
          else if (BONUS_DWS.has(k)) cell.style.background = '#e8a0a0';
          else if (BONUS_TLS.has(k)) cell.style.background = '#4080c0';
          else if (BONUS_DLS.has(k)) cell.style.background = '#a0c8e0';
          else cell.style.background = '#d8d0b8';
        }
      }
      container.appendChild(cell);
    }
  }
}

function aggregateHm(arms, plyIdx, field) {
  field = field || 'hm';
  if (!arms.length) return new Array(225).fill(0);
  const hm = new Array(225).fill(0);
  for (const arm of arms) {
    const ply = arm.plies[plyIdx];
    if (!ply || !ply[field]) continue;
    const src = ply[field];
    for (let i=0; i<225; i++) hm[i] += src[i];
  }
  return hm;
}

// Aggregate per-ply non-tile-placement counts (pass + exchange-by-size)
// across arms. Returns { pass, exch: [8 counts indexed by size 1..7], total }.
function aggregateExchCounts(arms, plyIdx) {
  let pass = 0;
  const exch = new Array(7).fill(0);
  let total = 0;
  for (const arm of arms) {
    const ply = arm.plies[plyIdx];
    if (!ply) continue;
    pass += ply.pass_count || 0;
    total += ply.score_count || 0;
    if (ply.exch_counts) {
      for (let i = 0; i < 7; i++) exch[i] += ply.exch_counts[i] || 0;
    }
  }
  return { pass, exch, total };
}

function renderExchStrip(container, exchData) {
  if (!exchData || !exchData.total) return;
  const strip = document.createElement('div');
  strip.className = 'exch-strip';
  const cells = [{ label: 'PASS', count: exchData.pass }];
  for (let i = 0; i < 7; i++) {
    cells.push({ label: `EX${i+1}`, count: exchData.exch[i] });
  }
  // Color scale: use sqrt(pct) so small but nonzero is visible.
  cells.forEach(({ label, count }) => {
    const cell = document.createElement('div');
    cell.className = 'exch-cell';
    const pct = (count / exchData.total) * 100;
    if (pct > 0) {
      const t = Math.min(1, Math.sqrt(pct / 30));
      const col = heatColor(t);
      if (col) cell.style.background = col;
      if (pct >= 0.1) {
        cell.innerHTML = `<div class="lbl-sm">${label}</div>` +
                        `<div class="val" style="color:#000">${pct.toFixed(1).padStart(4,'0')}</div>`;
      } else {
        cell.innerHTML = `<div class="lbl-sm">${label}</div>` +
                        `<div class="val" style="color:#000">·</div>`;
      }
    } else {
      cell.innerHTML = `<div class="lbl-sm">${label}</div><div class="val">·</div>`;
    }
    strip.appendChild(cell);
  });
  container.appendChild(strip);
}

// Compute per-square diff (nested% - flat%) over a chosen ply.
// Always normalizes by ALL-plays total so that bingos-only diffs reflect
// (bingo-pct of all plays) differences between variants — not (bingo-pct
// of bingos), which would distort the comparison when the two variants
// have very different bingo rates.
function buildDiffPcts(nestedArms, flatArms, plyIdx, sel, field) {
  field = field || 'hm';
  let nHm, fHm, nAll, fAll;
  if (sel === '__all__') {
    nHm = aggregateHm(nestedArms, plyIdx, field);
    fHm = aggregateHm(flatArms, plyIdx, field);
    nAll = (field === 'hm') ? nHm : aggregateHm(nestedArms, plyIdx, 'hm');
    fAll = (field === 'hm') ? fHm : aggregateHm(flatArms, plyIdx, 'hm');
  } else {
    const armIdx = parseInt(sel, 10);
    const npl = nestedArms[armIdx] && nestedArms[armIdx].plies[plyIdx];
    const fpl = flatArms[armIdx] && flatArms[armIdx].plies[plyIdx];
    nHm = (npl && npl[field]) || new Array(225).fill(0);
    fHm = (fpl && fpl[field]) || new Array(225).fill(0);
    nAll = (npl && npl.hm) || new Array(225).fill(0);
    fAll = (fpl && fpl.hm) || new Array(225).fill(0);
  }
  const nTot = nAll.reduce((s,v)=>s+v,0);
  const fTot = fAll.reduce((s,v)=>s+v,0);
  const diffs = new Array(225).fill(0);
  if (nTot === 0 || fTot === 0) return diffs;
  for (let i=0; i<225; i++) {
    diffs[i] = (nHm[i] / nTot - fHm[i] / fTot) * 100;
  }
  return diffs;
}

function renderDetails(card, posId) {
  const data = window.INNERAGREE[posId];
  if (!data || !data.details) {
    card.querySelector('.det-content').innerHTML = '<div style="color:#888">no details available</div>';
    return;
  }
  const cgp = data.csv.cgp;
  const det = data.details;
  const content = card.querySelector('.det-content');
  content.innerHTML = '';

  // Variant tabs + arm selector + (player-overlay placeholder)
  const ctrls = document.createElement('div');
  ctrls.className = 'det-controls';
  ctrls.innerHTML = `
    <div class="variant-tabs">
      <button data-variant="nested" class="active">nested</button>
      <button data-variant="flat">flat</button>
    </div>
    <label>Arm: <select class="arm-select"></select></label>
    <label><input type="checkbox" class="bingos-only"> bingos only</label>
    <label><input type="checkbox" class="diff-toggle"> diff (nested - flat)</label>
  `;
  content.appendChild(ctrls);

  const hmRow = document.createElement('div');
  hmRow.className = 'hm-row';
  content.appendChild(hmRow);

  const tableHost = document.createElement('div');
  content.appendChild(tableHost);

  let variant = 'nested';

  function refreshArmSelect() {
    const sel = ctrls.querySelector('.arm-select');
    const prevValue = sel.value;
    sel.innerHTML = '';
    const all = document.createElement('option');
    all.value = '__all__';
    all.textContent = 'All candidates (aggregate)';
    sel.appendChild(all);
    const arms = det.variants[variant].arms;
    // Sort dropdown entries by current variant's win_pct, best first.
    // Keep the original arm index in `value` so refreshHeat indexes correctly.
    const sorted = arms.map((arm, idx) => ({arm, idx}))
                       .sort((a, b) => b.arm.win_pct - a.arm.win_pct);
    sorted.forEach(({arm, idx}) => {
      const opt = document.createElement('option');
      opt.value = String(idx);
      opt.textContent = `[${idx}] ${arm.move}  wpct=${arm.win_pct.toFixed(3)}  n=${arm.win_pct_count}`;
      sel.appendChild(opt);
    });
    // Preserve previous selection across variant switches (same arm index
    // points to the same move in both variants since they share movegen).
    const hasPrev = prevValue &&
        Array.from(sel.options).some(o => o.value === prevValue);
    sel.value = hasPrev ? prevValue : '__all__';
  }

  function refreshHeat() {
    hmRow.innerHTML = '';
    const bingosOnly = ctrls.querySelector('.bingos-only').checked;
    const diffMode = ctrls.querySelector('.diff-toggle').checked;
    const field = bingosOnly ? 'hm_bingo' : 'hm';
    const sel = ctrls.querySelector('.arm-select').value;
    const onTurn = parseInt(data.csv.on_turn_player, 10);

    const nestedArms = det.variants.nested.arms;
    const flatArms = det.variants.flat.arms;
    const variantArms = det.variants[variant].arms;
    if (!variantArms.length) {
      hmRow.innerHTML = '<div style="color:#888">no arms</div>';
      return;
    }
    const numPlies = variantArms[0].plies.length;

    // Header text
    let header;
    if (diffMode) {
      header = `Diff (nested − flat) per-cell pct-points`;
      if (sel !== '__all__') {
        const arm = nestedArms[parseInt(sel,10)] || flatArms[parseInt(sel,10)];
        if (arm) header += ` · arm [${sel}] ${arm.move}`;
      } else {
        header += ` · all candidates`;
      }
      if (bingosOnly) header += ` · bingos only`;
    } else {
      if (sel === '__all__') {
        header = `${variant} · all candidates (${variantArms.length} arms)`;
      } else {
        const arm = variantArms[parseInt(sel,10)];
        header = `${variant} · arm ${sel}: ${arm.move}  wpct=${arm.win_pct.toFixed(3)}  n=${arm.win_pct_count}`;
      }
      if (bingosOnly) header += ` · bingos only`;
    }
    const headerDiv = document.createElement('div');
    headerDiv.style.gridColumn = '1 / -1';
    headerDiv.style.color = '#ccc';
    headerDiv.style.marginBottom = '6px';
    headerDiv.style.fontSize = '0.85rem';
    headerDiv.textContent = header;
    hmRow.appendChild(headerDiv);

    for (let p=0; p<numPlies; p++) {
      const colEl = document.createElement('div');
      colEl.className = 'hm-col';
      const plyPlayer = (onTurn + p) % 2;
      colEl.innerHTML = `<div class="lbl">ply ${p}  ·  P${plyPlayer}</div>`;
      const bd = document.createElement('div');
      bd.className = 'hm-board';
      colEl.appendChild(bd);

      // Caption: per-ply avg score + bingo rate.
      const captionDiv = document.createElement('div');
      captionDiv.className = 'lbl';
      captionDiv.style.color = '#ddd';
      colEl.appendChild(captionDiv);

      if (diffMode) {
        const diffs = buildDiffPcts(nestedArms, flatArms, p, sel, field);
        // For caption: report total abs pct-point divergence and per-variant
        // avg scores so user can see how the rollouts differ.
        const totalAbs = diffs.reduce((s,v)=>s+Math.abs(v),0);
        const nArm = (sel === '__all__') ? null : nestedArms[parseInt(sel,10)];
        const fArm = (sel === '__all__') ? null : flatArms[parseInt(sel,10)];
        let cap = `|diff| total ${totalAbs.toFixed(1)}pp`;
        if (nArm && fArm && nArm.plies[p] && fArm.plies[p]) {
          const ns = nArm.plies[p].score_mean, fs = fArm.plies[p].score_mean;
          const nb = nArm.plies[p].bingo_rate, fb = fArm.plies[p].bingo_rate;
          cap += `  ·  score N=${ns.toFixed(1)} F=${fs.toFixed(1)} (${(ns-fs).toFixed(1)})`;
          cap += `  ·  bingo N=${(nb*100).toFixed(0)}% F=${(fb*100).toFixed(0)}%`;
        }
        captionDiv.textContent = cap;
        renderHm(bd, cgp, { kind: 'diff', vals: diffs });
      } else {
        let vals, valsAll;
        if (sel === '__all__') {
          vals = aggregateHm(variantArms, p, field);
          valsAll = (field === 'hm') ? vals
                    : aggregateHm(variantArms, p, 'hm');
        } else {
          const arm = variantArms[parseInt(sel,10)];
          const pp = arm && arm.plies[p];
          vals = (pp && pp[field]) || new Array(225).fill(0);
          valsAll = (pp && pp.hm) || new Array(225).fill(0);
        }
        // For bingos-only we normalize by the ALL-plays total so that the
        // shown percentages reflect bingo-as-fraction-of-all-plays (and the
        // heatmap sums to the bingo rate, not to 100%).
        const total = (bingosOnly ? valsAll : vals)
                        .reduce((s, v) => s + v, 0);
        if (sel === '__all__') {
          const totalScore = variantArms.reduce((s, a) => {
            const pp = a.plies[p];
            return s + (pp ? pp.score_mean * pp.score_count : 0);
          }, 0);
          const totalN = variantArms.reduce((s, a) => {
            const pp = a.plies[p];
            return s + (pp ? pp.score_count : 0);
          }, 0);
          const bingoSum = variantArms.reduce((s, a) => {
            const pp = a.plies[p];
            return s + (pp ? pp.bingo_rate * pp.score_count : 0);
          }, 0);
          const bingoRate = totalN ? (bingoSum / totalN) : 0;
          captionDiv.textContent = totalN
            ? `avg score ${(totalScore/totalN).toFixed(1)} (n=${totalN}) bingo ${(bingoRate*100).toFixed(1)}%`
            : 'no data';
        } else {
          const arm = variantArms[parseInt(sel,10)];
          const pp = arm.plies[p];
          captionDiv.textContent = pp
            ? `avg score ${pp.score_mean.toFixed(1)} (n=${pp.score_count}) bingo ${(pp.bingo_rate*100).toFixed(1)}%`
            : 'no data';
        }
        renderHm(bd, cgp, { kind: 'count', vals, total });

        // Exchange / pass strip below the heatmap, scaled to total plays.
        let exchData;
        if (sel === '__all__') {
          exchData = aggregateExchCounts(variantArms, p);
        } else {
          const arm = variantArms[parseInt(sel,10)];
          const pp = arm && arm.plies[p];
          exchData = pp ? {
            pass: pp.pass_count || 0,
            exch: pp.exch_counts || new Array(7).fill(0),
            total: pp.score_count || 0,
          } : null;
        }
        renderExchStrip(colEl, exchData);
      }
      hmRow.appendChild(colEl);
    }
  }

  function buildMergedRows() {
    // Merge nested + flat arms by move string. Each row carries both
    // variants' data plus numeric values for sorting.
    const onTurn = parseInt(data.csv.on_turn_player, 10);
    const N = det.variants.nested.arms;
    const F = det.variants.flat.arms;
    const numPlies = (N[0]?.plies.length) || (F[0]?.plies.length) || 4;
    const byMove = new Map();
    function add(arm, variantKey) {
      if (!byMove.has(arm.move)) {
        byMove.set(arm.move, { move: arm.move, nested: null, flat: null });
      }
      byMove.get(arm.move)[variantKey] = arm;
    }
    N.forEach(a => add(a, 'nested'));
    F.forEach(a => add(a, 'flat'));
    const rows = Array.from(byMove.values()).map(row => {
      // Numeric helpers for sort/display.
      const nN = row.nested ? row.nested.win_pct_count : 0;
      const fN = row.flat ? row.flat.win_pct_count : 0;
      const nW = row.nested ? row.nested.win_pct : null;
      const fW = row.flat ? row.flat.win_pct : null;
      const plyScores = [];
      const plyBingos = [];
      for (let p=0; p<numPlies; p++) {
        const npl = row.nested?.plies[p];
        const fpl = row.flat?.plies[p];
        plyScores.push({
          n: (npl && npl.score_count) ? npl.score_mean : null,
          f: (fpl && fpl.score_count) ? fpl.score_mean : null,
        });
        plyBingos.push({
          n: (npl && npl.score_count) ? npl.bingo_rate : null,
          f: (fpl && fpl.score_count) ? fpl.bingo_rate : null,
        });
      }
      return { move: row.move, nW, fW, nN, fN, plyScores, plyBingos };
    });
    return { rows, numPlies, onTurn };
  }

  function refreshTable() {
    const { rows, numPlies, onTurn } = buildMergedRows();
    // Build header
    let head1 = '<tr><th class="col-group" colspan="1">&nbsp;</th>'
              + '<th class="col-group" colspan="1">move</th>'
              + '<th class="col-group" colspan="2">win pct</th>'
              + '<th class="col-group" colspan="2">samples</th>';
    head1 += `<th class="col-group" colspan="${2*numPlies}">avg score</th>`;
    head1 += `<th class="col-group" colspan="${2*numPlies}">bingo rate</th>`;
    head1 += '</tr>';
    let head2 = '<tr>'
              + '<th data-key="rank">#</th>'
              + '<th data-key="move">Move</th>'
              + '<th data-key="nW">N wpct</th>'
              + '<th data-key="fW">F wpct</th>'
              + '<th data-key="nN">N samples</th>'
              + '<th data-key="fN">F samples</th>';
    for (let p=0; p<numPlies; p++) {
      const player = (onTurn + p) % 2;
      head2 += `<th data-key="nS${p}">N ply${p} P${player}</th>`;
      head2 += `<th data-key="fS${p}">F ply${p} P${player}</th>`;
    }
    for (let p=0; p<numPlies; p++) {
      head2 += `<th data-key="nB${p}">N bingo${p}</th>`;
      head2 += `<th data-key="fB${p}">F bingo${p}</th>`;
    }
    head2 += '</tr>';
    const headHtml = `<thead>${head1}${head2}</thead>`;

    const baseSortKey = 'nW';
    let sortKey = tableHost.dataset.sortKey || baseSortKey;
    let sortDir = tableHost.dataset.sortDir || 'desc';
    let searchTerm = (tableHost.querySelector('.table-search')?.value || '').trim().toLowerCase();

    function keyValue(row, key) {
      if (key === 'rank') return row._initialRank; // assigned below
      if (key === 'move') return row.move;
      if (key === 'nW') return row.nW;
      if (key === 'fW') return row.fW;
      if (key === 'nN') return row.nN;
      if (key === 'fN') return row.fN;
      if (/^([nf])S(\d+)$/.test(key)) {
        const v = key[0] === 'n' ? 'n' : 'f';
        const p = parseInt(key.substring(2),10);
        return row.plyScores[p][v];
      }
      if (/^([nf])B(\d+)$/.test(key)) {
        const v = key[0] === 'n' ? 'n' : 'f';
        const p = parseInt(key.substring(2),10);
        return row.plyBingos[p][v];
      }
      return null;
    }

    rows.forEach((row, idx) => { row._initialRank = idx; });
    rows.sort((a, b) => {
      const va = keyValue(a, sortKey), vb = keyValue(b, sortKey);
      const aN = (va === null || va === undefined);
      const bN = (vb === null || vb === undefined);
      if (aN && bN) return 0;
      if (aN) return 1;
      if (bN) return -1;
      const cmp = (typeof va === 'string')
                ? va.localeCompare(vb)
                : (va - vb);
      return sortDir === 'asc' ? cmp : -cmp;
    });

    const filtered = searchTerm
      ? rows.filter(r => r.move.toLowerCase().includes(searchTerm))
      : rows;

    let bodyHtml = '<tbody>';
    filtered.forEach((row, displayIdx) => {
      const fmt = (v, dp=1) => (v === null || v === undefined) ? '<span class="dim">-</span>' : v.toFixed(dp);
      const pct = (v) => (v === null || v === undefined) ? '<span class="dim">-</span>' : (v*100).toFixed(0)+'%';
      bodyHtml += `<tr>`;
      bodyHtml += `<td>${row._initialRank}</td>`;
      bodyHtml += `<td class="move">${row.move.replace(/</g,'&lt;')}</td>`;
      bodyHtml += `<td class="n-cell">${fmt(row.nW, 3)}</td>`;
      bodyHtml += `<td class="f-cell">${fmt(row.fW, 3)}</td>`;
      bodyHtml += `<td class="n-cell">${row.nN || '<span class="dim">-</span>'}</td>`;
      bodyHtml += `<td class="f-cell">${row.fN || '<span class="dim">-</span>'}</td>`;
      for (let p=0; p<numPlies; p++) {
        const player = (onTurn + p) % 2;
        const plyCls = player === onTurn ? 'us' : 'them';
        bodyHtml += `<td class="n-cell ${plyCls}">${fmt(row.plyScores[p].n, 1)}</td>`;
        bodyHtml += `<td class="f-cell ${plyCls}">${fmt(row.plyScores[p].f, 1)}</td>`;
      }
      for (let p=0; p<numPlies; p++) {
        bodyHtml += `<td class="n-cell">${pct(row.plyBingos[p].n)}</td>`;
        bodyHtml += `<td class="f-cell">${pct(row.plyBingos[p].f)}</td>`;
      }
      bodyHtml += '</tr>';
    });
    bodyHtml += '</tbody>';

    // Mark active sort header
    const arrow = sortDir === 'asc' ? '▲' : '▼';
    const headWithArrow = (headHtml).replace(
      `data-key="${sortKey}"`,
      `data-key="${sortKey}" class="active"`
    ).replace(
      `data-key="${sortKey}" class="active">`,
      `data-key="${sortKey}" class="active">`,
    );
    // Inject arrow into the active header label.
    const headFinal = headWithArrow.replace(
      new RegExp(`(data-key="${sortKey}"[^>]*>)([^<]*)`,'i'),
      (m, p1, p2) => `${p1}${p2}<span class="sort-arrow">${arrow}</span>`
    );

    const controlsHtml = `
      <div class="table-controls">
        <input type="text" class="table-search" placeholder="filter by move..." value="${searchTerm.replace(/"/g,'&quot;')}">
        <span class="table-summary">${filtered.length} / ${rows.length} arms shown</span>
        <span class="var-leg"><span class="n">N = nested</span><span class="f">F = flat</span></span>
      </div>
      <div class="table-scroll">
        <table class="arm-table">${headFinal}${bodyHtml}</table>
      </div>
    `;
    tableHost.innerHTML = controlsHtml;

    // Wire sort click handlers
    tableHost.querySelectorAll('thead th[data-key]').forEach(th => {
      th.addEventListener('click', () => {
        const key = th.dataset.key;
        if (tableHost.dataset.sortKey === key) {
          tableHost.dataset.sortDir =
              tableHost.dataset.sortDir === 'asc' ? 'desc' : 'asc';
        } else {
          tableHost.dataset.sortKey = key;
          tableHost.dataset.sortDir = (key === 'move' || key === 'rank') ? 'asc' : 'desc';
        }
        refreshTable();
      });
    });
    // Wire search input
    const searchInput = tableHost.querySelector('.table-search');
    if (searchInput) {
      searchInput.addEventListener('input', () => refreshTable());
      // Preserve focus + cursor position
      if (searchTerm) {
        searchInput.focus();
        searchInput.setSelectionRange(searchInput.value.length, searchInput.value.length);
      }
    }
  }

  function refresh() {
    refreshArmSelect();
    refreshHeat();
    refreshTable();
  }

  ctrls.querySelectorAll('.variant-tabs button').forEach(btn => {
    btn.addEventListener('click', () => {
      ctrls.querySelectorAll('.variant-tabs button').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      variant = btn.dataset.variant;
      refresh();
    });
  });
  ctrls.querySelector('.arm-select').addEventListener('change', () => {
    refreshHeat();
  });
  ctrls.querySelector('.bingos-only').addEventListener('change', () => {
    refreshHeat();
  });
  ctrls.querySelector('.diff-toggle').addEventListener('change', () => {
    refreshHeat();
  });

  refresh();
}

document.addEventListener('click', (e) => {
  const btn = e.target.closest('.show-details-btn');
  if (!btn) return;
  const card = btn.closest('.card');
  const posId = card.dataset.posId;
  const area = card.querySelector('.det-area');
  if (area.style.display === 'block') {
    area.style.display = 'none';
    btn.textContent = 'Show details';
  } else {
    if (!area.dataset.rendered) {
      renderDetails(card, posId);
      area.dataset.rendered = '1';
    }
    area.style.display = 'block';
    btn.textContent = 'Hide details';
  }
});

// Auto-expand details on cards that have augment data, so the report
// surfaces what's there without forcing a click hunt.
document.addEventListener('DOMContentLoaded', () => {
  document.querySelectorAll('.card').forEach(card => {
    const posId = card.dataset.posId;
    const data = window.INNERAGREE[posId];
    if (data && data.details) {
      const area = card.querySelector('.det-area');
      const btn = card.querySelector('.show-details-btn');
      renderDetails(card, posId);
      area.dataset.rendered = '1';
      area.style.display = 'block';
      btn.textContent = 'Hide details';
    }
  });
});
</script>""")

    for r in rows:
        loss_mean = float(r["inner_loss_mean"])
        loss_std = float(r["inner_loss_stddev"])
        loss_max = float(r["inner_loss_max"])
        agree = float(r["inner_agree_rate"])
        calls = int(r["inner_calls"])
        early = float(r["inner_early_stop_rate"])
        rollouts = int(r["inner_rollouts"])
        avg_rollouts = rollouts / calls if calls else 0
        on_turn = int(r["on_turn_player"])

        parsed = parse_cgp(r["cgp"])
        if on_turn == 0:
            p0s, p0r = parsed["score_on_turn"], parsed["rack_on_turn"]
            p1s, p1r = parsed["score_other"], parsed["rack_other"]
        else:
            p1s, p1r = parsed["score_on_turn"], parsed["rack_on_turn"]
            p0s, p0r = parsed["score_other"], parsed["rack_other"]
        p0_cls = "on" if on_turn == 0 else ""
        p1_cls = "on" if on_turn == 1 else ""
        pos_id = f"g{r['game']}_t{r['turn']}_p{on_turn}"

        parts.append(f'<div class="card" data-pos-id="{pos_id}">')
        parts.append('  <div class="card-header">')
        parts.append(f'    <span class="pos-id">g{r["game"]} t{r["turn"]} p{on_turn} on turn</span>')
        parts.append(f'    <span class="scores">'
                     f'<span class="{p0_cls}">P0 {p0s}</span> &middot; '
                     f'<span class="{p1_cls}">P1 {p1s}</span></span>')
        parts.append(
            f'    <span class="pill" style="background:{loss_color(loss_mean)}">'
            f'mean loss {loss_mean:+.4f}</span>')
        parts.append('  </div>')
        parts.append('  <div class="metrics">')
        for label, val in [
            ("inner calls", f"{calls:,}"),
            ("avg rollouts/call", f"{avg_rollouts:.0f}"),
            ("agree rate", f'<span class="pill" style="background:{agree_color(agree)}">{agree:.3f}</span>'),
            ("loss stddev", f"{loss_std:.4f}"),
            ("loss max", f"{loss_max:+.4f}"),
            ("early stop", f"{early:.3f}"),
        ]:
            parts.append(f'    <div><div class="m-label">{label}</div>'
                         f'<div class="m-val">{val}</div></div>')
        parts.append('  </div>')
        parts.append(f'  <div class="board-col">{render_board_html(parsed["board"])}</div>')
        parts.append('  <div class="rhs">')
        parts.append(f'    <div class="ctx-row"><div class="ctx">'
                     f'{"★" if on_turn==0 else "&nbsp;"} P0 rack</div>'
                     f'{render_rack_html(p0r)}</div>')
        parts.append(f'    <div class="ctx-row"><div class="ctx">'
                     f'{"★" if on_turn==1 else "&nbsp;"} P1 rack</div>'
                     f'{render_rack_html(p1r)}</div>')
        parts.append(f'    {render_bag_html(parsed["bag"])}')
        parts.append(f'    <div class="pick-line"><span class="label">nested pick:</span> '
                     f'<strong>{html.escape(r["nested_pick"])}</strong></div>')
        parts.append('    <button class="btn show-details-btn">Show details</button>')
        parts.append(f'    <details><summary>CGP</summary><pre>{html.escape(r["cgp"])}</pre></details>')
        parts.append('  </div>')
        parts.append(f'  <div class="det-area" style="display:none">'
                     f'<div class="det-content"></div></div>')
        parts.append('</div>')

    parts.append('</body></html>')
    OUT_PATH.write_text("\n".join(parts))
    print(f"wrote {OUT_PATH} ({OUT_PATH.stat().st_size:,} bytes)", file=sys.stderr)

if __name__ == "__main__":
    main()
