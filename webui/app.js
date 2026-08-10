// MAGPIE analysis board — mouse-oriented web UI over the WASM engine.
//
// Talks to the existing wasmentry/wasm-worker.js (extended with JSON getters).
// The board/racks are edited client-side; before each analysis run we serialize
// the position to a CGP string and sync it into the engine, then read structured
// JSON back for rendering.

import { Board, tilePoints } from './board.js';

const WORKER_URL = '../wasmentry/wasm-worker.js';
const DATA_ROOT = '/data';
const RACK_SIZE = 7;

// English lexica we ship the data files for. All map to the english letter
// distribution and the standard 15x15 layout.
const LEXICA = ['CSW21', 'CSW24', 'NWL23', 'NWL20', 'TWL14', 'TWL06', 'TWL98'];

const SAMPLES = {
  Midgame:
    'C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 -lex NWL23',
  Endgame:
    'GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW24',
};

// ---------------------------------------------------------------------------
// Worker bridge: a small promise-based RPC over postMessage.
// ---------------------------------------------------------------------------
class Bridge {
  constructor(url, handlers) {
    this.handlers = handlers || {};
    this.worker = new Worker(url, { type: 'module' });
    this._pending = null; // sequential op: { resolve, reject, expect, outputs }
    this._jsonWaiters = new Map(); // requestId -> resolve
    this._reqId = 0;
    this.ready = new Promise((res) => {
      this._resolveReady = res;
    });
    this.worker.onmessage = (e) => this._route(e.data);
    this.worker.onerror = (err) => {
      const text = err && (err.message || err.filename) ? `${err.message || ''} ${err.filename || ''}` : 'worker error';
      if (this.handlers.onError) {
        this.handlers.onError(text);
      }
      if (this._pending) {
        const p = this._pending;
        this._pending = null;
        p.reject(new Error(text));
      }
      // Settle any outstanding JSON getters so their awaiters don't hang if the
      // worker dies outright.
      for (const [, resolve] of this._jsonWaiters) {
        resolve({ ok: false, error: text });
      }
      this._jsonWaiters.clear();
    };
  }

  _route(msg) {
    const { type } = msg;
    if (type === 'ready') {
      this._resolveReady();
      return;
    }
    if (type === 'log') {
      if (this.handlers.onLog) {
        this.handlers.onLog(msg.text);
      }
      return;
    }
    if (type === 'status') {
      if (this.handlers.onStatus) {
        this.handlers.onStatus(msg.text);
      }
      return;
    }
    if (type === 'output') {
      if (this._pending) {
        this._pending.outputs.push(msg.text);
      }
      if (this.handlers.onOutput) {
        this.handlers.onOutput(msg.text);
      }
      return;
    }
    if (type === 'error') {
      if (this.handlers.onError) {
        this.handlers.onError(msg.text);
      }
      if (this._pending) {
        const p = this._pending;
        this._pending = null;
        p.reject(new Error(msg.text));
      }
      return;
    }
    if (type === 'state' || type === 'moves' || type === 'endgame') {
      const cb = this._jsonWaiters.get(msg.requestId);
      if (cb) {
        this._jsonWaiters.delete(msg.requestId);
        cb(msg.data);
      }
      return;
    }
    if (this._pending && type === this._pending.expect) {
      const p = this._pending;
      this._pending = null;
      p.resolve({ outputs: p.outputs });
    }
  }

  _op(type, data, expect) {
    return new Promise((resolve, reject) => {
      this._pending = { resolve, reject, expect, outputs: [] };
      this.worker.postMessage({ type, data });
    });
  }

  init(dataPath) {
    return this._op('init', { dataPath }, 'init_complete');
  }

  precache(filename, url) {
    return this._op('precache', { filename, url }, 'precache_complete');
  }

  runCommands(commands) {
    return this._op('run', { commands }, 'complete');
  }

  getJson(kind) {
    const requestId = ++this._reqId;
    const type = { getState: 'state', getMoves: 'moves', getEndgame: 'endgame' }[kind];
    return new Promise((resolve) => {
      this._jsonWaiters.set(requestId, resolve);
      this.worker.postMessage({ type: kind, data: { requestId } });
    });
  }

  stop() {
    this.worker.postMessage({ type: 'stop' });
  }
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
const $ = (id) => document.getElementById(id);

const state = {
  lexicon: 'CSW21',
  onTurn: 0,
  players: [
    { rack: '', score: 0 },
    { rack: '', score: 0 },
  ],
  bagCount: 0,
  cgp: '',
  moves: [],
  selectedMove: null,
  sortKey: 'equity',
  sortDir: -1,
  hasSim: false,
  blankMode: false,
  focus: { kind: 'board' },
  busy: false,
};

const precached = new Set();
let board = null;
let bridge = null;
let threads = Math.min(Math.max(navigator.hardwareConcurrency || 4, 1), 16);

function setStatus(text, kind = '') {
  const el = $('status');
  el.textContent = text;
  el.className = 'status' + (kind ? ' ' + kind : '');
}

function logLine(text) {
  const el = $('results');
  el.textContent += text + '\n';
  el.scrollTop = el.scrollHeight;
}

function clearResults() {
  $('results').textContent = '';
}

function setBusy(busy) {
  state.busy = busy;
  document.querySelectorAll('button[data-act]').forEach((b) => {
    b.disabled = busy;
  });
  if (board) {
    board.setEditable(!busy);
  }
  $('stop').disabled = !busy;
}

// --- CGP serialization -----------------------------------------------------
function buildCgp() {
  const boardStr = board.cgpBoard();
  const on = state.onTurn;
  const off = 1 - on;
  const rackOn = state.players[on].rack || '';
  const rackOff = state.players[off].rack || '';
  const scoreOn = state.players[on].score | 0;
  const scoreOff = state.players[off].score | 0;
  return `${boardStr} ${rackOn}/${rackOff} ${scoreOn}/${scoreOff} 0`;
}

// --- Lexicon / data files --------------------------------------------------
function lexFiles(lex) {
  return [
    `data/lexica/${lex}.kwg`,
    `data/lexica/${lex}.klv2`,
    'data/letterdistributions/english.csv',
    'data/strategy/winpct.csv',
    'data/layouts/standard15.txt',
  ];
}

async function ensureLexicon(lex) {
  for (const filename of lexFiles(lex)) {
    if (precached.has(filename)) {
      continue;
    }
    await bridge.precache(filename, `${DATA_ROOT}/${filename.slice('data/'.length)}`);
    precached.add(filename);
  }
}

// --- Apply engine state to the UI ------------------------------------------
function applyState(data) {
  if (!data || data.ok === false) {
    setStatus('Engine state unavailable' + (data && data.error ? `: ${data.error}` : ''), 'error');
    return;
  }
  board.load(data);
  if (Array.isArray(data.players) && data.players.length === 2) {
    state.players = data.players.map((p) => ({ rack: p.rack || '', score: p.score | 0, tiles: p.tiles || [] }));
  }
  if (typeof data.onTurn === 'number') {
    state.onTurn = data.onTurn;
  }
  if (typeof data.bagCount === 'number') {
    state.bagCount = data.bagCount;
  }
  if (data.cgp) {
    state.cgp = data.cgp;
  }
  if (data.lexicon) {
    state.lexicon = data.lexicon;
    const sel = $('lexicon');
    if (sel.value !== data.lexicon && LEXICA.includes(data.lexicon)) {
      sel.value = data.lexicon;
    }
  }
  renderPlayers();
  renderMeta();
}

function applyMoves(data) {
  if (!data || data.ok === false) {
    state.moves = [];
    state.hasSim = false;
    renderMoves();
    return;
  }
  state.moves = data.hasMoves ? data.moves : [];
  state.hasSim = !!data.hasSim;
  state.selectedMove = null;
  board.setHighlight(null);
  board.setPreview(null);
  if (state.hasSim) {
    state.sortKey = 'win';
  } else {
    state.sortKey = 'equity';
  }
  state.sortDir = -1;
  renderMoves();
  const sub = data.hasSim ? ` · sim ${formatInt(data.iterations)} iters` : '';
  setStatus(`${data.total || state.moves.length} plays${sub}`, 'success');
}

// Clears the play list and any board preview/highlight. Called after committing
// a move and whenever the board or a rack is edited (which makes the engine's
// generated move list — and thus a pending Play — stale).
function clearPlays() {
  if (state.moves.length === 0 && !state.selectedMove) {
    return;
  }
  state.moves = [];
  state.hasSim = false;
  state.selectedMove = null;
  board.setPreview(null);
  board.setHighlight(null);
  renderMoves();
}

// --- Rendering: players, meta ----------------------------------------------
function renderPlayers() {
  const container = $('players');
  container.innerHTML = '';
  // On-turn card first.
  const order = [state.onTurn, 1 - state.onTurn];
  for (const idx of order) {
    const player = state.players[idx];
    const card = document.createElement('div');
    card.className = 'player-card' + (idx === state.onTurn ? ' on-turn' : '');

    const head = document.createElement('div');
    head.className = 'player-head';
    const label = document.createElement('span');
    label.className = 'player-label';
    label.textContent = idx === state.onTurn ? '● To move' : 'Opponent';
    head.appendChild(label);

    const scoreWrap = document.createElement('label');
    scoreWrap.className = 'score';
    scoreWrap.textContent = 'Score ';
    const scoreInput = document.createElement('input');
    scoreInput.type = 'number';
    scoreInput.value = String(player.score | 0);
    scoreInput.addEventListener('change', () => {
      player.score = parseInt(scoreInput.value, 10) || 0;
    });
    scoreWrap.appendChild(scoreInput);
    head.appendChild(scoreWrap);
    card.appendChild(head);

    const rackRow = document.createElement('div');
    rackRow.className = 'rack' + (state.focus.kind === 'rack' && state.focus.index === idx ? ' focused' : '');
    rackRow.title = 'Click to focus, then use the tile palette. Click a tile to remove it.';
    rackRow.addEventListener('click', (e) => {
      if (e.target === rackRow || e.target.classList.contains('rack-empty')) {
        focusRack(idx);
      }
    });
    const tiles = parseRackTiles(player.rack);
    if (tiles.length === 0) {
      const empty = document.createElement('span');
      empty.className = 'rack-empty';
      empty.textContent = 'empty rack — click to edit';
      rackRow.appendChild(empty);
    } else {
      tiles.forEach((t, ti) => {
        const tEl = document.createElement('span');
        tEl.className = 'tile-face' + (t === '?' ? ' blank' : '');
        tEl.textContent = t;
        tEl.title = 'Remove';
        const points = tilePoints(t, t === '?');
        if (points != null && points > 0) {
          const pt = document.createElement('span');
          pt.className = 'pt';
          pt.textContent = String(points);
          tEl.appendChild(pt);
        }
        tEl.addEventListener('click', () => {
          const arr = parseRackTiles(player.rack);
          arr.splice(ti, 1);
          player.rack = arr.join('');
          clearPlays();
          renderPlayers();
        });
        rackRow.appendChild(tEl);
      });
    }
    card.appendChild(rackRow);

    const tools = document.createElement('div');
    tools.className = 'rack-tools';
    const input = document.createElement('input');
    input.type = 'text';
    input.className = 'rack-input';
    input.placeholder = 'type rack (use ? for blank)';
    input.value = player.rack;
    input.maxLength = RACK_SIZE;
    input.addEventListener('input', () => {
      player.rack = input.value.toUpperCase().replace(/[^A-Z?]/g, '').slice(0, RACK_SIZE);
      input.value = player.rack;
      clearPlays();
    });
    input.addEventListener('change', () => renderPlayers());
    tools.appendChild(input);

    const focusBtn = document.createElement('button');
    focusBtn.type = 'button';
    focusBtn.className = 'mini';
    focusBtn.textContent = 'Palette';
    focusBtn.addEventListener('click', () => focusRack(idx));
    tools.appendChild(focusBtn);

    if (idx === state.onTurn) {
      const randomBtn = document.createElement('button');
      randomBtn.type = 'button';
      randomBtn.className = 'mini';
      randomBtn.dataset.act = 'random';
      randomBtn.textContent = 'Random';
      randomBtn.addEventListener('click', () => randomRack());
      tools.appendChild(randomBtn);
    }

    const swapBtn = document.createElement('button');
    swapBtn.type = 'button';
    swapBtn.className = 'mini';
    swapBtn.textContent = idx === state.onTurn ? '↕ swap to move' : 'make to move';
    swapBtn.addEventListener('click', () => {
      state.onTurn = 1 - state.onTurn;
      renderPlayers();
    });
    tools.appendChild(swapBtn);

    card.appendChild(tools);
    container.appendChild(card);
  }
}

function renderMeta() {
  $('meta').textContent = `${state.lexicon} · bag ${state.bagCount}`;
}

function parseRackTiles(rack) {
  return (rack || '').split('');
}

function focusRack(idx) {
  state.focus = { kind: 'rack', index: idx };
  board.clearCursor();
  renderPlayers();
  updatePaletteHint();
}

// --- Move list -------------------------------------------------------------
function sortedMoves() {
  const moves = state.moves.slice();
  const key = state.sortKey;
  const dir = state.sortDir;
  moves.sort((a, b) => {
    let av;
    let bv;
    if (key === 'win') {
      av = a.win == null ? -Infinity : a.win;
      bv = b.win == null ? -Infinity : b.win;
    } else if (key === 'score') {
      av = a.score;
      bv = b.score;
    } else if (key === 'equity') {
      const ae = state.hasSim && a.eqMean != null ? a.eqMean : a.equity;
      const be = state.hasSim && b.eqMean != null ? b.eqMean : b.equity;
      av = ae == null ? -Infinity : ae;
      bv = be == null ? -Infinity : be;
    } else {
      av = a.i;
      bv = b.i;
    }
    if (av < bv) {
      return -dir;
    }
    if (av > bv) {
      return dir;
    }
    return a.i - b.i;
  });
  return moves;
}

function renderMoves() {
  const tbody = $('moves-body');
  tbody.innerHTML = '';
  const head = $('moves-head');
  head.querySelector('[data-col="win"]').style.display = state.hasSim ? '' : 'none';
  const moves = sortedMoves();
  if (moves.length === 0) {
    const tr = document.createElement('tr');
    const td = document.createElement('td');
    td.colSpan = 6;
    td.className = 'empty';
    td.textContent = 'No plays yet — set a rack and click Generate or Simulate.';
    tr.appendChild(td);
    tbody.appendChild(tr);
    return;
  }
  for (const move of moves) {
    const tr = document.createElement('tr');
    if (state.selectedMove && state.selectedMove.i === move.i) {
      tr.className = 'selected';
    }
    tr.appendChild(td(move.desc, 'play'));
    tr.appendChild(td(String(move.score), 'num'));
    // After a sim, prefer the simmed mean equity; otherwise the static equity.
    const eq = state.hasSim && move.eqMean != null ? move.eqMean : move.equity;
    tr.appendChild(td(eq == null ? '—' : eq.toFixed(1), 'num'));
    const winTd = td(move.win == null ? '—' : move.win.toFixed(1), 'num');
    winTd.style.display = state.hasSim ? '' : 'none';
    tr.appendChild(winTd);
    tr.appendChild(td(move.leave || '', 'leave'));
    const playTd = document.createElement('td');
    const playBtn = document.createElement('button');
    playBtn.type = 'button';
    playBtn.className = 'mini';
    playBtn.dataset.act = 'play';
    playBtn.textContent = 'Play';
    playBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      playMove(move);
    });
    playTd.appendChild(playBtn);
    tr.appendChild(playTd);

    tr.addEventListener('mouseenter', () => board.setPreview(move.placed));
    tr.addEventListener('mouseleave', () => board.setPreview(state.selectedMove ? state.selectedMove.placed : null));
    tr.addEventListener('click', () => selectMove(move));
    tbody.appendChild(tr);
  }
}

function td(text, cls) {
  const cell = document.createElement('td');
  cell.textContent = text;
  if (cls) {
    cell.className = cls;
  }
  return cell;
}

function selectMove(move) {
  state.selectedMove = move;
  board.setHighlight(move.placed);
  board.setPreview(move.placed);
  renderMoves();
}

// --- Palette ---------------------------------------------------------------
function buildPalette() {
  const palette = $('palette');
  palette.innerHTML = '';
  const letters = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ'.split('');
  for (const letter of letters) {
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'pal';
    b.textContent = letter;
    b.addEventListener('click', () => paletteKey(letter));
    palette.appendChild(b);
  }
  const blank = document.createElement('button');
  blank.type = 'button';
  blank.className = 'pal pal-blank';
  blank.id = 'pal-blank';
  blank.textContent = 'Blank';
  blank.addEventListener('click', () => paletteBlank());
  palette.appendChild(blank);

  const back = document.createElement('button');
  back.type = 'button';
  back.className = 'pal pal-wide';
  back.textContent = '⌫';
  back.title = 'Backspace';
  back.addEventListener('click', () => paletteBackspace());
  palette.appendChild(back);

  const clear = document.createElement('button');
  clear.type = 'button';
  clear.className = 'pal pal-wide';
  clear.textContent = 'Clear';
  clear.title = 'Clear board / rack';
  clear.addEventListener('click', () => paletteClear());
  palette.appendChild(clear);

  updatePaletteHint();
}

function updatePaletteHint() {
  const hint = $('palette-hint');
  const blankBtn = $('pal-blank');
  if (state.focus.kind === 'rack') {
    hint.textContent = `Editing ${state.focus.index === state.onTurn ? 'to-move' : 'opponent'} rack — letters add tiles, Blank adds "?".`;
    if (blankBtn) {
      blankBtn.classList.remove('active');
    }
  } else {
    hint.textContent = board.cursor
      ? `Placing at ${cellName(board.cursor)} (${board.cursor.dir === 'h' ? 'across' : 'down'}) — click the square again to flip direction.`
      : 'Click a board square to start placing tiles.';
    if (blankBtn) {
      blankBtn.classList.toggle('active', state.blankMode);
    }
  }
}

function cellName(cur) {
  return 'ABCDEFGHIJKLMNOPQRSTUVWXY'[cur.col] + (cur.row + 1);
}

function paletteKey(letter) {
  if (state.busy) {
    return;
  }
  if (state.focus.kind === 'rack') {
    const player = state.players[state.focus.index];
    if (parseRackTiles(player.rack).length < RACK_SIZE) {
      player.rack = (player.rack + letter).toUpperCase();
      clearPlays();
      renderPlayers();
    }
    return;
  }
  if (!board.cursor) {
    setStatus('Click a board square first to place tiles.', '');
    return;
  }
  board.placeLetter(letter, state.blankMode);
  if (state.blankMode) {
    state.blankMode = false;
  }
  updatePaletteHint();
}

function paletteBlank() {
  if (state.busy) {
    return;
  }
  if (state.focus.kind === 'rack') {
    const player = state.players[state.focus.index];
    if (parseRackTiles(player.rack).length < RACK_SIZE) {
      player.rack = player.rack + '?';
      clearPlays();
      renderPlayers();
    }
    return;
  }
  state.blankMode = !state.blankMode;
  updatePaletteHint();
}

function paletteBackspace() {
  if (state.busy) {
    return;
  }
  if (state.focus.kind === 'rack') {
    const player = state.players[state.focus.index];
    const arr = parseRackTiles(player.rack);
    arr.pop();
    player.rack = arr.join('');
    clearPlays();
    renderPlayers();
    return;
  }
  board.backspace();
}

function paletteClear() {
  if (state.busy) {
    return;
  }
  if (state.focus.kind === 'rack') {
    state.players[state.focus.index].rack = '';
    clearPlays();
    renderPlayers();
    return;
  }
  board.clearBoard();
}

// --- Keyboard (bonus; mouse is primary) ------------------------------------
function onKeydown(e) {
  if (e.target.tagName === 'INPUT' || e.target.tagName === 'SELECT' || e.target.tagName === 'TEXTAREA') {
    return;
  }
  if (state.busy) {
    return;
  }
  if (state.focus.kind !== 'board' || !board.cursor) {
    return;
  }
  if (/^[a-zA-Z]$/.test(e.key)) {
    board.placeLetter(e.key.toUpperCase(), e.shiftKey || state.blankMode);
    state.blankMode = false;
    updatePaletteHint();
    e.preventDefault();
  } else if (e.key === 'Backspace') {
    board.backspace();
    e.preventDefault();
  } else if (e.key === ' ') {
    board.setCursor(board.cursor.row, board.cursor.col, board.cursor.dir === 'h' ? 'v' : 'h');
    updatePaletteHint();
    e.preventDefault();
  } else if (e.key === 'Escape') {
    board.clearCursor();
    updatePaletteHint();
  } else if (e.key.startsWith('Arrow')) {
    const cur = board.cursor;
    let { row, col } = cur;
    if (e.key === 'ArrowUp') {
      row = Math.max(0, row - 1);
    } else if (e.key === 'ArrowDown') {
      row = Math.min(board.dim - 1, row + 1);
    } else if (e.key === 'ArrowLeft') {
      col = Math.max(0, col - 1);
    } else if (e.key === 'ArrowRight') {
      col = Math.min(board.dim - 1, col + 1);
    }
    board.setCursor(row, col, cur.dir);
    updatePaletteHint();
    e.preventDefault();
  }
}

// --- Engine actions --------------------------------------------------------
async function syncPosition() {
  await ensureLexicon(state.lexicon);
  const cgp = buildCgp();
  const res = await bridge.runCommands([`set -lex ${state.lexicon}`, `cgp ${cgp}`]);
  return res;
}

async function withBusy(label, fn) {
  if (state.busy) {
    return;
  }
  setBusy(true);
  setStatus(label + '…');
  try {
    await fn();
  } catch (err) {
    setStatus(`${label} failed: ${err.message}`, 'error');
    logLine(`❌ ${err.message}`);
  } finally {
    setBusy(false);
  }
}

function generate() {
  return withBusy('Generating', async () => {
    await ensureLexicon(state.lexicon);
    const cgp = buildCgp();
    await bridge.runCommands([`set -lex ${state.lexicon}`, `cgp ${cgp}`, 'generate']);
    applyState(await bridge.getJson('getState'));
    applyMoves(await bridge.getJson('getMoves'));
  });
}

function simulate() {
  return withBusy('Simulating', async () => {
    await ensureLexicon(state.lexicon);
    const cgp = buildCgp();
    const plies = parseInt($('opt-plies').value, 10) || 2;
    const tlim = parseInt($('opt-tlim').value, 10) || 5;
    await bridge.runCommands([
      `set -lex ${state.lexicon} -plies ${plies}`,
      `cgp ${cgp}`,
      'generate',
      `simulate -threads ${threads} -tlim ${tlim}`,
    ]);
    applyState(await bridge.getJson('getState'));
    applyMoves(await bridge.getJson('getMoves'));
  });
}

function endgame() {
  return withBusy('Solving endgame', async () => {
    await ensureLexicon(state.lexicon);
    const cgp = buildCgp();
    const eplies = parseInt($('opt-eplies').value, 10) || 5;
    clearResults();
    const res = await bridge.runCommands([
      `set -lex ${state.lexicon} -eplies ${eplies} -ttfraction 0.25`,
      `cgp ${cgp}`,
      'endgame',
      'shendgame',
    ]);
    applyState(await bridge.getJson('getState'));
    const eg = await bridge.getJson('getEndgame');
    if (eg && eg.valid) {
      if (eg.best) {
        board.setHighlight(eg.best.placed);
      }
      setStatus(`Endgame: ${eg.value >= 0 ? '+' : ''}${eg.value} (depth ${eg.depth}, ${eg.seconds.toFixed(1)}s)`, 'success');
    } else {
      setStatus('Endgame produced no result', '');
    }
    (res.outputs || []).forEach((o) => logLine(o));
  });
}

function infer() {
  return withBusy('Inferring', async () => {
    await ensureLexicon(state.lexicon);
    const cgp = buildCgp();
    clearResults();
    const res = await bridge.runCommands([`set -lex ${state.lexicon}`, `cgp ${cgp}`, 'infer', 'shinference']);
    applyState(await bridge.getJson('getState'));
    (res.outputs || []).forEach((o) => logLine(o));
    setStatus('Inference complete', 'success');
  });
}

// Resets to an empty board with empty racks. Used for both startup and the
// "New game" button. (The `newgame` command needs a GCG filename when the
// fgrequired option is on, so an empty CGP is the reliable reset for analysis.)
async function loadEmptyPosition() {
  await ensureLexicon(state.lexicon);
  board.clearBoard();
  await bridge.runCommands([
    `set -lex ${state.lexicon}`,
    `cgp ${board.cgpBoard()} / 0/0 0`,
  ]);
  applyState(await bridge.getJson('getState'));
  clearPlays();
}

function newGame() {
  return withBusy('New game', loadEmptyPosition);
}

function playMove(move) {
  // Advance the position client-side: place the move's tiles, set the mover's
  // rack to its leave, add the score, and flip the turn — then re-sync the
  // engine via CGP. (We deliberately do not auto-draw: in analysis you control
  // the next rack. The engine's commit command also mis-validates plays that
  // run through existing tiles, so applying locally is both correct and apt.)
  const mover = state.onTurn;
  if (move.type === 'play') {
    board.applyPlaced(move.placed);
  }
  if (move.type !== 'pass' && typeof move.score === 'number') {
    state.players[mover].score = (state.players[mover].score | 0) + move.score;
  }
  state.players[mover].rack = (move.leave || '').toUpperCase();
  state.onTurn = 1 - mover;
  clearPlays();
  renderPlayers();
  return withBusy(`Played ${move.desc}`, async () => {
    await ensureLexicon(state.lexicon);
    const cgp = buildCgp();
    await bridge.runCommands([`set -lex ${state.lexicon}`, `cgp ${cgp}`]);
    applyState(await bridge.getJson('getState'));
  });
}

function randomRack() {
  return withBusy('Random rack', async () => {
    await ensureLexicon(state.lexicon);
    const cgp = buildCgp();
    await bridge.runCommands([`set -lex ${state.lexicon}`, `cgp ${cgp}`, 'rrack']);
    applyState(await bridge.getJson('getState'));
    clearPlays();
  });
}

async function loadCgpString(cgpString) {
  const trimmed = cgpString.trim();
  if (!trimmed) {
    return;
  }
  const lexMatch = trimmed.match(/-lex\s+(\S+)/);
  if (lexMatch && LEXICA.includes(lexMatch[1])) {
    state.lexicon = lexMatch[1];
    $('lexicon').value = state.lexicon;
  }
  await withBusy('Loading CGP', async () => {
    await ensureLexicon(state.lexicon);
    const cmd = /-lex\s+\S+/.test(trimmed) ? trimmed : `${trimmed} -lex ${state.lexicon}`;
    await bridge.runCommands([`set -lex ${state.lexicon}`, `cgp ${cmd}`]);
    applyState(await bridge.getJson('getState'));
    clearPlays();
  });
}

// --- Wiring ----------------------------------------------------------------
function wireUi() {
  const lexSel = $('lexicon');
  LEXICA.forEach((lex) => {
    const opt = document.createElement('option');
    opt.value = lex;
    opt.textContent = lex;
    lexSel.appendChild(opt);
  });
  lexSel.value = state.lexicon;
  lexSel.addEventListener('change', () => {
    state.lexicon = lexSel.value;
    renderMeta();
  });

  $('btn-generate').addEventListener('click', generate);
  $('btn-simulate').addEventListener('click', simulate);
  $('btn-endgame').addEventListener('click', endgame);
  $('btn-infer').addEventListener('click', infer);
  $('btn-new').addEventListener('click', newGame);
  $('stop').addEventListener('click', () => bridge.stop());

  $('btn-copy-cgp').addEventListener('click', async () => {
    const cgp = state.cgp || `${buildCgp()} -lex ${state.lexicon}`;
    try {
      await navigator.clipboard.writeText(cgp);
      setStatus('CGP copied to clipboard', 'success');
    } catch (err) {
      window.prompt('Copy this CGP:', cgp);
    }
  });
  // Inline Load-CGP panel (prompt() is blocked in cross-origin iframes and is
  // hard to test, so use an in-page field).
  const loadPanel = $('loadcgp-panel');
  const cgpInput = $('cgp-input');
  const openLoad = () => {
    loadPanel.hidden = false;
    cgpInput.value = state.cgp || '';
    cgpInput.focus();
    cgpInput.select();
    board.fit();
  };
  const closeLoad = () => {
    loadPanel.hidden = true;
    board.fit();
  };
  const submitLoad = () => {
    const cgp = cgpInput.value.trim();
    closeLoad();
    if (cgp) {
      loadCgpString(cgp);
    }
  };
  $('btn-load-cgp').addEventListener('click', () => {
    if (loadPanel.hidden) {
      openLoad();
    } else {
      closeLoad();
    }
  });
  $('cgp-load-btn').addEventListener('click', submitLoad);
  $('cgp-cancel-btn').addEventListener('click', closeLoad);
  cgpInput.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') {
      submitLoad();
    } else if (e.key === 'Escape') {
      closeLoad();
    }
  });

  const sampleSel = $('sample');
  Object.keys(SAMPLES).forEach((name) => {
    const opt = document.createElement('option');
    opt.value = name;
    opt.textContent = name;
    sampleSel.appendChild(opt);
  });
  $('btn-sample').addEventListener('click', () => loadCgpString(SAMPLES[sampleSel.value]));

  // Sortable headers.
  $('moves-head').querySelectorAll('[data-sort]').forEach((th) => {
    th.addEventListener('click', () => {
      const key = th.dataset.sort;
      if (state.sortKey === key) {
        state.sortDir = -state.sortDir;
      } else {
        state.sortKey = key;
        state.sortDir = -1;
      }
      renderMoves();
    });
  });

  document.addEventListener('keydown', onKeydown);
  window.addEventListener('resize', () => board.fit());
}

async function boot() {
  board = new Board($('board'), {
    onCursorChange: (cur) => {
      if (cur) {
        state.focus = { kind: 'board' };
        renderPlayers();
      }
      updatePaletteHint();
    },
    onEdit: () => clearPlays(),
  });
  buildPalette();
  wireUi();
  renderPlayers();
  renderMoves();
  renderMeta();
  board.fit();
  requestAnimationFrame(() => board.fit());

  bridge = new Bridge(WORKER_URL, {
    onLog: (t) => console.debug('[worker]', t),
    onStatus: (t) => console.debug('[status]', t),
    onOutput: (t) => console.debug('[output]', t),
    onError: (t) => console.error('[worker error]', t),
  });

  setStatus('Loading WASM…');
  try {
    await bridge.ready;
    setStatus('Loading lexicon data…');
    await ensureLexicon(state.lexicon);
    await bridge.init('data');
    // Establish a known empty position so the board renders premium squares.
    await loadEmptyPosition();
    setStatus('Ready — click a square to place tiles, set a rack, then Generate.', 'success');
  } catch (err) {
    setStatus('Failed to initialize: ' + err.message, 'error');
    logLine('❌ ' + err.message);
  }
}

function formatInt(n) {
  return (n || 0).toLocaleString();
}

boot();
