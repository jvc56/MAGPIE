#!/usr/bin/env python3
"""Sortable HTML report for POND scenarios with a hoverable board.

For each scenario, embed the parsed PV (per-ply tile placements) and let
JS render the board with color overlays:
  - mover's cand tiles            -> cand (amber)
  - Noah's pick tiles             -> opp  (blue)
  - subsequent PV plies           -> pv1 (faded mover) / pv2 (faded opp)
"""

import csv
import html
import json
import re
import sys


PREMIUM_ROWS = [
    "tws,,,dls,,,,tws,,,,dls,,,tws",
    ",dws,,,,tls,,,,tls,,,,dws,",
    ",,dws,,,,dls,,dls,,,,dws,,",
    "dls,,,dws,,,,dls,,,,dws,,,dls",
    ",,,,dws,,,,,,dws,,,,",
    ",tls,,,,tls,,,,tls,,,,tls,",
    ",,dls,,,,dls,,dls,,,,dls,,",
    "tws,,,dls,,,,star,,,,dls,,,tws",
    ",,dls,,,,dls,,dls,,,,dls,,",
    ",tls,,,,tls,,,,tls,,,,tls,",
    ",,,,dws,,,,,,dws,,,,",
    "dls,,,dws,,,,dls,,,,dws,,,dls",
    ",,dws,,,,dls,,dls,,,,dws,,",
    ",dws,,,,tls,,,,tls,,,,dws,",
    "tws,,,dls,,,,tws,,,,dls,,,tws",
]
PREMIUM = [r.split(",") for r in PREMIUM_ROWS]


def parse_cgp_grid(cgp_pos):
    """cgp_pos: the first field of a CGP (15 rows separated by '/').
    Returns 15x15 grid; '.' for empty, [A-Z] for tile, [a-z] for blank-tile."""
    rows = cgp_pos.split("/")
    grid = []
    for raw in rows:
        cells = []
        i = 0
        while i < len(raw):
            ch = raw[i]
            if ch.isdigit():
                # run-length empties
                j = i
                while j < len(raw) and raw[j].isdigit():
                    j += 1
                cells.extend(["."] * int(raw[i:j]))
                i = j
            else:
                cells.append(ch)
                i += 1
        # pad / trim to 15
        while len(cells) < 15:
            cells.append(".")
        grid.append(cells[:15])
    while len(grid) < 15:
        grid.append(["."] * 15)
    return grid[:15]


_MOVE_POS = re.compile(r"^([A-O]\d+|\d+[A-O])\s+(\S+)\s+(-?\d+)$")


def parse_move(move_text):
    """Parse 'POS WORD SCORE' into list of placed-tile (row, col, letter)."""
    m = _MOVE_POS.match(move_text.strip())
    if not m:
        return []
    pos, word, _ = m.groups()
    if pos[0].isdigit():
        i = 0
        while i < len(pos) and pos[i].isdigit():
            i += 1
        row = int(pos[:i]) - 1
        col = ord(pos[i].upper()) - ord("A")
        direction = "H"
    else:
        col = ord(pos[0].upper()) - ord("A")
        row = int(pos[1:]) - 1
        direction = "V"
    placed = []
    cur_r, cur_c = row, col
    i = 0
    while i < len(word):
        ch = word[i]
        if ch == "(":
            j = word.index(")", i)
            n_skip = j - i - 1
            if direction == "H":
                cur_c += n_skip
            else:
                cur_r += n_skip
            i = j + 1
        else:
            if 0 <= cur_r < 15 and 0 <= cur_c < 15:
                placed.append((cur_r, cur_c, ch))
            if direction == "H":
                cur_c += 1
            else:
                cur_r += 1
            i += 1
    return placed


def load_tsv(path):
    out = {}
    with open(path, encoding="utf-8") as f:
        reader = csv.DictReader(f, delimiter="\t")
        for row in reader:
            key = (row["drawn"], row["remaining"])
            out[key] = {
                "weight": int(row["weight"]),
                "mt": int(row["mover_total"]),
                "pv": row.get("pv_text", "") or "",
                "final_cgp": row.get("final_cgp", "") or "",
                "mover_rack_end": row.get("mover_rack_end", "") or "",
                "opp_rack_end": row.get("opp_rack_end", "") or "",
            }
    return out


def split_pv(pv):
    if not pv:
        return []
    return [p.strip() for p in pv.split("|")]


def main(orig_cgp_first_field, cand_text, d0_path, d1_path, d2_path, out_path):
    orig_grid = parse_cgp_grid(orig_cgp_first_field)
    cand_tiles = parse_move(cand_text)

    d0 = load_tsv(d0_path)
    d1 = load_tsv(d1_path)
    d2 = load_tsv(d2_path)

    keys = sorted(set(d0) | set(d1) | set(d2))
    rows = []
    scen_data = {}  # JS keyed by "drawn|remaining"
    sum_w = sum_w_x2 = sum_spread = 0

    for k in keys:
        drawn, remaining = k
        ref = d2.get(k) or d1.get(k) or d0.get(k)
        w = ref["weight"]
        mt0 = d0.get(k, {}).get("mt")
        mt1 = d1.get(k, {}).get("mt")
        e2 = d2.get(k, {})
        mt2 = e2.get("mt")
        plies = split_pv(e2.get("pv", ""))
        pick = plies[0] if plies else ""
        egpv = " | ".join(plies[1:]) if len(plies) > 1 else ""

        # Per-ply tile placements for board overlays. Each tile is encoded
        # as {r, c, l} (matching CAND_TILES) so renderBoard can read t.r
        # / t.c / t.l uniformly. ply 0 = Noah's pick (opp). Endgame plies
        # follow, alternating mover, opp, ...
        ply_tiles = []
        for ply in plies:
            ply_tiles.append(
                [{"r": r, "c": c, "l": l} for (r, c, l) in parse_move(ply)]
            )

        if mt2 is not None:
            sum_w += w
            sum_spread += w * mt2
            if mt2 > 0:
                sum_w_x2 += 2 * w
                outcome = "WIN"
            elif mt2 == 0:
                sum_w_x2 += w
                outcome = "TIE"
            else:
                outcome = "LOSS"
        else:
            outcome = ""

        key_str = f"{drawn}|{remaining}"
        # `pv` is the full PV text (Noah's pick | endgame plies); JS
        # splits on '|' and pairs each text with scen.plies[i].
        scen_data[key_str] = {
            "drawn": drawn,
            "remaining": remaining,
            "weight": w,
            "mt2": mt2,
            "pick": pick,
            "pv": " | ".join(plies) if plies else "",
            "plies": ply_tiles,
            "mover_rack_end": e2.get("mover_rack_end", ""),
            "opp_rack_end": e2.get("opp_rack_end", ""),
        }
        rows.append({
            "drawn": drawn,
            "remaining": remaining,
            "weight": w,
            "mt0": mt0,
            "mt1": mt1,
            "mt2": mt2,
            "outcome": outcome,
            "pick": pick,
            "egpv": egpv,
            "key": key_str,
        })

    win_pct = sum_w_x2 / (2 * sum_w) if sum_w else 0.0
    mean_spread = sum_spread / sum_w if sum_w else 0.0
    losers = [r for r in rows if r["outcome"] == "LOSS"]

    def fmt(v):
        return "" if v is None else f"{v:+d}"

    body = []
    for r in rows:
        cls = "loss" if r["outcome"] == "LOSS" else (
            "tie" if r["outcome"] == "TIE" else "")
        egpv_html = ""
        if r["egpv"]:
            plies = [p.strip() for p in r["egpv"].split("|")]
            spans = []
            for i, p in enumerate(plies):
                pv_cls = "pv-mover" if i % 2 == 0 else "pv-opp"
                spans.append(f'<span class="{pv_cls}">{html.escape(p)}</span>')
            egpv_html = '<div class="cell-pv">' + "".join(spans) + "</div>"
        body.append(
            f'<tr class="scen-row {cls}" '
            f'data-key="{html.escape(r["key"])}" '
            f'onmouseenter="showScen(this.dataset.key)">'
            f'<td>{html.escape(r["drawn"])}</td>'
            f'<td>{html.escape(r["remaining"])}</td>'
            f'<td class="num">{r["weight"]}</td>'
            f'<td class="num">{fmt(r["mt0"])}</td>'
            f'<td class="num">{fmt(r["mt1"])}</td>'
            f'<td class="num">{fmt(r["mt2"])}</td>'
            f'<td class="rslt">{r["outcome"]}</td>'
            f'<td class="opp-pick">{html.escape(r["pick"])}</td>'
            f'<td class="egpv">{egpv_html}</td>'
            "</tr>"
        )
    body_html = "\n".join(body)

    losers_html = "<i>none</i>"
    if losers:
        items = []
        for r in losers:
            items.append(
                f'<li><b>{html.escape(r["drawn"])}/{html.escape(r["remaining"])}</b> '
                f'(weight {r["weight"]}, mt {fmt(r["mt2"])}): '
                f'<code>{html.escape(r["pick"])}</code> &rarr; '
                f'{html.escape(r["egpv"])}</li>'
            )
        losers_html = '<ul class="losers">' + "".join(items) + "</ul>"

    cand_tiles_json = json.dumps([{"r": t[0], "c": t[1], "l": t[2]} for t in cand_tiles])
    scen_json = json.dumps(scen_data)
    grid_json = json.dumps(orig_grid)
    premium_json = json.dumps(PREMIUM)

    doc = f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>POND scenarios — rational Noah, K=8</title>
<style>
body {{ font-family: -apple-system, Helvetica, sans-serif; margin: 20px;
       padding-right: 420px; color: #222; }}
h1, h2 {{ color: #333; }}
.summary {{ display: flex; gap: 12px; margin-bottom: 16px; flex-wrap: wrap; }}
.card {{ border: 1px solid #ddd; padding: 10px 12px; border-radius: 8px;
        min-width: 200px; background: #fafafa; font-size: 13px; }}
.card h3 {{ margin: 0 0 6px 0; font-size: 14px; }}
.card p {{ margin: 3px 0; }}
.card.best {{ border-color: #28a745; background: #f0fff0; }}
table.scen {{ border-collapse: collapse; margin-top: 10px; }}
table.scen th, table.scen td {{ border: 1px solid #ccc; padding: 3px 6px;
        text-align: center; font-family: monospace; font-size: 13px; }}
table.scen th {{ background: #eee; font-size: 11px; line-height: 1.1;
        font-weight: normal; cursor: pointer; user-select: none;
        position: sticky; top: 0; z-index: 1; }}
table.scen th:hover {{ background: #ddd; }}
table.scen th.sort-asc::after  {{ content: " ▲"; color: #888; }}
table.scen th.sort-desc::after {{ content: " ▼"; color: #888; }}
table.scen td.num {{ text-align: right; }}
table.scen td.opp-pick, table.scen td.egpv {{ text-align: left;
        font-size: 11px; max-width: 360px; }}
tr.scen-row:hover td {{ background: #fff8b0; }}
.loss td {{ background: #fdd; color: #c00; }}
.loss .rslt {{ font-weight: bold; }}
.tie td {{ background: #ffd; }}
.cell-pv {{ display: inline-flex; flex-wrap: wrap; gap: 3px; }}
.cell-pv .pv-mover {{ background: #f0d3a8; color: #6b4a14;
                      padding: 0 3px; border-radius: 2px; }}
.cell-pv .pv-opp {{ background: #cde0f0; color: #2b4a66;
                    padding: 0 3px; border-radius: 2px; }}
/* Hover panel: fixed on the right. */
#hover-panel {{ position: fixed; right: 16px; top: 16px; width: 390px;
                border: 1px solid #ddd; background: #fff; padding: 10px;
                border-radius: 8px; z-index: 100;
                box-shadow: 0 2px 8px rgba(0,0,0,0.06);
                font-family: -apple-system, Helvetica, sans-serif;
                font-size: 13px; }}
table.board {{ border-collapse: collapse; font-family: monospace; }}
table.board td, table.board th {{ width: 22px; height: 22px;
        text-align: center; vertical-align: middle;
        border: 1px solid #999; font-size: 11px; }}
table.board td.tile        {{ background: #f4e4bc; color: #000;
                              font-weight: bold; font-size: 14px; }}
table.board td.blank       {{ background: #f4e4bc; color: #9a9a9a;
                              font-weight: bold; font-size: 14px; }}
table.board td.tile.opp    {{ background: #b0d8ff; color: #003366; }}
table.board td.blank.opp   {{ background: #b0d8ff; color: #5a7a99; }}
table.board td.tile.cand   {{ background: #ffd28a; color: #663300; }}
table.board td.blank.cand  {{ background: #ffd28a; color: #8a6633; }}
table.board td.tile.pv1    {{ background: #f0d3a8; color: #6b4a14; }}
table.board td.blank.pv1   {{ background: #f0d3a8; color: #8c7350; }}
table.board td.tile.pv2    {{ background: #cde0f0; color: #2b4a66; }}
table.board td.blank.pv2   {{ background: #cde0f0; color: #5a7186; }}
table.board td.empty.tws   {{ background: #ff8a80; }}
table.board td.empty.dws   {{ background: #ffcdd2; }}
table.board td.empty.tls   {{ background: #82b1ff; }}
table.board td.empty.dls   {{ background: #bbdefb; }}
table.board td.empty.star  {{ background: #ffcdd2; }}
.losers code {{ background: #f5f5f5; padding: 0 4px; border-radius: 3px; }}
.rack-strip {{ margin-top: 8px; }}
.rack-strip .row {{ display: flex; align-items: center; gap: 4px;
                   margin-bottom: 4px; min-height: 24px; }}
.rack-strip .row .label {{ width: 50px; font-size: 11px; color: #555; }}
.rack-tile {{ display: inline-block; width: 22px; height: 22px;
              line-height: 22px; text-align: center; font-family: monospace;
              font-weight: bold; font-size: 14px; border: 1px solid #999;
              border-radius: 3px; }}
.rack-tile.mover {{ background: #ffd28a; color: #663300; }}
.rack-tile.opp   {{ background: #b0d8ff; color: #003366; }}
.rack-tile.blank {{ font-style: italic; }}
.rack-tile.blank.mover {{ color: #a07d40; }}
.rack-tile.blank.opp   {{ color: #5f7e9a; }}
/* PV display: per-ply colored by player + going-out leftover/bonus. */
.pv-line {{ margin-top: 4px; }}
.pv-ply {{ display: flex; align-items: center; gap: 4px;
           margin-bottom: 3px; font-family: monospace; font-size: 12px; }}
.pv-ply .pv-text {{ padding: 1px 4px; border-radius: 2px; }}
.pv-ply.pv-mover .pv-text {{ background: #f0d3a8; color: #6b4a14; }}
.pv-ply.pv-opp   .pv-text {{ background: #cde0f0; color: #2b4a66; }}
.pv-leftover {{ padding: 1px 4px; border-radius: 2px;
                font-family: monospace; font-size: 12px; }}
.pv-leftover.mover {{ background: #f0d3a8; color: #6b4a14; }}
.pv-leftover.opp   {{ background: #cde0f0; color: #2b4a66; }}
.pv-bonus-num {{ padding: 1px 5px; border-radius: 2px;
                 font-weight: bold; font-size: 12px; margin-left: 2px;
                 font-family: monospace; }}
.pv-bonus-num.mover {{ background: #ffd28a; color: #663300; }}
.pv-bonus-num.opp   {{ background: #b0d8ff; color: #003366; }}
#scen-info {{ font-family: monospace; font-size: 12px;
              line-height: 1.5; margin-top: 8px; }}
#scen-info .label {{ color: #777; font-size: 11px; }}
</style>
</head>
<body>

<div id="hover-panel">
  <div id="scen-board"></div>
  <div id="scen-info"><div class="label">hover a scenario row to see Noah's reply.</div></div>
</div>

<h1>P(O)ND deep-oracle comparison (rational Noah, K=8 halving)</h1>

<div class="summary">
<div class="card best">
<h3>2L P(O)ND 14</h3>
<p><b>scenarios</b>: {len(rows)}</p>
<p><b>weight_sum</b>: {sum_w}</p>
<p><b>d=2 win %</b>: {win_pct*100:.2f}%</p>
<p><b>d=2 spread</b>: {mean_spread:+.3f}</p>
<p><b>losses</b>: {len(losers)} ({sum(r["weight"] for r in losers)} weighted)</p>
</div>
<div class="card">
<h3>Model</h3>
<p>4-in-bag, K_drawn ∈ {{1..4}}</p>
<p>Rational Noah: utility = win% + 1e-4·spread, averaged over perceived bag-tile types (weighted by physical-tile counts)</p>
<p>Halving from K=8 → K/2 → … → MIN at depth</p>
<p>Realized mt at d=2 reported</p>
</div>
</div>

<h2>Losing scenarios</h2>
{losers_html}

<h2>Per-scenario detail (click any header to sort, hover row for board)</h2>
<table class="scen" id="scen">
<thead>
<tr>
  <th data-type="str">drawn</th>
  <th data-type="str">rem</th>
  <th data-type="num">scen<br>wght</th>
  <th data-type="num">d=0<br>mt</th>
  <th data-type="num">d=1<br>rat mt</th>
  <th data-type="num">d=2<br>rat mt</th>
  <th data-type="str">d=2<br>rslt</th>
  <th data-type="str">Noah's pick<br>(d=2)</th>
  <th data-type="str">endgame PV<br>(d=2)</th>
</tr>
</thead>
<tbody>
{body_html}
</tbody>
</table>

<script>
const ORIGINAL_GRID = {grid_json};
const PREMIUM = {premium_json};
const CAND_TILES = {cand_tiles_json};
const SCENARIOS = {scen_json};

function renderBoard(scen) {{
  // Start from ORIGINAL_GRID; overlay cand, then per-ply tiles.
  // colorClass[r][c]: 'cand' | 'opp' | 'pv1' | 'pv2' | null
  const grid = ORIGINAL_GRID.map(r => r.slice());
  const colorClass = Array.from({{length: 15}}, () => new Array(15).fill(null));
  for (const t of CAND_TILES) {{
    grid[t.r][t.c] = t.l;
    colorClass[t.r][t.c] = 'cand';
  }}
  if (scen && scen.plies) {{
    for (let i = 0; i < scen.plies.length; i++) {{
      const ply = scen.plies[i];
      // ply 0 = Noah (opp); ply 1 = mover-pv1; ply 2 = opp-pv2; ...
      let cls;
      if (i === 0) cls = 'opp';
      else if (i % 2 === 1) cls = 'pv1';
      else cls = 'pv2';
      for (const t of ply) {{
        grid[t.r][t.c] = t.l;
        colorClass[t.r][t.c] = cls;
      }}
    }}
  }}
  // Build table HTML.
  let h = '<table class="board"><tbody>';
  h += '<tr><th></th>';
  for (let c = 0; c < 15; c++) h += '<th>' + String.fromCharCode(65 + c) + '</th>';
  h += '</tr>';
  for (let r = 0; r < 15; r++) {{
    h += '<tr><th>' + (r + 1) + '</th>';
    for (let c = 0; c < 15; c++) {{
      const ch = grid[r][c];
      const isEmpty = (ch === '.');
      const classes = [];
      if (isEmpty) {{
        if (PREMIUM[r][c]) classes.push(PREMIUM[r][c]);
        classes.push('empty');
      }} else {{
        const blank = (ch >= 'a' && ch <= 'z');
        classes.push(blank ? 'blank' : 'tile');
        if (colorClass[r][c]) classes.push(colorClass[r][c]);
      }}
      const disp = isEmpty ? '&nbsp;' : ch.toUpperCase();
      h += '<td class="' + classes.join(' ') + '">' + disp + '</td>';
    }}
    h += '</tr>';
  }}
  h += '</tbody></table>';
  return h;
}}

const TILE_VALUES = {{A:1,B:3,C:3,D:2,E:1,F:4,G:2,H:4,I:1,J:8,K:5,L:1,
                     M:3,N:1,O:1,P:3,Q:10,R:1,S:1,T:1,U:1,V:4,W:4,X:8,
                     Y:4,Z:10,'?':0}};

function rackValue(rack) {{
  let v = 0;
  for (const ch of rack) v += TILE_VALUES[ch.toUpperCase()] || 0;
  return v;
}}

function renderGoingOut(moverRackEnd, oppRackEnd) {{
  if (!moverRackEnd && !oppRackEnd) return '';
  let leftRack, leftWho, bonusWho;
  if (!moverRackEnd && oppRackEnd) {{
    leftRack = oppRackEnd; leftWho = 'opp'; bonusWho = 'mover';
  }} else if (moverRackEnd && !oppRackEnd) {{
    leftRack = moverRackEnd; leftWho = 'mover'; bonusWho = 'opp';
  }} else {{
    return '';
  }}
  const bonus = 2 * rackValue(leftRack);
  let h = '<span class="pv-leftover ' + leftWho + '">(' + leftRack + ')</span>';
  h += '<span class="pv-bonus-num ' + bonusWho + '">+' + bonus + '</span>';
  return h;
}}

function renderPV(scen) {{
  // scen.plies = list of placed-tile arrays per ply, starting with
  // Noah's pick (opp). The endgame PV that follows alternates
  // mover, opp, mover, ...
  const allTexts = (scen.pv || '').split('|').map(s => s.trim());
  if (allTexts.length === 0) return '';
  let h = '<div class="pv-line">';
  for (let i = 0; i < allTexts.length; i++) {{
    // Ply 0 = Noah (opp); 1 = mover; 2 = opp; ...
    const isMover = (i % 2 === 1);
    const plyCls = isMover ? 'pv-mover' : 'pv-opp';
    h += '<div class="pv-ply ' + plyCls + '">';
    h += '<span class="pv-text">' + allTexts[i] + '</span>';
    if (i === allTexts.length - 1) {{
      h += renderGoingOut(scen.mover_rack_end, scen.opp_rack_end);
    }}
    h += '</div>';
  }}
  h += '</div>';
  return h;
}}

function showScen(key) {{
  const scen = SCENARIOS[key];
  if (!scen) return;
  document.getElementById('scen-board').innerHTML = renderBoard(scen);
  const mtStr = (scen.mt2 === null) ? '?' :
                ((scen.mt2 >= 0 ? '+' : '') + scen.mt2);
  const info = document.getElementById('scen-info');
  info.innerHTML =
    '<div><span class="label">drawn:</span> ' + scen.drawn +
    ' &nbsp; <span class="label">remaining:</span> ' + scen.remaining +
    ' &nbsp; <span class="label">weight:</span> ' + scen.weight + '</div>' +
    '<div><span class="label">mover_total:</span> ' + mtStr +
    ' &nbsp; <span class="label">Noah pick:</span> ' + (scen.pick || '') +
    '</div>' +
    (scen.pv ? '<div><span class="label">PV:</span></div>' +
       renderPV(scen) : '');
}}

// Initial board: just the post-cand state (no scenario).
document.getElementById('scen-board').innerHTML = renderBoard(null);

// Sortable headers.
(function () {{
  const table = document.getElementById("scen");
  const thead = table.tHead;
  const tbody = table.tBodies[0];
  thead.addEventListener("click", (ev) => {{
    const th = ev.target.closest("th");
    if (!th) return;
    const idx = Array.from(th.parentNode.children).indexOf(th);
    const type = th.dataset.type || "str";
    const cur = th.classList.contains("sort-asc") ? "asc" :
                th.classList.contains("sort-desc") ? "desc" : null;
    const dir = cur === "asc" ? "desc" : "asc";
    Array.from(thead.querySelectorAll("th")).forEach(h =>
      h.classList.remove("sort-asc", "sort-desc"));
    th.classList.add(dir === "asc" ? "sort-asc" : "sort-desc");
    const sign = dir === "asc" ? 1 : -1;
    // d=2 mt is at column index 5; the rslt column at index 6 tie-breaks
    // by that.
    const MT2_COL = 5;
    const RSLT_COL = 6;
    const rows = Array.from(tbody.rows);
    rows.sort((a, b) => {{
      const av = a.cells[idx].innerText.trim();
      const bv = b.cells[idx].innerText.trim();
      let primary;
      if (type === "num") {{
        const an = av === "" ? -Infinity : parseFloat(av);
        const bn = bv === "" ? -Infinity : parseFloat(bv);
        primary = an - bn;
      }} else {{
        primary = av.localeCompare(bv);
      }}
      if (primary !== 0 || idx !== RSLT_COL) {{
        return sign * primary;
      }}
      // Tie-break on rslt column: by d=2 rat mt (same direction).
      const am = parseFloat(a.cells[MT2_COL].innerText.trim());
      const bm = parseFloat(b.cells[MT2_COL].innerText.trim());
      return sign * ((isNaN(am) ? -Infinity : am) -
                     (isNaN(bm) ? -Infinity : bm));
    }});
    const frag = document.createDocumentFragment();
    rows.forEach(r => frag.appendChild(r));
    tbody.appendChild(frag);
  }});
}})();
</script>
</body>
</html>
"""
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(doc)
    print(f"wrote {out_path}")
    print(f"summary: scen={len(rows)} weight_sum={sum_w} "
          f"d=2 win%={win_pct*100:.2f} d=2 spread={mean_spread:+.3f} "
          f"losses={len(losers)}")


if __name__ == "__main__":
    # args: <orig_cgp_first_field> <cand_text> <d0> <d1> <d2> <out>
    main(*sys.argv[1:])
