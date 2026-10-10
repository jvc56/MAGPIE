import { installTileDrag } from "./tile-drag.mjs";
import { deviceBudget } from "./device-budget.mjs";
import { listWMPs } from "./wmp-cache.mjs";
import { parsePegProgress, mergePegRows } from "./peg-review.mjs";
import { ensureIsolation } from "./isolation.mjs";
import { EngineClient } from "./engine-client.mjs";
import { readGCG } from "./gcg-loader.mjs?v=2";
import { logGCG, gcgLogText } from "./gcg-diagnostics.mjs";
import {
  SAMPLES,
  VALUES,
  parseCGP,
  toCGP,
  snapshotPosition,
  restorePosition,
  inventory,
  parseResults,
  formatHistoryPlay,
  previewMove,
  analysisCommands,
  combineSim,
  analysisPositionKey,
} from "./analysis-model.mjs";
import {
  remainingRack,
  rackSlots,
  reorderRack,
  placeTile,
  moveNotation,
  normalizeMove,
} from "./move-entry.mjs";
let cachedWMPFiles = [];
let positionGeneration = 0, analysisGeneration = -1, historyClickTimer, pendingHistoryIndex = null;
const $ = (id) => document.getElementById(id);
let position = parseCGP(SAMPLES.opening);
let mode = "kibitz",
  results = [],
  selectedMove = "",
  busy = false,
  canceled = false;
let cursor = null,
  vertical = false,
  undo = [],
  engine,
  timer;
let tileDrag = null;
let draftTiles = [],
  rackChoice = null,
  gameUndo = [];
let previewTimer,
  previewGeneration = 0,
  previewPromise = Promise.resolve();
let recordedGame = null;
let historyIndex = 0;
let recent = [];
let hintTarget = null;
let continuationPosition = null;
let session = null,
  botRunning = false,
  botTimer,
  reviewRunning = false;
let analysisCache = new Map(),
  analysisBase = null,
  analysisBaseIterations = 0,
  analysisIterations = 0,
  activeAnalysisKey = "",
  analysisMeta = "";
let pegProgress = null;
const randomSeed = () => crypto.getRandomValues(new Uint32Array(1))[0];
const searchMode = () => {
  const { bagCount } = inventory(position);
  return bagCount === 0 ? "endgame" : bagCount <= 4 ? "peg" : "sim";
};
const cacheKey = (kind = mode) =>
  `${analysisPositionKey(position)}|${kind}|${$("plies").value}|${$("candidates").value}|${$("hide-spoilers").checked ? "hidden" : recordedGame && !isLive() ? playedMove() : ""}`;
function physicalCGP() {
  const physical = structuredClone(position);
  if (position.onTurn === 1) {
    physical.racks.reverse();
    physical.scores.reverse();
  }
  return toCGP(physical);
}
function liveAction(action, move = "") {
  return {
    text: recordedGame.gcg,
    lexicon: position.lexicon,
    index: historyIndex,
    rack: position.racks[0],
    move,
    action,
    challenge: session?.challenge || "void",
    cgp: physicalCGP(),
    seed: randomSeed(),
    onTurn: position.onTurn,
  };
}
function isLive() {
  return session && !session.finished;
}
function hiddenLiveGame() {
  return !!(isLive() && session.kind === "play");
}
function humanTurn() {
  return (
    !isLive() ||
    (session.kind === "play" &&
      position.onTurn === session.human &&
      historyIndex === recordedGame.positions.length - 1)
  );
}

try {
  recent = JSON.parse(localStorage.getItem("magpie-preview-recent") || "[]")
    .filter((x) => {
      try {
        restorePosition(x);
        return true;
      } catch {
        return false;
      }
    })
    .slice(0, 5);
} catch {
  /* Storage is optional. */
}
const premiums = [
  "=  '   =   '  =",
  ' -   "   "   - ',
  "  -   ' '   -  ",
  "'  -   '   -  '",
  "    -     -    ",
  ' "   "   "   " ',
  "  '   ' '   '  ",
  "=  '   -   '  =",
  "  '   ' '   '  ",
  ' "   "   "   " ',
  "    -     -    ",
  "'  -   '   -  '",
  "  -   ' '   -  ",
  ' -   "   "   - ',
  "=  '   =   '  =",
];
const premiumNames = {
  "=": ["tw", "3W"],
  "-": ["dw", "2W"],
  '"': ["tl", "3L"],
  "'": ["dl", "2L"],
  " ": ["normal", ""],
};
const note = {
  kibitz: "Static equity = score + leave value.",
  sim: "Estimated win % and equity from simulation. Early results may change.",
  peg: "Win % and expected spread, grouped by search depth. Shallower rows have received less analysis.",
  endgame:
    "Spread is the final margin from the on-turn player’s perspective. A time-limited line may still be approximate.",
};
function message(text) {
  $("status").textContent = text;
}
function error(text) {
  $("error").hidden = !text;
  $("error").textContent = text;
}
function readSettings() {
  const settings = {};
  for (const id of ["seconds", "candidates", "plies", "threads"]) {
    const field = $(id),
      n = Number(field.value);
    if (
      !Number.isInteger(n) ||
      n < Number(field.min) ||
      n > Number(field.max)
    )
      throw new Error(
        `${field.labels[0].querySelector(".setting-name")?.textContent || field.labels[0].textContent.trim()} must be ${field.min}–${field.max}.`,
      );
    settings[id] = n;
  }
  settings.ttMiB = Number($("tt-memory").value);
  settings.wmpSource = $("wmp-source").value;
  settings.wmp = settings.wmpSource !== "off";
  return settings;
}
const budget = deviceBudget();
$("threads").max = budget.maxThreads;
$("threads").value = budget.threads;
$("tt-memory").value = budget.ttMiB;
function settingsSummary() {
  $("settings-summary").textContent =
    `${$("seconds").value}s · ${$("plies").value} plies · ${$("threads").value} threads`;
}
for (const id of ["seconds", "candidates", "plies", "threads"])
  $(id).addEventListener("input", settingsSummary);
settingsSummary();
let settingsOpen = false;
let settingsReturnFocus = null;
let gameScrollY = 0;
function selectSettingsSection(section) {
  for (const button of document.querySelectorAll("[data-settings-section]")) {
    const selected = button.dataset.settingsSection === section;
    button.toggleAttribute("aria-current", selected);
    if (selected) button.setAttribute("aria-current", "page");
    $(`${button.dataset.settingsSection}-settings`).hidden = !selected;
  }
}
function openSettings(section = "appearance") {
  if (!$("settings-view")) return;
  settingsReturnFocus = document.activeElement;
  gameScrollY = window.scrollY;
  settingsOpen = true;
  $("game-view").hidden = true;
  $("game-header-actions").hidden = true;
  $("unseen-pool").hidden = true;
  $("settings-view").hidden = false;
  selectSettingsSection(section);
  window.scrollTo(0, 0);
  $("settings-heading").focus({ preventScroll: true });
}
function closeSettings() {
  settingsOpen = false;
  $("settings-view").hidden = true;
  $("game-view").hidden = false;
  $("game-header-actions").hidden = false;
  $("unseen-pool").hidden = false;
  settingsReturnFocus?.focus({ preventScroll: true });
  window.scrollTo(0, gameScrollY);
}
$("open-settings")?.addEventListener("click", () => openSettings());
$("open-lexicon-settings")?.addEventListener("click", () =>
  openSettings("lexicon"),
);
$("open-analysis-settings")?.addEventListener("click", () =>
  openSettings("analysis"),
);
$("back-to-game")?.addEventListener("click", closeSettings);
for (const button of document.querySelectorAll("[data-settings-section]")) {
  button.addEventListener("click", () =>
    selectSettingsSection(button.dataset.settingsSection),
  );
}
try {
  if ($("premium-labels"))
    $("premium-labels").checked =
      localStorage.getItem("magpie-preview-premium-labels") === "true";
} catch {
  /* Storage is optional. */
}
function setBusy(value) {
  value = value || Boolean(engine?.fatalError);
  if (value) tileDrag?.cancel();
  busy = value;
  if (!value) $("stop-wmp").hidden = true;
  document.body.classList.toggle("busy", value);
  document
    .querySelectorAll(
      "#game-view input, #game-view select, #game-view button, #analysis-settings input, #analysis-settings select, #wmp-files button, #lexicon, #choose-gcg, #submit-gcg, #gcg-text",
    )
    .forEach((element) => {
      element.disabled = value;
    });
  if ($("open-analysis-settings"))
    $("open-analysis-settings").disabled = false;
  if ($("settings-busy")) $("settings-busy").hidden = !value;
  if ($("lexicon-busy")) $("lexicon-busy").hidden = !value;
  $("stop").disabled = !value || Boolean(engine?.fatalError);
  if (reviewRunning) $("stop-review").disabled = false;
  renderContextHint();
  if (!value) {
    $("undo").disabled = !undo.length;
    updateActionAvailability();
    renderHistory();
  }
  renderSession();
  renderHistory();
  renderWMPCache();
  if (!value) scheduleComputer();
}
function updateActionAvailability() {
  const visibleMode = searchMode();
  document.querySelectorAll("[data-mode]").forEach((button) => {
    button.hidden =
      button.dataset.mode !== "kibitz" && button.dataset.mode !== visibleMode;
  });
  if (busy) return;
  const { bagCount } = inventory(position);
  document.querySelector('[data-mode="peg"]').disabled =
    bagCount < 1 || bagCount > 4;
  document.querySelector('[data-mode="endgame"]').disabled = bagCount !== 0;
}
function clearResults() {
  pegProgress = null;
  continuationPosition = null;
  analysisMeta = "";
  analysisIterations = 0;
  results = [];
  selectedMove = "";
  $("raw").textContent = "";
  $("elapsed").textContent = "";
  renderResults();
}
function remember() {
  const cgp = toCGP(position);
  recent = [
    snapshotPosition(position),
    ...recent.filter((saved) => toCGP(restorePosition(saved)) !== cgp),
  ].slice(0, 5);
  try {
    localStorage.setItem("magpie-preview-recent", JSON.stringify(recent));
  } catch {
    /* Optional. */
  }
  renderRecent();
}
function renderRecent() {
  $("recent").replaceChildren(new Option("Select position…", ""));
  recent.forEach((cgp, index) => {
    try {
      const p = restorePosition(cgp);
      $("recent").add(
        new Option(
          `${p.lexicon} · ${p.racks[0] || "empty"} · ${p.scores.join("–")}`,
          String(index),
        ),
      );
    } catch {
      /* Ignore an outdated or invalid saved position. */
    }
  });
}
function setPosition(next, rememberPosition = false, fromHistory = false) {
  positionGeneration++;
  clearTimeout(historyClickTimer);
  pendingHistoryIndex = null;
  if (!fromHistory) {
    recordedGame = null;
    session = null;
    clearTimeout(botTimer);
    analysisCache.clear();
  }
  cancelDraftCheck();
  draftTiles = [];
  rackChoice = null;
  $("move-feedback").textContent = "";
  position = next;
  // A just-finished live game retains both dealt racks; normal review must
  // use the information available to the mover, like a reloaded GCG does.
  if (fromHistory && !isLive() && inventory(position).bagCount > 0)
    position.racks[1] = "";
  undo = [];
  cursor = null;
  vertical = false;
  $("edit").checked = false;
  $("event-note").value = recordedGame?.positions[historyIndex]?.note || "";
  error("");
  clearResults();
  renderPosition();
  if (rememberPosition) remember();
  message("Position ready");
  restoreAnalysis();
  renderSession();
}
function renderBoard() {
  tileDrag?.cancel();
  const drawPosition = continuationPosition || position;
  const board = $("board");
  board.classList.toggle("position-editing", $("edit").checked);
  const preview = new Map(
    previewMove(drawPosition, continuationPosition ? "" : selectedMove).map(
      (t) => [`${t.row},${t.col}`, t.letter],
    ),
  );
  for (const tile of draftTiles)
    preview.set(`${tile.row},${tile.col}`, tile.letter);
  board.classList.toggle("editing", $("edit").checked || !!recordedGame);
  board.classList.toggle(
    "show-premium-labels",
    $("premium-labels")?.checked ?? false,
  );
  const fragment = document.createDocumentFragment();
  for (let row = -1; row < 15; row++) {
    const rowElement = document.createElement("div");
    rowElement.setAttribute("role", "row");
    rowElement.style.display = "contents";
    for (let col = -1; col < 15; col++) {
      if (row === -1 || col === -1) {
        const label = document.createElement("span");
        label.className = "coordinate";
        label.textContent =
          row === -1
            ? col === -1
              ? ""
              : String.fromCharCode(65 + col)
            : String(row + 1);
        label.setAttribute("role", row === -1 ? "columnheader" : "rowheader");
        rowElement.append(label);
        continue;
      }
      const cell = document.createElement("button");
      cell.type = "button";
      cell.setAttribute("role", "gridcell");
      const [kind, name] = premiumNames[premiums[row][col]];
      const ghost = preview.get(`${row},${col}`);
      const letter = drawPosition.board[row][col] || ghost;
      const owner = ghost
        ? drawPosition.onTurn
        : drawPosition.owners[row][col];
      cell.className = `square ${kind}${letter ? " played" : ""}${ghost ? " preview" : ""}${letter && letter === letter.toLowerCase() ? " blank" : ""}`;
      if (draftTiles.some((tile) => tile.row === row && tile.col === col))
        cell.classList.add("pending");
      if (letter)
        cell.classList.add(owner === -1 ? "owner-unknown" : `owner-${owner}`);
      const coord = `${String.fromCharCode(65 + col)}${row + 1}`;
      cell.dataset.row = row;
      cell.dataset.col = col;
      const glyph = document.createElement("span");
      glyph.className =
        letter || (row === 7 && col === 7) ? "tile-letter" : "premium-label";
      glyph.textContent = letter
        ? letter.toUpperCase()
        : row === 7 && col === 7
          ? "★"
          : name;
      cell.append(glyph);
      cell.setAttribute(
        "aria-label",
        `${coord} ${letter ? `${letter === letter.toLowerCase() ? "blank " : ""}${letter.toUpperCase()}` : name || "empty"}${ghost ? " preview" : ""}${letter && owner !== -1 ? ` · Player ${owner + 1}` : ""}`,
      );
      cell.tabIndex =
        ($("edit").checked || recordedGame) &&
        (!cursor
          ? row === 7 && col === 7
          : cursor.row === row && cursor.col === col)
          ? 0
          : -1;
      if (letter && letter !== letter.toLowerCase()) {
        const points = document.createElement("sup");
        points.textContent = VALUES[letter];
        points.classList.toggle("two-digit", VALUES[letter] >= 10);
        cell.append(points);
      }
      if (
        cursor?.row === row &&
        cursor?.col === col &&
        ($("edit").checked || recordedGame)
      ) {
        cell.classList.add("cursor");
        cell.dataset.arrow = vertical ? "↓" : "→";
      }
      cell.disabled = busy;
      rowElement.append(cell);
    }
    fragment.append(rowElement);
  }
  board.replaceChildren(fragment);
  renderContextHint();
}
function terminalPosition() {
  return (
    recordedGame?.ended && historyIndex === recordedGame.positions.length - 1
  );
}
const wideLayout = matchMedia("(min-width: 1500px)");
function placeUnseenPool() {
  $(wideLayout.matches ? "brand-rail" : "unseen-home").append(
    $("unseen-pool"),
  );
}
wideLayout.addEventListener("change", placeUnseenPool);
placeUnseenPool();
const compactPool = matchMedia("(max-width: 850px)");
function setPoolExpansion() {
  $("unseen-pool").open = !compactPool.matches;
}
compactPool.addEventListener("change", setPoolExpansion);
setPoolExpansion();

function renderPosition() {
  $("rack").value = position.racks[0];
  $("opponent-rack").value = position.racks[1];
  $("score").value = position.scores[0];
  $("opponent-score").value = position.scores[1];
  $("lexicon").value = position.lexicon;
  if ($("current-lexicon"))
    $("current-lexicon").textContent = position.lexicon;
  $("on-turn-player").value = position.onTurn;
  $("ownership-tools").hidden = !$("edit").checked;
  $("rack-tiles").dataset.owner = position.onTurn;
  const rackIndex =
    isLive() && session.kind === "play" && session.human !== position.onTurn
      ? 1
      : 0;
  const shownRack = position.racks[rackIndex];
  if (isLive() && session.kind === "play")
    $("rack-tiles").dataset.owner = session.human;
  const visiblePosition = structuredClone(position);
  if (rackIndex) visiblePosition.racks.reverse();
  const { unseen, unseenCount } = inventory(visiblePosition);
  const { bagCount } = inventory(position);
  $("bag-count").textContent = `${bagCount} in bag`;
  $("unseen-label").textContent = `Unseen tiles · ${unseenCount}`;
  const vowels = [..."AEIOU"].reduce(
    (total, letter) => total + unseen[letter], 0,
  );
  const blanks = unseen["?"];
  const consonants = unseenCount - vowels - blanks;
  $("unseen-counts").replaceChildren();
  for (const [kind, count] of [
    ["vowels", vowels], ["consonants", consonants], ["blanks", blanks],
  ]) {
    const tally = document.createElement("span");
    tally.dataset.kind = kind;
    tally.textContent = `${count} ${kind}`;
    $("unseen-counts").append(tally, " ");
  }
  $("unseen-tiles").replaceChildren();
  for (const letter of "AEIOUBCDFGHJKLMNPQRSTVWXYZ?") {
    const count = unseen[letter];
    if (count) {
      const tile = document.createElement("span");
      tile.textContent = letter.repeat(count);
      tile.dataset.letter = letter;
      tile.dataset.count = count;
      tile.setAttribute(
        "aria-label",
        `${letter === "?" ? "Blank" : letter}: ${count}`,
      );
      if ($("unseen-tiles").childNodes.length)
        $("unseen-tiles").append(" ");
      $("unseen-tiles").append(tile);
    }
  }
  $("rack-tiles").replaceChildren();
  for (const {letter, index} of rackSlots(shownRack, rackIndex ? [] : draftTiles).remaining) {
    const tile = document.createElement("button");
    tile.type = "button";
    tile.dataset.letter = letter;
    tile.dataset.rackIndex = index;
    tile.setAttribute(
      "aria-label",
      letter === "?" ? "Blank tile" : `Tile ${letter}`,
    );
    tile.disabled =
      busy ||
      terminalPosition() ||
      (isLive() && (!humanTurn() || session.paused));
    tile.classList.toggle("chosen", rackChoice?.index === index);
    tile.classList.add("rack-tile");
    const glyph = document.createElement("span");
    glyph.className = "tile-letter";
    glyph.textContent = letter;
    const score = document.createElement("sup");
    score.textContent = VALUES[letter];
    score.classList.toggle("two-digit", VALUES[letter] >= 10);
    tile.append(glyph, score);
    $("rack-tiles").append(tile);
  }
  $("move-entry").hidden = !recordedGame || terminalPosition();
  $("undo").disabled = busy || !undo.length;
  renderBoard();
  updateActionAvailability();
  renderHistory();
  renderSession();
}
const format = (n, decimals = 2) =>
  Number.isFinite(n) ? n.toFixed(decimals) : "—";
function renderResults() {
  document
    .querySelectorAll("[data-mode]")
    .forEach((button) =>
      button.classList.toggle(
        "active",
        (busy || results.length > 0) && button.dataset.mode === mode,
      ),
    );
  renderPegProgress();
  const columns = [
    ["rank", "#"],
    ["move", "Play"],
  ];
  if (mode === "kibitz")
    columns.push(
      ["leave", "Leave"],
      ["score", "Score"],
      ["equity", "Equity"],
    );
  if (mode === "sim")
    columns.push(
      ["leave", "Leave"],
      ["score", "Score"],
      ["win", "Win %"],
      ["equity", "Equity"],
      ["iterations", "Iters"],
    );
  if (mode === "peg")
    columns.push(
      ["state", "Status"],
      ["win", "Win %"],
      ["spread", "Spread"],
      ["outcomes", "W / T / L"],
      ["depth", "Depth"],
    );
  if (mode === "endgame")
    columns.push(
      ["score", "Score"],
      ["outcome", "Result"],
      ["spread", "Spread"],
      ["value", "Gain"],
    );
  if (mode === "sim") {
    const plies = Math.max(
      0,
      ...results.map((row) => row.plyAverages?.length || 0),
    );
    for (let index = 0; index < plies; index++)
      columns.push([`ply${index}`, `Ply ${index + 1}`]);
  }
  $("analysis-meta").textContent = results.length
    ? [analysisMeta, playedAssessment()].filter(Boolean).join(" · ")
    : "";
  const simButton = document.querySelector('[data-mode="sim"]');
  const canContinueSim = (mode === "sim" && results.length > 0) || analysisCache.has(cacheKey("sim"));
  simButton.textContent = canContinueSim ? "Continue sim" : "Run sim";
  simButton.dataset.contextHint = canContinueSim
    ? "Add iterations to this position’s simulation."
    : simButton.title;
  $("fresh-analysis").hidden = !results.length || mode === "kibitz";
  $("fresh-analysis").disabled = busy;
  const header = document.createElement("tr");
  for (const [, label] of columns) {
    const th = document.createElement("th");
    th.scope = "col";
    th.textContent = label;
    header.append(th);
  }
  $("results").tHead.replaceChildren(header);
  const body = document.createDocumentFragment();
  results.forEach((row) => {
    const tr = document.createElement("tr");
    tr.classList.toggle("selected", row.move === selectedMove);
    tr.classList.toggle(
      "peg-pruned",
      mode === "peg" && row.state === "Pruned",
    );
    tr.classList.toggle(
      "peg-evaluating",
      mode === "peg" && row.state === "Evaluating",
    );
    const played = !$("hide-spoilers").checked && playedMove();
    tr.classList.toggle(
      "played",
      !!played && moveIdentity(row.move) === moveIdentity(played),
    );
    for (const [key] of columns) {
      const td = document.createElement("td");
      if (key === "move") {
        const button = document.createElement("button");
        button.textContent = row.move;
        button.type = "button";
        button.setAttribute(
          "aria-pressed",
          String(row.move === selectedMove),
        );
        button.addEventListener("click", () => selectMove(row.move));
        td.append(button);
      } else if (key === "outcomes")
        td.textContent = Number.isFinite(row.wins)
          ? `${row.wins} / ${row.ties} / ${row.losses}`
          : "—";
      else if (key === "outcome")
        td.textContent =
          row.spread > 0 ? "Win" : row.spread < 0 ? "Loss" : "Tie";
      else if (key.startsWith("ply")) {
        const index = +key.slice(3);
        td.textContent = format(row.plyAverages?.[index], 1);
        td.classList.add(
          (index + position.onTurn) % 2 ? "ply-odd" : "ply-even",
        );
      } else if (mode === "sim" && (key === "win" || key === "equity")) {
        td.textContent = format(row[key], 1);
        td.title = `Standard error ±${format(key === "win" ? row.winSE : row.equitySE)}`;
        td.dataset.contextHint = `${key === "win" ? "Win probability" : "Mean equity"} · standard error ±${format(key === "win" ? row.winSE : row.equitySE)}`;
      } else
        td.textContent = ["equity", "win", "spread"].includes(key)
          ? format(row[key], mode === "endgame" ? 0 : 2)
          : (row[key] ?? "—");
      if (
        ["score", "win", "equity", "spread"].includes(key) &&
        Number.isFinite(row[key])
      )
        td.classList.toggle(
          "best",
          row[key] ===
            Math.max(...results.map((value) => value[key] ?? -Infinity)),
        );
      if (["spread", "outcome"].includes(key))
        td.classList.add(
          row.spread > 0 ? "winning" : row.spread < 0 ? "losing" : "tie",
        );
      tr.append(td);
    }
    body.append(tr);
  });
  $("results").tBodies[0].replaceChildren(body);
  $("empty").hidden = results.length > 0;
  $("result-note").textContent = note[mode];
  const row = results.find((r) => r.move === selectedMove);
  $("selection").hidden = !row;
  $("selection").replaceChildren();
  if (row) {
    const title = document.createElement("strong");
    title.textContent = row.move;
    $("selection").append(
      title,
      ` · ${row.leave ? `Leave ${row.leave}` : "Board preview"}`,
    );
    if (recordedGame && !terminalPosition()) {
      const play = document.createElement("button");
      play.textContent = "Play selected move";
      play.disabled = busy || (isLive() && (!humanTurn() || session.paused));
      play.addEventListener("click", () => submitMove(row.move));
      $("selection").append(document.createTextNode(" "), play);
    }
    if (mode === "sim")
      $("selection").append(
        document.createElement("br"),
        `Win ${format(row.win)}% ±${format(row.winSE)} · Equity ${format(row.equity)} ±${format(row.equitySE)} · ${row.iterations.toLocaleString()} iterations`,
      );
    if (row.continuation) {
      const line = document.createElement("div");
      line.className = "pv-line";
      const steps = [
        row.move,
        ...(row.continuation.match(
          /(?:[A-O]\d+|\d+[A-O]) [A-Za-z().?]+|pass/g,
        ) || []),
      ];
      steps.forEach((move, index) => {
        const button = document.createElement("button");
        button.textContent = `${index + 1}. ${move}`;
        button.dataset.contextHint =
          "Preview the continuation through this move.";
        button.addEventListener("click", () =>
          previewContinuation(steps.slice(0, index + 1)),
        );
        line.append(button);
      });
      $("selection").append(line);
    }
  }
}
function renderPegProgress() {
  const panel = $("peg-progress");
  panel.hidden = mode !== "peg" || !pegProgress?.stages.length;
  panel.replaceChildren();
  if (panel.hidden) return;
  pegProgress.stages.forEach((stage, index) => {
    const card = document.createElement("div");
    card.className = "peg-stage";
    card.classList.toggle("current", index === pegProgress.stages.length - 1);
    const title = document.createElement("strong");
    title.textContent =
      index === 0 ? "Greedy" : `Stage ${index} · ${stage.depth} ply`;
    const count = document.createElement("span");
    count.textContent = `${stage.completed} / ${stage.total}`;
    const bar = document.createElement("progress");
    bar.max = Math.max(1, stage.total);
    bar.value = stage.completed;
    bar.setAttribute(
      "aria-label",
      `${title.textContent} candidates evaluated`,
    );
    const detail = document.createElement("small");
    const next = pegProgress.stages[index + 1];
    detail.textContent = next
      ? `${next.total} retained · ${Math.max(0, stage.completed - next.total)} pruned`
      : `${stage.seconds.toFixed(1)}s${pegProgress.done && stage.completed < stage.total ? " · partial" : ""}`;
    card.append(title, count, bar, detail);
    panel.append(card);
  });
}
function selectMove(move) {
  continuationPosition = null;
  recallTiles(false);
  selectedMove = selectedMove === move ? "" : move;
  renderResults();
  renderBoard();
}
function acceptOutput(text, command) {
  if (analysisGeneration !== positionGeneration) return;
  // Preparatory set/CGP/generate output must not masquerade as sim results.
  const relevant =
    mode === "kibitz"
      ? /^generate/.test(command)
      : mode === "sim"
        ? /^(sim|snoprune)(?: |$)/.test(command)
        : command.startsWith(mode);
  if (!relevant) return;
  $("raw").textContent = text;
  let next = parseResults(text, mode);
  if (mode === "peg") {
    pegProgress = parsePegProgress(text);
    next = mergePegRows(results, next, pegProgress);
    renderPegProgress();
  }
  if (next.length) {
    results =
      analysisBase && mode === "sim" ? combineSim(analysisBase, next) : next;
    if (mode === "sim") {
      const batchIterations = [...text.matchAll(/Iters:\s+(\d+)/g)].at(-1)?.[1];
      analysisIterations = analysisBaseIterations + (batchIterations !== undefined
        ? Number(batchIterations)
        : next.reduce((total, row) => total + (row.iterations || 0), 0));
    }
    analysisMeta = analysisSummary(text, mode);
    renderResults();
    renderBoard();
  }
}
async function analyze(nextMode, options = {}) {
  if (busy) return;
  if (pendingHistoryIndex !== null) {
    showHistory(pendingHistoryIndex, {auto:false});
  }
  let succeeded = false;
  const generation = positionGeneration;
  analysisGeneration = generation;
  try {
    const settings = readSettings();
    if (options.seconds) settings.seconds = options.seconds;
    const analyzed = structuredClone(position);
    if (isLive() && inventory(position).bagCount > 0) analyzed.racks[1] = "";
    if (
      recordedGame &&
      !isLive() &&
      !$("hide-spoilers").checked &&
      recordedBoardMatches() &&
      [...position.racks[0]].sort().join("") ===
        [...restorePosition(recordedGame.positions[historyIndex]).racks[0]]
          .sort()
          .join("")
    )
      settings.playedMove = playedMove();
    const commands = analysisCommands(analyzed, nextMode, settings);
    commands[0] += ` -seed ${randomSeed()}`;
    activeAnalysisKey = cacheKey(nextMode);
    const resuming = options.resume ?? nextMode === "sim";
    analysisBase = resuming
      ? analysisCache.get(activeAnalysisKey)?.results
      : null;
    analysisBaseIterations = resuming
      ? analysisCache.get(activeAnalysisKey)?.iterations || 0
      : 0;
    const keepSelection = mode === nextMode && analysisBase?.some(row => row.move === selectedMove);
    mode = nextMode;
    error("");
    if (!keepSelection) selectedMove = "";
    continuationPosition = null;
    pegProgress = null;
    if (analysisBase?.length) {
      const saved = analysisCache.get(activeAnalysisKey);
      results = structuredClone(analysisBase);
      analysisIterations = analysisBaseIterations;
      analysisMeta = saved.meta;
      $("raw").textContent = saved.raw;
      renderResults();
      $("elapsed").textContent = "";
    } else {
      clearResults();
    }
    canceled = false;
    document
      .querySelectorAll("[data-mode]")
      .forEach((button) =>
        button.classList.toggle("active", button.dataset.mode === mode),
      );
    setBusy(true);
    remember();
    message(`Loading ${position.lexicon}…`);
    await previewPromise;
    await engine.prepare(position.lexicon);
    if (!canceled) await engine.prepareWMP(position.lexicon, settings.wmpSource, settings.threads, $("wmp-cache-enabled").checked);
    if (generation !== positionGeneration) return false;
    if (canceled) {
      message("Stopped");
      return;
    }
    message(
      `${{ kibitz: "Generating moves", sim: "Simulating", peg: "Solving pre-endgame", endgame: "Solving endgame" }[mode]}…`,
    );
    const started = performance.now();
    timer = setInterval(() => {
      $("elapsed").textContent =
        `${((performance.now() - started) / 1000).toFixed(1)}s`;
    }, 100);
    const response = await engine.run(commands);
    if (generation !== positionGeneration) return false;
    message(response.stopped ? "Stopped · partial results" : "Complete");
    succeeded = !response.stopped && results.length > 0;
    if (results.length)
      analysisCache.set(activeAnalysisKey, {
        mode,
        results: structuredClone(results),
        raw: $("raw").textContent,
        meta: analysisMeta,
        iterations: analysisIterations,
        pegProgress: structuredClone(pegProgress),
      });
  } catch (e) {
    if (generation !== positionGeneration) return false;
    error(e.message);
    message("Analysis could not run");
  } finally {
    clearInterval(timer);
    setBusy(false);
    analysisBase = null;
    refreshWMPCache();
    renderResults();
    renderHistory();
  }
  return succeeded;
}
function stop() {
  if (!busy || !engine || $("stop").disabled) return;
  canceled = true;
  engine.stop();
  message("Stopping…");
  $("stop").disabled = true;
}
function commitEdit(next) {
  try {
    const validated = restorePosition(snapshotPosition(next));
    if (
      JSON.stringify(snapshotPosition(validated)) ===
      JSON.stringify(snapshotPosition(position))
    )
      return;
    undo.push(snapshotPosition(position));
    cancelDraftCheck();
    draftTiles = [];
    rackChoice = null;
    position = validated;
    error("");
    clearResults();
    renderPosition();
  } catch (e) {
    error(e.message);
    renderPosition();
  }
}
for (const [id, field, index] of [
  ["rack", "racks", 0],
  ["opponent-rack", "racks", 1],
  ["score", "scores", 0],
  ["opponent-score", "scores", 1],
]) {
  const updateField = () => {
    const next = structuredClone(position);
    next[field][index] =
      field === "scores"
        ? Number($(id).value)
        : $(id).value.trim().toUpperCase();
    commitEdit(next);
  };
  $(id).addEventListener("change", updateField);
  if (field === "racks") $(id).addEventListener("input", updateField);
}
$("on-turn-player").addEventListener("change", () => {
  const next = structuredClone(position);
  next.onTurn = Number($("on-turn-player").value);
  commitEdit(next);
});
$("assign-owner").addEventListener("click", () => {
  if (!cursor || !position.board[cursor.row][cursor.col]) {
    error("Select an occupied square first.");
    return;
  }
  const next = structuredClone(position);
  next.owners[cursor.row][cursor.col] = Number($("tile-owner").value);
  commitEdit(next);
});
$("lexicon").addEventListener("change", () => {
  const next = structuredClone(position);
  next.lexicon = $("lexicon").value;
  commitEdit(next);
});
$("sample").addEventListener("change", () =>
  setPosition(parseCGP(SAMPLES[$("sample").value]), true),
);
$("recent").addEventListener("change", () => {
  if ($("recent").value !== "")
    setPosition(restorePosition(recent[Number($("recent").value)]), true);
});
$("undo").addEventListener("click", () => {
  if (undo.length) {
    position = restorePosition(undo.pop());
    error("");
    clearResults();
    renderPosition();
  }
});
$("clear").addEventListener("click", () =>
  commitEdit(parseCGP(SAMPLES.opening.replace("CSW24", position.lexicon))),
);
$("edit").addEventListener("change", () => {
  recallTiles(false);
  selectedMove = "";
  renderResults();
  renderPosition();
});
$("premium-labels")?.addEventListener("change", () => {
  renderBoard();
  try {
    localStorage.setItem(
      "magpie-preview-premium-labels",
      String($("premium-labels").checked),
    );
  } catch {
    /* Storage is optional. */
  }
});
$("board").addEventListener("click", (event) => {
  if (busy) return;
  if (!$("edit").checked) {
    if (recordedGame) entryClick(event);
    return;
  }
  const cell = event.target.closest("[data-row]");
  if (!cell) return;
  const next = { row: +cell.dataset.row, col: +cell.dataset.col };
  if (cursor?.row === next.row && cursor?.col === next.col)
    vertical = !vertical;
  cursor = next;
  renderBoard();
  focusCursor();
});
function focusCursor() {
  if (!cursor) $("board").querySelector('[tabindex="0"]')?.focus();
  if (cursor)
    $("board")
      .querySelector(`[data-row="${cursor.row}"][data-col="${cursor.col}"]`)
      ?.focus();
}
$("board").addEventListener("keydown", (event) => {
  if (!$("edit").checked && recordedGame) {
    entryKey(event);
    return;
  }
  if (
    !$("edit").checked ||
    busy ||
    !cursor ||
    event.ctrlKey ||
    event.metaKey ||
    event.altKey
  )
    return;
  const arrows = {
    ArrowUp: [-1, 0],
    ArrowDown: [1, 0],
    ArrowLeft: [0, -1],
    ArrowRight: [0, 1],
  };
  if (arrows[event.key]) {
    event.preventDefault();
    cursor.row = Math.max(0, Math.min(14, cursor.row + arrows[event.key][0]));
    cursor.col = Math.max(0, Math.min(14, cursor.col + arrows[event.key][1]));
  } else if (event.key === " ") {
    event.preventDefault();
    vertical = !vertical;
  } else if (
    /^[a-zA-Z]$/.test(event.key) ||
    ["Backspace", "Delete"].includes(event.key)
  ) {
    event.preventDefault();
    const next = structuredClone(position);
    if (event.key === "Backspace" && !next.board[cursor.row][cursor.col]) {
      if (vertical) cursor.row = Math.max(0, cursor.row - 1);
      else cursor.col = Math.max(0, cursor.col - 1);
    }
    const letter = /^[a-zA-Z]$/.test(event.key)
      ? event.shiftKey
        ? event.key.toLowerCase()
        : event.key.toUpperCase()
      : "";
    next.board[cursor.row][cursor.col] = letter;
    next.owners[cursor.row][cursor.col] = letter
      ? Number($("tile-owner").value)
      : -1;
    const previous = toCGP(position);
    commitEdit(next);
    if (letter && previous !== toCGP(position)) {
      if (vertical) cursor.row = Math.min(14, cursor.row + 1);
      else cursor.col = Math.min(14, cursor.col + 1);
    }
  } else return;
  renderBoard();
  focusCursor();
});
$("import").addEventListener("click", () => {
  $("cgp").value = "";
  $("import-error").textContent = "";
  $("import-dialog").showModal();
  $("cgp").focus();
});
$("close-import").addEventListener("click", () => $("import-dialog").close());
$("import-form").addEventListener("submit", (event) => {
  event.preventDefault();
  try {
    setPosition(parseCGP($("cgp").value, position.lexicon), true);
    $("import-dialog").close();
  } catch (e) {
    $("import-error").textContent = e.message;
  }
});
$("copy").addEventListener("click", async () => {
  if (hiddenLiveGame()) return;
  try {
    await navigator.clipboard.writeText(toCGP(position));
    message("CGP copied");
  } catch {
    $("import-dialog").showModal();
    $("cgp").value = toCGP(position);
    $("cgp").select();
    $("import-error").textContent = "Copy the selected CGP.";
  }
});
function canExportGame() {
  return !!recordedGame && (engine?.fatalError || (!busy && !hiddenLiveGame()));
}
function renderHistory() {
  $("game-history").hidden = !recordedGame;
  $("copy").disabled = busy || hiddenLiveGame();
  $("save-gcg").disabled = !canExportGame();
  $("copy-gcg").disabled = !canExportGame();
  $("save-gcg").title = hiddenLiveGame() ? "Finish the game before exporting hidden racks" : recordedGame
    ? "Save the complete recorded game"
    : "Load a game record first";
  if (!recordedGame) return;
  $("history-previous").disabled = busy || historyIndex === 0;
  $("history-next").disabled =
    busy || historyIndex === recordedGame.positions.length - 1;
  $("history-position").value = String(historyIndex);
  $("history-note").textContent = recordedGame.positions[historyIndex].note;
  $("history-note").hidden = !$("history-note").textContent;
  $("history-edited").hidden = recordedBoardMatches();
  $("undo-game").disabled = busy || !!isLive() || !gameUndo.length;
  $("edit-history-move").disabled = busy || historyIndex === 0;
  $("save-note").disabled = busy || historyIndex === 0;
  $("challenge-move").disabled = busy || historyIndex === 0 || !!session;
  renderReviewTable();
}
function showHistory(index, {auto = true} = {}) {
  if (busy || !recordedGame) return;
  if (isLive()) {
    chargeClock();
    session.paused = true;
    clearTimeout(botTimer);
  }
  historyIndex = Math.max(
    0,
    Math.min(index, recordedGame.positions.length - 1),
  );
  setPosition(
    restorePosition(recordedGame.positions[historyIndex]),
    false,
    true,
  );
  if (auto && $("auto-analyze").value !== "off" && !reviewRunning)
    analyze($("auto-analyze").value === "kibitz" ? "kibitz" : searchMode());
}
$("history-previous").addEventListener("click", () =>
  showHistory(historyIndex - 1),
);
$("history-next").addEventListener("click", () =>
  showHistory(historyIndex + 1),
);
$("history-position").addEventListener("change", () =>
  showHistory(Number($("history-position").value)),
);
let gcgLoading = false;
let chooserPending = false;
function renderGCGDiagnostics() {
  $("gcg-diagnostics-log").textContent = gcgLogText();
}
document.addEventListener("gcg-diagnostics", renderGCGDiagnostics);
renderGCGDiagnostics();
logGCG("page.ready", {
  diagnosticsVersion: 1,
  browser: navigator.userAgent,
  isolated: crossOriginIsolated,
});
$("copy-gcg-diagnostics").addEventListener("click", async () => {
  try {
    await navigator.clipboard.writeText(gcgLogText());
    $("copy-gcg-diagnostics").textContent = "Copied";
  } catch {
    const selection = getSelection();
    const range = document.createRange();
    range.selectNodeContents($("gcg-diagnostics-log"));
    selection.removeAllRanges();
    selection.addRange(range);
    $("copy-gcg-diagnostics").textContent = "Select and copy log";
  }
});
for (const name of ["focus", "blur", "pageshow", "pagehide"]) {
  window.addEventListener(name, () => {
    if (chooserPending || gcgLoading) {
      logGCG(`page.${name}`, { chooserPending });
    }
  });
}
document.addEventListener("visibilitychange", () => {
  if (chooserPending || gcgLoading) {
    logGCG("page.visibility", { state: document.visibilityState, chooserPending });
  }
});
function openGameImport() {
  logGCG("dialog.open", { busy });
  $("copy-gcg-diagnostics").textContent = "Copy log";
  $("gcg-load-error").textContent = "";
  $("gcg-load-status").textContent = "";
  $("gcg-dialog").showModal();
}
$("load-gcg").addEventListener("click", openGameImport);
$("close-gcg").addEventListener("click", () => $("gcg-dialog").close());
$("choose-gcg").addEventListener("click", () => {
  chooserPending = true;
  logGCG("chooser.request", { busy, focused: document.hasFocus() });
  try {
    $("gcg-file").click();
    // Returning from click does not mean the native chooser has closed.
    logGCG("chooser.click-returned");
  } catch (failure) {
    chooserPending = false;
    logGCG("chooser.error", { message: failure.message });
    $("gcg-load-error").textContent = failure.message;
  }
});
$("gcg-file").addEventListener("cancel", () => {
  chooserPending = false;
  logGCG("chooser.cancel");
});
$("gcg-file").addEventListener("change", () => {
  chooserPending = false;
  const file = $("gcg-file").files[0];
  logGCG("chooser.selection", { files: $("gcg-file").files.length, bytes: file?.size });
  $("gcg-file").value = "";
  if (file) loadGame(file);
});
$("gcg-form").addEventListener("submit", (event) => {
  event.preventDefault();
  loadGame(null, $("gcg-text").value);
});
$("gcg-drop-zone").addEventListener("dragover", (event) => {
  if ([...event.dataTransfer.types].includes("Files")) {
    event.preventDefault();
    event.dataTransfer.dropEffect = busy ? "none" : "copy";
  }
});
$("gcg-drop-zone").addEventListener("drop", (event) => {
  event.preventDefault();
  if (busy) return;
  const files = event.dataTransfer.files;
  if (files.length !== 1) {
    $("gcg-load-error").textContent = "Drop one GCG file at a time.";
    return;
  }
  loadGame(files[0]);
});
async function loadGame(file, pastedText = "") {
  if (busy) {
    logGCG("load.skipped-busy");
    return;
  }
  gcgLoading = true;
  const started = performance.now();
  let stage = "input";
  const progress = (next, details = {}) => {
    stage = next;
    logGCG(next, { elapsedMs: Math.round(performance.now() - started), ...details });
  };
  progress("load.start", { source: file ? "file" : "paste", bytes: file?.size ?? new Blob([pastedText]).size });
  const watchdog = setInterval(() => logGCG("load.waiting", {
    stage,
    elapsedMs: Math.round(performance.now() - started),
    enginePending: engine?.pending?.response || null,
  }), 5000);
  try {
    if ((file?.size ?? new Blob([pastedText]).size) > 1024 * 1024)
      throw new Error("GCG files must be smaller than 1 MB.");
    setBusy(true);
    $("stop").disabled = true;
    error("");
    message("Loading game…");
    $("gcg-load-error").textContent = "";
    $("gcg-load-status").textContent = "Loading game…";
    const { text, lexicon } = await readGCG(file, pastedText, position.lexicon, progress);
    progress("preview.wait");
    await previewPromise;
    progress("engine.prepare", { lexicon });
    await engine.prepare(lexicon);
    progress("engine.import");
    const { game } = await engine.importGCG(text, lexicon);
    progress("engine.imported", { events: game.positions.length - 1 });
    restoreFinishedSession(game, text);
    game.filename = (file?.name || "pasted-game.gcg").replace(/\.gcg$/i, "") + ".gcg";
    gameUndo = [];
    session = null;
    clearTimeout(botTimer);
    analysisCache.clear();
    progress("board.render");
    adoptGame(game, lexicon, 0);
    progress("load.complete");
    message(`Loaded ${file?.name || "pasted game"} · ${game.positions.length - 1} events`);
    $("gcg-dialog").close();
    $("gcg-text").value = "";
  } catch (failure) {
    error(failure.message);
    $("gcg-load-error").textContent = failure.message;
    logGCG("load.failed", { stage, message: failure.message, stack: failure.stack });
    $("gcg-diagnostics").open = true;
    message("Game could not be loaded");
  } finally {
    gcgLoading = false;
    clearInterval(watchdog);
    $("gcg-load-status").textContent = "";
    setBusy(false);
  }
}
$("save-gcg").addEventListener("click", () => {
  if (!canExportGame()) return;
  const url = URL.createObjectURL(
    new Blob([recordedGame.gcg], { type: "text/plain;charset=utf-8" }),
  );
  const link = document.createElement("a");
  link.href = url;
  link.download = recordedGame.filename;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
  message("GCG saved · complete recorded game");
});
$("stop").addEventListener("click", stop);
document
  .querySelectorAll("[data-mode]")
  .forEach((button) =>
    button.addEventListener("click", () => analyze(button.dataset.mode, {
      resume: button.dataset.mode === "sim",
    })),
  );
document.addEventListener("keydown", (event) => {
  // Board input may replace its event target while the event is bubbling.
  if (event.defaultPrevented) return;
  if (settingsOpen) {
    if (event.key === "Escape") {
      event.preventDefault();
      closeSettings();
    }
    return;
  }
  if (event.key === "Escape" && busy && !$("stop").disabled) {
    event.preventDefault();
    stop();
    return;
  }
});
function recordedBoardMatches() {
  if (!recordedGame) return false;
  const saved = restorePosition(recordedGame.positions[historyIndex]);
  return (
    position.lexicon === saved.lexicon &&
    position.onTurn === saved.onTurn &&
    position.zeros === saved.zeros &&
    JSON.stringify(position.board) === JSON.stringify(saved.board) &&
    JSON.stringify(position.scores) === JSON.stringify(saved.scores)
  );
}
function adoptGame(game, lexicon, index = game.index || 0) {
  for (const saved of game.positions) {
    if (!saved.cgp.includes(" -lex ")) saved.cgp += ` -lex ${lexicon}`;
    restorePosition(saved);
  }
  recordedGame = game;
  historyIndex = Math.min(index, game.positions.length - 1);
  $("game-players").textContent = game.players.join(" vs ");
  const events = game.gcg
    .split(/\r?\n/)
    .filter((line) => line.startsWith(">"));
  $("history-position").replaceChildren(
    new Option("Start of game", "0"),
    ...events.map((line, index) => {
      const colon = line.indexOf(":");
      const player = line.slice(1, colon);
      const play = formatHistoryPlay(
        line, restorePosition(game.positions[index]),
      );
      return new Option(
        `${index + 1}. ${play} · ${player}`,
        String(index + 1),
      );
    }),
  );
  setPosition(restorePosition(game.positions[historyIndex]), false, true);
}
function saveGameUndo() {
  if (recordedGame && !isLive())
    gameUndo.push({
      game: structuredClone(recordedGame),
      index: historyIndex,
      position: snapshotPosition(position),
      session: session ? structuredClone(session) : null,
    });
  if (gameUndo.length > 20) gameUndo.shift();
}
function confirmGame(title, text, label = "Continue") {
  $("confirm-title").textContent = title;
  $("confirm-message").textContent = text;
  $("confirm-yes").textContent = label;
  const dialog = $("game-confirm");
  dialog.returnValue = "cancel";
  dialog.showModal();
  return new Promise((resolve) =>
    dialog.addEventListener(
      "close",
      () => resolve(dialog.returnValue === "yes"),
      { once: true },
    ),
  );
}
$("confirm-yes").addEventListener("click", () =>
  $("game-confirm").close("yes"),
);
$("confirm-no").addEventListener("click", () =>
  $("game-confirm").close("cancel"),
);
function recallTiles(render = true) {
  cancelDraftCheck();
  draftTiles = [];
  rackChoice = null;
  $("move-feedback").textContent = "";
  if (render) renderPosition();
}
function cancelDraftCheck() {
  clearTimeout(previewTimer);
  previewGeneration++;
}
function scheduleDraftCheck() {
  cancelDraftCheck();
  if (!recordedGame || !recordedBoardMatches()) return;
  let move;
  try {
    move = moveNotation(position, draftTiles, vertical);
    if (!move) return;
  } catch {
    return;
  }
  const generation = previewGeneration;
  previewTimer = setTimeout(() => {
    if (busy || !engine || engine.pending) return;
    previewPromise = engine
      .gameAction({
        text: recordedGame.gcg,
        lexicon: position.lexicon,
        index: historyIndex,
        rack: position.racks[0],
        move,
        action: 1,
      })
      .then(({ game }) => {
        if (generation !== previewGeneration || busy) return;
        $("move-feedback").textContent =
          `${game.score} points${game.warning.trim() ? " · Word not in lexicon" : ""}`;
      })
      .catch((failure) => {
        if (generation === previewGeneration && !busy)
          $("move-feedback").textContent = failure.message;
      });
  }, 220);
}
function renderDraft() {
  continuationPosition = null;
  selectedMove = "";
  error("");
  try {
    moveNotation(position, draftTiles, vertical);
    $("move-feedback").textContent = draftTiles.length
      ? `${draftTiles.length} tile${draftTiles.length === 1 ? "" : "s"} placed`
      : "";
  } catch (failure) {
    $("move-feedback").textContent = failure.message;
  }
  renderResults();
  renderPosition();
  scheduleDraftCheck();
}
function advanceCursor() {
  if (!cursor) return;
  do {
    if (vertical) cursor.row++;
    else cursor.col++;
  } while (
    cursor.row < 15 &&
    cursor.col < 15 &&
    position.board[cursor.row][cursor.col]
  );
  if (cursor.row >= 15 || cursor.col >= 15) cursor = null;
}
function addEntryTile(row, col, letter, blank = false, source = null, rackIndex = null) {
  try {
    const rest = source
      ? draftTiles.filter(
          (tile) => tile.row !== source.row || tile.col !== source.col,
        )
      : draftTiles;
    draftTiles = placeTile(position, rest, row, col, letter, blank, rackIndex);
    cursor = { row, col };
    advanceCursor();
    rackChoice = null;
    renderDraft();
    focusCursor();
  } catch (failure) {
    error(failure.message);
  }
}
let blankTarget = null;
function chooseBlank(row, col, source = null, rackIndex = null, apply = null) {
  blankTarget = { row, col, source, rackIndex, apply, generation:positionGeneration, editing:$("edit").checked };
  $("blank-dialog").showModal();
}
for (const letter of "ABCDEFGHIJKLMNOPQRSTUVWXYZ") {
  const button = document.createElement("button");
  button.textContent = letter;
  button.setAttribute("aria-label", `Blank as ${letter}`);
  button.addEventListener("click", () => {
    const target = blankTarget;
    $("blank-dialog").close();
    if (!target || busy || target.generation !== positionGeneration || target.editing !== $("edit").checked ||
        (isLive() && (!humanTurn() || session.paused))) return;
    if (target.apply) target.apply(letter.toLowerCase());
    else addEntryTile(target.row, target.col, letter, true, target.source, target.rackIndex);
  });
  $("blank-letters").append(button);
}
$("cancel-blank").addEventListener("click", () => $("blank-dialog").close());
$("blank-dialog").addEventListener("close", () => {
  blankTarget = null;
});
function entryClick(event) {
  if (terminalPosition()) return;
  if (isLive() && (!humanTurn() || session.paused)) return;
  const cell = event.target.closest("[data-row]");
  if (!cell) return;
  const row = +cell.dataset.row,
    col = +cell.dataset.col;
  if (rackChoice) {
    if (rackChoice.letter === "?") chooseBlank(row, col, null, rackChoice.index);
    else addEntryTile(row, col, rackChoice.letter, false, null, rackChoice.index);
    return;
  }
  if (cursor?.row === row && cursor?.col === col) vertical = !vertical;
  cursor = { row, col };
  while (cursor && position.board[cursor.row][cursor.col]) advanceCursor();
  renderBoard();
  focusCursor();
}
function entryKey(event) {
  if (terminalPosition()) return;
  if (isLive() && (!humanTurn() || session.paused)) return;
  if (busy || event.ctrlKey || event.metaKey || event.altKey) return;
  const arrows = {
    ArrowUp: [-1, 0],
    ArrowDown: [1, 0],
    ArrowLeft: [0, -1],
    ArrowRight: [0, 1],
  };
  if (event.key === "Enter") {
    event.preventDefault();
    submitMove();
    return;
  }
  if (event.key === "Escape") {
    event.preventDefault();
    recallTiles();
    return;
  }
  if (event.key === "Backspace" || event.key === "Delete") {
    event.preventDefault();
    const last = draftTiles.pop();
    if (last) cursor = { row: last.row, col: last.col };
    renderDraft();
    focusCursor();
    return;
  }
  if (event.key === " ") {
    event.preventDefault();
    vertical = !vertical;
    // Like native entry, toggling direction reflows uncommitted tiles from
    // their first square, skipping fixed board tiles.
    if (draftTiles.length) {
      const previous = [...draftTiles];
      const first = previous[0];
      cursor = { row: first.row, col: first.col };
      let next = [];
      try {
        for (const tile of previous) {
          if (!cursor) throw new Error("The move runs past the board edge.");
          while (cursor && position.board[cursor.row][cursor.col])
            advanceCursor();
          if (!cursor) throw new Error("The move runs past the board edge.");
          next = placeTile(
            position,
            next,
            cursor.row,
            cursor.col,
            tile.letter,
            tile.letter === tile.letter.toLowerCase(),
            tile.rackIndex,
          );
          advanceCursor();
        }
        draftTiles = next;
      } catch (failure) {
        vertical = !vertical;
        draftTiles = previous;
        error(failure.message);
      }
    }
    renderDraft();
    focusCursor();
    return;
  }
  if (arrows[event.key]) {
    event.preventDefault();
    cursor ||= { row: 7, col: 7 };
    cursor = {
      row: Math.max(0, Math.min(14, cursor.row + arrows[event.key][0])),
      col: Math.max(0, Math.min(14, cursor.col + arrows[event.key][1])),
    };
    renderBoard();
    focusCursor();
    return;
  }
  if (/^[a-zA-Z]$/.test(event.key)) {
    event.preventDefault();
    cursor ||= { row: 7, col: 7 };
    while (cursor && position.board[cursor.row][cursor.col]) advanceCursor();
    if (cursor)
      addEntryTile(cursor.row, cursor.col, event.key, event.shiftKey);
  }
}
async function submitMove(value = null, computer = false) {
  if (terminalPosition()) return;
  if (busy || (isLive() && !computer && (!humanTurn() || session.paused)))
    return;
  const generation = positionGeneration;
  try {
    if (!recordedGame) throw new Error("Start or load a game first.");
    if (!recordedBoardMatches())
      throw new Error(
        "Return to a recorded board before committing a move. Rack changes are allowed.",
      );
    const pendingMove = value || moveNotation(position, draftTiles, vertical);
    if (!pendingMove) throw new Error("Place tiles on the board first.");
    const move = normalizeMove(pendingMove);
    if (
      historyIndex < recordedGame.positions.length - 1 &&
      !(await confirmGame(
        "Replace later history?",
        "This move will replace every event after this position. Undo game change can restore the complete previous record.",
        "Replace and play",
      ))
    )
      return;
    if (generation !== positionGeneration) return;
    setBusy(true);
    $("stop").disabled = true;
    message("Validating move…");
    error("");
    await previewPromise;
    await engine.prepare(position.lexicon);
    if (generation !== positionGeneration) return;
    let { game } = await engine.gameAction(
      isLive()
        ? liveAction(5, move)
        : {
            text: recordedGame.gcg,
            lexicon: position.lexicon,
            index: historyIndex,
            rack: position.racks[0],
            move,
            action: 1,
          },
    );
    if (generation !== positionGeneration) return;
    if (
      !isLive() &&
      game.warning.trim() &&
      !(await confirmGame(
        "Word not in the lexicon",
        game.warning +
          "\nRecord the play anyway? You can challenge it afterward.",
        "Record play",
      ))
    ) {
      message("Move not recorded");
      return;
    }
    if (generation !== positionGeneration) return;
    saveGameUndo();
    game.filename = recordedGame.filename;
    if (isLive()) {
      chargeClock();
      game.positions.at(-1).clocks = [...session.remaining];
      for (let index = 0; index <= historyIndex; index++)
        game.positions[index] = structuredClone(
          recordedGame.positions[index],
        );
    }
    adoptGame(game, position.lexicon);
    if (isLive()) {
      session.started = performance.now();
      if (game.ended) finishSession("Game over");
    }
    message("Move recorded");
  } catch (failure) {
    if (generation !== positionGeneration) return;
    error(failure.message);
    message("Move could not be recorded");
  } finally {
    setBusy(false);
    if ($("status").textContent === "Move recorded" && !session)
      $("rack").focus();
    renderSession();
    scheduleComputer();
  }
}
$("play-move").addEventListener("click", () => submitMove());
$("pass-move").addEventListener("click", () => submitMove("pass"));
$("recall-tiles").addEventListener("click", () => recallTiles());
function openExchange(selected = "") {
  if (busy || terminalPosition() || (isLive() && (!humanTurn() || session.paused))) return;
  recallTiles();
  const remaining = [...selected];
  $("exchange-tiles").dataset.owner = position.onTurn;
  $("exchange-tiles").replaceChildren();
  for (const source of $("rack-tiles").children) {
    const tile = source.cloneNode(true);
    tile.disabled = false;
    const index = remaining.indexOf(tile.dataset.letter);
    if (index >= 0) remaining.splice(index, 1);
    tile.setAttribute("aria-pressed", String(index >= 0));
    tile.addEventListener("click", () => {
      tile.setAttribute("aria-pressed", String(tile.getAttribute("aria-pressed") !== "true"));
      updateExchange();
    });
    $("exchange-tiles").append(tile);
  }
  updateExchange();
  $("exchange-dialog").showModal();
}
function updateExchange() {
  const count = $("exchange-tiles").querySelectorAll('[aria-pressed="true"]').length;
  $("confirm-exchange").disabled = count === 0;
  $("confirm-exchange").textContent = count ? `Exchange ${count} tile${count === 1 ? "" : "s"}` : "Exchange";
}
$("exchange-move").addEventListener("click", () => openExchange());
$("cancel-exchange").addEventListener("click", () => $("exchange-dialog").close());
$("confirm-exchange").addEventListener("click", () => {
  const letters = [...$("exchange-tiles").querySelectorAll('[aria-pressed="true"]')]
    .map(tile => tile.dataset.letter).join("");
  if (!letters) return;
  $("exchange-dialog").close();
  submitMove(`ex ${letters}`);
});
$("shuffle-rack").addEventListener("click", () => {
  if (busy || (isLive() && !humanTurn())) return;
  const rack = [...position.racks[0]];
  for (let index = rack.length - 1; index > 0; index--) {
    const other = Math.floor(Math.random() * (index + 1));
    [rack[index], rack[other]] = [rack[other], rack[index]];
  }
  position.racks[0] = rack.join("");
  renderPosition();
});
$("sort-rack").addEventListener("click", () => {
  if (busy || (isLive() && !humanTurn())) return;
  position.racks[0] = [...position.racks[0]]
    .sort((a, b) => (a === "?" ? 1 : b === "?" ? -1 : a.localeCompare(b)))
    .join("");
  renderPosition();
});
$("new-game").addEventListener("click", () =>
  $("new-game-dialog").showModal(),
);
$("cancel-new-game").addEventListener("click", () =>
  $("new-game-dialog").close(),
);
$("new-game-form").addEventListener("submit", async (event) => {
  event.preventDefault();
  if (busy) return;
  const players = [
    $("player-one-name").value,
    $("player-two-name").value,
  ].map((name) => name.replace(/[\r\n\0]/g, " ").trim());
  if (players.some((name) => !name)) return;
  if (isLive() && !(await confirmGame("Start a new game?", "The current game will end and be replaced.", "New game"))) return;
  $("new-game-dialog").close();
  try {
    setBusy(true);
    $("stop").disabled = true;
    error("");
    message("Starting game…");
    await previewPromise;
    await engine.prepare(position.lexicon);
    const kind = $("game-kind").value;
    const first =
      kind === "record"
        ? 0
        : $("first-player").value === "random"
          ? randomSeed() % 2
          : +$("first-player").value;
    if (first) players.reverse();
    const text = `#character-encoding UTF-8\n#lexicon ${position.lexicon}\n#player1 p1 ${players[0]}\n#player2 p2 ${players[1]}\n`;
    let { game } = await engine.importGCG(text, position.lexicon);
    saveGameUndo();
    analysisCache.clear();
    session = null;
    if (kind !== "record") {
      const response = await engine.gameAction({
        text,
        lexicon: position.lexicon,
        index: 0,
        action: 4,
        seed: randomSeed(),
        onTurn: 0,
      });
      game = response.game;
      session = sessionOptions(kind);
      if (first) session.human = 1 - session.human;
      game.positions[0].clocks = [...session.remaining];
    }
    game.filename = "magpie-game.gcg";
    adoptGame(game, position.lexicon, 0);
    message(session ? "Game ready" : "Game ready · enter the first rack");
    if (!session) {
      $("position-editor").open = true;
      $("rack").focus();
    }
  } catch (failure) {
    error(failure.message);
  } finally {
    setBusy(false);
    renderSession();
    scheduleComputer();
  }
});
$("undo-game").addEventListener("click", () => {
  if (busy || isLive() || !gameUndo.length) return;
  const previous = gameUndo.pop();
  session = previous.session || null;
  if (session) {
    session.paused = true;
    session.started = performance.now();
  }
  adoptGame(
    previous.game,
    restorePosition(previous.position).lexicon,
    previous.index,
  );
  position = restorePosition(previous.position);
  renderPosition();
  message("Game change undone");
});
function editHistoryMove() {
  if (!recordedGame || !historyIndex || busy) return;
  const line = recordedGame.gcg
    .split(/\r?\n/)
    .filter((line) => line.startsWith(">"))[historyIndex - 1];
  const match = line.match(/^>[^:]+:\s*(\S+)\s+(\S+)(?:\s+(\S+))?/);
  if (!match) return;
  let move;
  if (match[2] === "-") move = "pass";
  else if (/^-[A-Z?]+$/i.test(match[2])) move = `ex ${match[2].slice(1)}`;
  else if (/^([A-O]\d+|\d+[A-O])$/i.test(match[2]))
    move = `${match[2]} ${match[3]}`;
  else {
    error(
      "Navigate to before the event and play a replacement move, or undo the last game change.",
    );
    return;
  }
  showHistory(historyIndex - 1, {auto:false});
  position.racks[0] = match[1];
  renderPosition();
  if (move.startsWith("ex ")) {
    openExchange(move.slice(3));
  } else if (move === "pass") {
    $("pass-move").focus();
  } else {
    draftTiles = previewMove(position, move);
    vertical = /^[A-O]/.test(move);
    cursor = draftTiles.length ? { row: draftTiles[0].row, col: draftTiles[0].col } : null;
    renderDraft();
    focusCursor();
  }
}
$("edit-history-move").addEventListener("click", editHistoryMove);
async function changeRecordedEvent(action) {
  if (busy || !recordedGame || !historyIndex) return;
  const note = $("event-note").value;
  if (
    action === 3 &&
    historyIndex < recordedGame.positions.length - 1 &&
    !(await confirmGame(
      "Challenge at this position?",
      "A successful challenge replaces later history. Undo game change restores the previous record.",
      "Challenge",
    ))
  )
    return;
  try {
    setBusy(true);
    $("stop").disabled = true;
    error("");
    await previewPromise;
    await engine.prepare(position.lexicon);
    const { game } = await engine.gameAction({
      text: recordedGame.gcg,
      lexicon: restorePosition(recordedGame.positions[0]).lexicon,
      index: historyIndex,
      note,
      action,
    });
    saveGameUndo();
    game.filename = recordedGame.filename;
    adoptGame(game, restorePosition(recordedGame.positions[0]).lexicon);
    message(action === 2 ? "Note saved" : "Challenge recorded");
  } catch (failure) {
    error(failure.message);
  } finally {
    setBusy(false);
  }
}
$("save-note").addEventListener("click", () => changeRecordedEvent(2));
$("challenge-move").addEventListener("click", () => changeRecordedEvent(3));
// During play, only draft tiles move. Explicit position editing can also move
// recorded tiles, preserving ownership and participating in editor undo.
function canDragTiles() {
  return !busy && !terminalPosition() && !(isLive() && (!humanTurn() || session.paused));
}
function rackDrop(source, gap) {
  const next = reorderRack(position.racks[0], draftTiles, source.rackIndex, gap);
  position.racks[0] = next.rack;
  draftTiles = next.tiles.filter(tile => !source.square || tile.row !== source.row || tile.col !== source.col);
  rackChoice = null;
  if (source.square) renderDraft();
  else renderPosition();
}
tileDrag = installTileDrag({
  board:$("board"), rack:$("rack-tiles"),
  sourceFor(target) {
    if (!canDragTiles()) return null;
    const rackTile = target.closest("#rack-tiles .rack-tile");
    if (rackTile) return {element:rackTile, letter:rackTile.dataset.letter, rackIndex:+rackTile.dataset.rackIndex};
    if (!recordedGame && !$("edit").checked) return null;
    const square = target.closest("#board .square");
    if (!square) return null;
    const row = +square.dataset.row, col = +square.dataset.col;
    if ($("edit").checked) {
      const letter = position.board[row][col];
      return letter ? {element:square, square, row, col, letter} : null;
    }
    const index = draftTiles.findIndex(tile => tile.row === row && tile.col === col);
    return index < 0 ? null : {element:square, square, row, col, letter:draftTiles[index].letter,
      rackIndex:rackSlots(position.racks[0], draftTiles).indexes[index]};
  },
  targetFor(x, y, source) {
    if (!canDragTiles()) return null;
    const target = document.elementFromPoint(x, y);
    const rack = target?.closest("#rack-tiles");
    if (rack) {
      if ($("edit").checked && source.square && position.racks[0].length >= 7) return null;
      const tiles = [...rack.children];
      const before = tiles.findIndex(tile => { const rect = tile.getBoundingClientRect(); return x < rect.left + rect.width / 2; });
      return {rack, gap:before < 0 ? tiles.length : before};
    }
    if (!recordedGame && !$("edit").checked) return null;
    const square = target?.closest("#board .square");
    if (!square) return null;
    const row = +square.dataset.row, col = +square.dataset.col;
    if (source.square === square) return {square, row, col};
    if (position.board[row][col] || draftTiles.some(tile => tile.row === row && tile.col === col)) return null;
    return {square, row, col};
  },
  onTap(source) {
    if (source.square || $("edit").checked || !recordedGame) return;
    rackChoice = rackChoice?.index === source.rackIndex ? null : {letter:source.letter, index:source.rackIndex};
    renderPosition();
  },
  onDrop(source, target) {
    if (!canDragTiles() || source.square === target.square && source.square) return;
    if (!source.square && target.rack) { rackDrop(source, target.gap); return; }
    if ($("edit").checked) {
      const apply = letter => {
        const next = structuredClone(position);
        const owner = source.square ? next.owners[source.row][source.col] : Number($("tile-owner").value);
        if (source.square) { next.board[source.row][source.col] = ""; next.owners[source.row][source.col] = -1; }
        else next.racks[0] = [...next.racks[0]].filter((_, index) => index !== source.rackIndex).join("");
        if (target.rack) {
          const rack = [...next.racks[0]];
          rack.splice(target.gap, 0, letter === letter.toLowerCase() ? "?" : letter);
          next.racks[0] = rack.join("");
        } else { next.board[target.row][target.col] = letter; next.owners[target.row][target.col] = owner; }
        commitEdit(next);
      };
      if (source.letter === "?" && target.square) chooseBlank(target.row, target.col, null, source.rackIndex, apply);
      else apply(source.letter);
    } else if (target.rack) rackDrop(source, target.gap);
    else if (source.letter === "?") chooseBlank(target.row, target.col, null, source.rackIndex);
    else addEntryTile(target.row, target.col, source.letter, source.letter === source.letter.toLowerCase(), source.square ? source : null, source.rackIndex);
  },
});
$("rack-tiles").addEventListener("keydown", (event) => {
  if (!canDragTiles() || !event.target.dataset.letter) return;
  const index = +event.target.dataset.rackIndex;
  if (event.altKey && ["ArrowLeft", "ArrowRight"].includes(event.key)) {
    event.preventDefault();
    const slots = rackSlots(position.racks[0], draftTiles).remaining;
    const from = slots.findIndex(slot => slot.index === index);
    const to = from + (event.key === "ArrowLeft" ? -1 : 1);
    if (to < 0 || to >= slots.length) return;
    rackDrop({rackIndex:index}, to < from ? to : to + 1);
    $("rack-tiles").children[to]?.focus();
  } else if ((event.key === "Enter" || event.key === " ") && recordedGame && !$("edit").checked) {
    event.preventDefault();
    rackChoice = {letter:event.target.dataset.letter, index};
    renderPosition();
    $("board").querySelector('[tabindex="0"]')?.focus();
  }
});

function previewContinuation(steps) {
  continuationPosition = structuredClone(position);
  steps.forEach((move, index) => {
    for (const tile of previewMove(continuationPosition, move)) {
      continuationPosition.board[tile.row][tile.col] = tile.letter;
      continuationPosition.owners[tile.row][tile.col] =
        (position.onTurn + index) % 2;
    }
  });
  renderBoard();
}
function sessionOptions(kind) {
  return {
    kind,
    human: +$("human-side").value,
    paused: false,
    finished: false,
    time: +$("game-time").value,
    remaining: [+$("game-time").value, +$("game-time").value],
    overtime: $("overtime").value,
    penalty: $("time-penalty").value,
    challenge: $("challenge-rule").value,
    strategy: $("bot-analysis").value,
    seconds: Math.max(1, Math.min(120, +$("bot-seconds").value || 5)),
    delay: Math.max(0, Math.min(30, +$("watch-delay").value || 0)),
    started: performance.now(),
  };
}
function chargeClock() {
  if (!isLive()) return;
  const now = performance.now();
  if (
    !session.paused &&
    session.time &&
    historyIndex === recordedGame.positions.length - 1
  )
    session.remaining[position.onTurn] -= (now - session.started) / 1000;
  session.started = now;
}
function formatClock(seconds) {
  const rounded = Math.ceil(Math.abs(seconds));
  return `${seconds < 0 ? "−" : ""}${Math.floor(rounded / 60)}:${String(rounded % 60).padStart(2, "0")}`;
}
function renderSession() {
  const live = isLive();
  document.body.classList.toggle("playing", !!live);
  document.body.classList.toggle(
    "show-analysis",
    !live || session.kind === "watch" || $("show-live-analysis").checked,
  );
  $("play-session").hidden = !session;
  $("position-editor").hidden = !!live;
  for (const [index, suffix] of ["one", "two"].entries()) {
    $("name-" + suffix).textContent =
      recordedGame?.players[index] || `Player ${index + 1}`;
    $("points-" + suffix).textContent =
      position.scores[index === position.onTurn ? 0 : 1];
    $("turn-" + suffix).textContent =
      index === position.onTurn &&
      !terminalPosition() &&
      !session?.finished
        ? "On turn"
        : "";
    $("player-" + suffix).classList.toggle(
      "on-turn",
      index === position.onTurn,
    );
    $("clock-" + suffix).hidden = !session;
  }
  if (!session) {
    $("copy").hidden = false;
    return;
  }
  for (const [index, id] of ["clock-one", "clock-two"].entries()) {
    const clocks =
      historyIndex < recordedGame.positions.length - 1
        ? recordedGame.positions[historyIndex].clocks || session.remaining
        : session.remaining;
    $(id).textContent =
      `${session.time ? formatClock(clocks[index]) : "Untimed"}`;
    $(id).classList.toggle(
      "running",
      live && !session.paused && index === position.onTurn,
    );
    $(id).classList.toggle("overtime", session.remaining[index] < 0);
  }
  $("pause-game").textContent = session.paused ? "Resume game" : "Pause";
  $("pause-game").disabled = session.finished;
  $("step-game").hidden = session.kind !== "watch";
  $("step-game").disabled = busy || !session.paused || session.finished;
  $("finish-game").textContent =
    session.kind === "play" ? "Resign" : "End game";
  $("finish-game").disabled = session.finished || busy;
  const sessionMessage =
    session.result ||
    (session.paused
      ? "Paused"
      : botRunning
        ? `${recordedGame.players[position.onTurn]} thinking…`
        : recordedGame.players[position.onTurn] === "You"
          ? "Your turn"
          : `${recordedGame.players[position.onTurn]}’s turn`);
  if ($("session-result").textContent !== sessionMessage)
    $("session-result").textContent = sessionMessage;
  if (live) {
    $("shuffle-rack").disabled = busy || !humanTurn();
    $("sort-rack").disabled = busy || !humanTurn();
    for (const id of [
      "rack",
      "opponent-rack",
      "score",
      "opponent-score",
      "edit",
      "clear",
      "on-turn-player",
      "sample",
      "lexicon",
      "edit-history-move",
    ])
      $(id).disabled = true;
    for (const id of [
      "play-move",
      "pass-move",
      "exchange-move",
      "recall-tiles",
    ])
      $(id).disabled = busy || session.paused || !humanTurn();
    $("import").disabled = true;
    $("copy").hidden =
      session.kind === "play" && !$("show-live-analysis").checked;
  } else $("copy").hidden = false;
}
// The native GCG reader deliberately trims overtime on incomplete boards.
// A resignation/forfeit has no standard GCG event; our description marks it final.
function restoreFinishedSession(game, text) {
  if (!/^#description Magpie finished: /m.test(text)) return;
  const original = text.split(/\r?\n/).filter((line) => line.startsWith(">"));
  const retained = game.gcg
    .split(/\r?\n/)
    .filter((line) => line.startsWith(">"));
  const dropped = original.slice(retained.length);
  if (!dropped.every((line) => /^>[^:]+:\s+\(time\) -\d+ -?\d+$/.test(line)))
    return;
  const names = [1, 2].map(
    (index) => text.match(new RegExp(`^#player${index}\\s+(\\S+)`, "m"))?.[1],
  );
  const restored = [];
  const final = restorePosition(game.positions.at(-1));
  for (const line of dropped) {
    const [, name, penalty, total] = line.match(
      /^>([^:]+):\s+\(time\) -(\d+) (-?\d+)$/,
    );
    const player = names.indexOf(name);
    if (player < 0) return;
    const index = player === final.onTurn ? 0 : 1;
    if (final.scores[index] - Number(penalty) !== Number(total)) return;
    final.scores[index] = Number(total);
    restored.push({ ...snapshotPosition(final), note: "Time penalty" });
  }
  game.positions.push(...restored);
  game.gcg = text;
  game.ended = true;
}
function finishSession(reason) {
  if (!session || session.finished) return;
  chargeClock();
  positionGeneration++;
  canceled = true;
  engine?.stop();
  session.finished = true;
  recordedGame.ended = true;
  session.paused = true;
  clearTimeout(botTimer);
  position = restorePosition(recordedGame.positions.at(-1));
  historyIndex = recordedGame.positions.length - 1;
  const scores = position.onTurn
    ? [...position.scores].reverse()
    : [...position.scores];
  const penalties = session.remaining.map((time) =>
    time < 0 && session.time && session.overtime !== "0"
      ? session.penalty === "second"
        ? Math.ceil(-time)
        : Math.ceil(-time / 60) * 10
      : 0,
  );
  const adjusted = scores.map((score, index) => score - penalties[index]);
  session.result = `${reason} · ${recordedGame.players[0]} ${adjusted[0]} — ${adjusted[1]} ${recordedGame.players[1]}${penalties.some(Boolean) ? ` · Time penalties ${penalties.join(" / ")}` : ""}`;
  // GCG supports time penalties. Keep the exported totals consistent with the clocks.
  if (penalties.some(Boolean)) {
    recordedGame.gcg = recordedGame.gcg.replace(/^#rack[12][^\n]*\n?/gm, "");
    penalties.forEach((points, index) => {
      if (points) {
        const nickname =
          recordedGame.gcg.match(
            new RegExp(`^#player${index + 1}\\s+(\\S+)`, "m"),
          )?.[1] || `p${index + 1}`;
        recordedGame.gcg += `\n>${nickname}:  (time) -${points} ${adjusted[index]}\n`;
        position.scores[index === position.onTurn ? 0 : 1] = adjusted[index];
        recordedGame.positions.push({
          ...snapshotPosition(position),
          note: "Time penalty",
          clocks: [...session.remaining],
        });
      }
    });
  }
  recordedGame.gcg = recordedGame.gcg.replace(/^#description Magpie finished: [^\n]*\n?/gm, "");
  recordedGame.gcg = recordedGame.gcg.replace(
    /(#character-encoding[^\n]*\n)/,
    (_, encoding) =>
      `${encoding}#description Magpie finished: ${session.result}\n`,
  );
  adoptGame(
    recordedGame,
    position.lexicon,
    recordedGame.positions.length - 1,
  );
  renderSession();
  message(session.result);
}
function scheduleComputer() {
  clearTimeout(botTimer);
  if (
    !isLive() ||
    session.paused ||
    busy ||
    botRunning ||
    historyIndex !== recordedGame.positions.length - 1 ||
    humanTurn()
  )
    return;
  botTimer = setTimeout(
    () => computerTurn(),
    session.kind === "watch" ? session.delay * 1000 : 150,
  );
}
async function computerTurn(single = false) {
  if (!isLive() || busy || botRunning || (!single && session.paused)) return;
  botRunning = true;
  const currentSession = session;
  try {
    const nextMode = session.strategy === "kibitz" ? "kibitz" : searchMode();
    const budget = session.time
      ? Math.min(
          session.seconds,
          Math.max(1, session.remaining[position.onTurn] / 20),
        )
      : session.seconds;
    const complete = await analyze(nextMode, { seconds: budget });
    if (
      session !== currentSession ||
      session.finished ||
      (!single && session.paused) ||
      !complete
    )
      return;
    const move = results[0]?.move;
    if (!move) throw new Error("The computer did not return a move.");
    await submitMove(move, true);
    if ($("error").textContent) session.paused = true;
  } catch (failure) {
    error(failure.message);
    session.paused = true;
  } finally {
    botRunning = false;
    if (session === currentSession) {
      renderPosition();
      scheduleComputer();
    }
  }
}
function updateGameSetup() {
  const kind = $("game-kind").value;
  $("computer-setup").hidden = kind === "record";
  $("human-side").closest("label").hidden = kind !== "play";
  $("watch-delay").closest("label").hidden = kind !== "watch";
  document.querySelectorAll("[data-game-kind]").forEach((button) => {
    button.setAttribute(
      "aria-pressed",
      String(button.dataset.gameKind === kind),
    );
  });
  const defaults =
    kind === "record"
      ? ["Player 1", "Player 2"]
      : kind === "watch"
        ? ["Magpie 1", "Magpie 2"]
        : $("human-side").value === "0"
          ? ["You", "Magpie"]
          : ["Magpie", "You"];
  for (const [index, id] of [
    "player-one-name",
    "player-two-name",
  ].entries()) {
    const input = $(id);
    if (!input.dataset.custom) input.value = defaults[index];
  }
}
for (const id of ["player-one-name", "player-two-name"]) {
  $(id).addEventListener("input", () => {
    $(id).dataset.custom = "true";
  });
}
$("game-kind").addEventListener("change", updateGameSetup);
$("human-side").addEventListener("change", updateGameSetup);
document.querySelectorAll("[data-game-kind]").forEach((button) => {
  button.addEventListener("click", () => {
    $("game-kind").value = button.dataset.gameKind;
    updateGameSetup();
  });
});
updateGameSetup();
$("analysis-details").addEventListener("click", () =>
  $("analysis-details-dialog").showModal(),
);
$("close-analysis-details").addEventListener("click", () =>
  $("analysis-details-dialog").close(),
);
$("game-menu").addEventListener("click", (event) => {
  if (event.target.closest("button")) $("game-menu").open = false;
});
document.addEventListener("click", (event) => {
  if (!$("game-menu").contains(event.target))
    $("game-menu").open = false;
});
$("game-menu").addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    $("game-menu").open = false;
    $("game-menu").querySelector("summary").focus();
    event.stopPropagation();
  }
});
$("pause-game").addEventListener("click", () => {
  if (!isLive()) return;
  chargeClock();
  session.paused = !session.paused;
  if (session.paused) {
    clearTimeout(botTimer);
    if (busy) stop();
  } else {
    session.started = performance.now();
    if (historyIndex !== recordedGame.positions.length - 1)
      showHistory(recordedGame.positions.length - 1, {auto:false});
    session.paused = false;
    scheduleComputer();
  }
  renderPosition();
});
$("step-game").addEventListener("click", () => {
  if (!isLive() || busy || session.kind !== "watch" || !session.paused) return;
  if (historyIndex !== recordedGame.positions.length - 1)
    showHistory(recordedGame.positions.length - 1, {auto:false});
  computerTurn(true);
});
$("show-live-analysis").addEventListener("change", renderSession);
$("finish-game").addEventListener("click", async () => {
  if (!isLive()) return;
  if (
    !(await confirmGame(
      "End this game?",
      "The game will remain available for review and saving.",
      "End game",
    ))
  )
    return;
  const reason =
    session.kind === "play"
      ? `${recordedGame.players[session.human]} resigned`
      : "Game ended";
  if (busy) stop();
  finishSession(reason);
});
setInterval(() => {
  if (!isLive()) return;
  chargeClock();
  if (
    session.time &&
    session.overtime !== "unlimited" &&
    session.remaining[position.onTurn] < -Number(session.overtime)
  ) {
    if (busy) stop();
    finishSession(`${recordedGame.players[position.onTurn]} lost on time`);
  }
  renderSession();
}, 250);
async function branchGame(kind) {
  if (busy || !recordedGame) return;
  if (!recordedBoardMatches()) {
    error("Return to a recorded board before continuing the game. Rack changes are allowed.");
    return;
  }
  if (
    !(await confirmGame(
      "Continue from this position?",
      "Later moves will be replaced. After ending the game, Undo game change restores the original.",
      "Continue",
    ))
  )
    return;
  try {
    setBusy(true);
    await previewPromise;
    await engine.prepare(position.lexicon);
    const { game } = await engine.gameAction(liveAction(4));
    saveGameUndo();
    session = sessionOptions(kind);
    session.human = position.onTurn;
    game.filename = recordedGame.filename;
    adoptGame(game, position.lexicon, game.positions.length - 1);
  } catch (failure) {
    error(failure.message);
  } finally {
    setBusy(false);
    renderPosition();
    scheduleComputer();
  }
}
$("play-from").addEventListener("click", () => branchGame("play"));
$("watch-from").addEventListener("click", () => branchGame("watch"));
$("copy-gcg").addEventListener("click", async () => {
  if (!canExportGame()) return;
  try {
    await navigator.clipboard.writeText(recordedGame.gcg);
    message("GCG copied");
  } catch {
    error("Clipboard unavailable. Use Save GCG.");
  }
});
$("history-first").addEventListener("click", () => showHistory(0));
$("history-last").addEventListener("click", () =>
  showHistory(recordedGame.positions.length - 1),
);
$("hide-spoilers").addEventListener("change", () => {
  positionGeneration++;
  if (busy) stop();
  clearResults();
  restoreAnalysis();
  renderHistory();
  renderResults();
});
function playedMove(index = historyIndex) {
  const line = gameEvents()[index];
  const parsed = line?.match(/^>[^:]+:\s+\S+\s+(\S+)(?:\s+(\S+))?/);
  if (!parsed) return "";
  if (parsed[1] === "-") return "pass";
  if (/^-[A-Z?]+$/.test(parsed[1])) return `ex ${parsed[1].slice(1)}`;
  return /^([A-O]\d+|\d+[A-O])$/.test(parsed[1])
    ? `${parsed[1]} ${parsed[2]}`
    : "";
}
function moveIdentity(move) {
  const tiles = previewMove(position, move);
  return tiles.length
    ? JSON.stringify(tiles.sort((a, b) => a.row - b.row || a.col - b.col))
    : normalizeMove(move).replace(/^ex (.+)$/, (_, letters) => `ex ${[...letters].sort().join("")}`);
}
function playedAssessment() {
  if (!recordedGame || $("hide-spoilers").checked || !results.length)
    return "";
  const played = playedMove();
  if (!played) return "";
  const row = results.find(
    (candidate) => moveIdentity(candidate.move) === moveIdentity(played),
  );
  if (!row) return "Played move outside candidate list";
  const metric =
    mode === "sim" || mode === "peg"
      ? "win"
      : mode === "endgame"
        ? "spread"
        : "equity";
  if (mode === "peg" && busy) return `Played #${row.rank} · protected`;
  if (mode === "peg" && row.depth !== results[0].depth)
    return `Played #${row.rank} · comparison needs equal depth`;
  const loss = Math.max(0, results[0][metric] - row[metric]);
  return `Played #${row.rank} · ${format(loss, mode === "endgame" ? 0 : 2)} ${metric === "win" ? "win %-points" : metric} lost`;
}
function gameEvents() {
  return (
    recordedGame?.gcg.split(/\r?\n/).filter((line) => line.startsWith(">")) ||
    []
  );
}
function renderReviewTable() {
  if (!recordedGame) return;
  const hidden = $("hide-spoilers").checked;
  const events = gameEvents();
  const header = document.createElement("tr");
  for (const name of ["Turn", ...recordedGame.players]) {
    const cell = document.createElement("th");
    cell.textContent = name;
    header.append(cell);
  }
  $("history-table").tHead.replaceChildren(header);
  const rows = [];
  let cells, previousPlayer = 1;
  for (const [index, line] of events.entries()) {
    const parsed = line.match(/^>([^:]+):\s+(.*)$/);
    const player =
      parsed[1] === recordedGame.gcg.match(/^#player2\s+(\S+)/m)?.[1] ? 1 : 0;
    if (!cells || player <= previousPlayer) {
      const row = document.createElement("tr");
      const number = document.createElement("td");
      number.textContent = String(rows.length + 1);
      row.append(number);
      cells = [document.createElement("td"), document.createElement("td")];
      row.append(...cells);
      rows.push(row);
    }
    previousPlayer = player;
    const cell = cells[player];
    cell.classList.toggle("current", historyIndex === index);
    const button = document.createElement("button");
    button.dataset.eventIndex = index;
    button.textContent =
      hidden && index >= historyIndex
        ? "•••"
        : formatHistoryPlay(line, restorePosition(recordedGame.positions[index]));
    button.disabled = busy;
    button.addEventListener("click", (event) => {
      clearTimeout(historyClickTimer);
      if (event.detail === 0) showHistory(index);
      else {
        pendingHistoryIndex = index;
        historyClickTimer = setTimeout(() => showHistory(index), 250);
      }
    });
    button.addEventListener("dblclick", () => {
      clearTimeout(historyClickTimer);
      showHistory(index + 1, {auto:false});
      editHistoryMove();
    });
    cell.append(button);
  }
  $("history-table").tBodies[0].replaceChildren(...rows);
  for (const [index, option] of [
    ...$("history-position").options,
  ].entries()) {
    if (!option.dataset.fullLabel)
      option.dataset.fullLabel = option.textContent;
    option.textContent =
      hidden && index > historyIndex
        ? `${index}. •••`
        : option.dataset.fullLabel;
  }
  $("history-first").disabled = busy || historyIndex === 0;
  $("history-last").disabled =
    busy || historyIndex === recordedGame.positions.length - 1;
  $("review-game").disabled = busy || !!isLive();
  const terminal =
    recordedGame.ended && historyIndex === recordedGame.positions.length - 1;
  $("play-from").disabled = busy || !!isLive() || terminal;
  $("watch-from").disabled = busy || !!isLive() || terminal;
}
function restoreAnalysis() {
  const saved =
    analysisCache.get(cacheKey(searchMode())) ||
    analysisCache.get(cacheKey("kibitz"));
  if (!saved) return;
  mode = saved.mode;
  results = structuredClone(saved.results);
  analysisMeta = saved.meta;
  analysisIterations = saved.iterations || 0;
  pegProgress = structuredClone(saved.pegProgress || null);
  $("raw").textContent = saved.raw;
  renderResults();
}
$("fresh-analysis").addEventListener("click", () => analyze(mode, {resume:false}));
$("review-game").addEventListener("click", async () => {
  if (busy || !recordedGame || isLive()) return;
  reviewRunning = true;
  const original = historyIndex,
    game = recordedGame;
  let failed = 0;
  $("stop-review").hidden = false;
  for (
    let index = 0;
    index < game.positions.length - 1 &&
    reviewRunning &&
    game === recordedGame;
    index++
  ) {
    if (!playedMove(index)) continue;
    showHistory(index);
    $("review-progress").textContent =
      `Analyzing turn ${index + 1} of ${game.positions.length - 1}`;
    if (position.racks[0] && !(await analyze(searchMode())) && reviewRunning)
      failed++;
  }
  const completed = reviewRunning;
  reviewRunning = false;
  $("stop-review").hidden = true;
  showHistory(original);
  $("review-progress").textContent = completed
    ? failed
      ? `Review complete · ${failed} failed`
      : "Review complete"
    : "Review stopped";
});
$("stop-review").addEventListener("click", () => {
  reviewRunning = false;
  stop();
});
function analysisSummary(text, kind) {
  if (kind === "sim") {
    return `${$("plies").value} plies · ${analysisIterations.toLocaleString()} total iterations`;
  }
  if (kind === "endgame") {
    const info = text.match(/depth:\s*(\d+).*?status:\s*([^\)\n]+)/);
    return info ? `${info[1]} plies · ${info[2]}` : "Solving…";
  }
  if (kind === "peg")
    return `${inventory(position).bagCount} in bag · ${results[0]?.depth || "Solving"}`;
  return "Static equity";
}

const controlHints = {
  "copy-gcg": "Copy the complete game as GCG.",
  "pause-game": "Pause the game clock and computer; resume when ready.",
  "step-game": "Play one computer turn while watching is paused.",
  "finish-game": "End the game and keep its history for review.",
  "show-live-analysis":
    "Reveal analysis while playing. This can reveal the computer’s rack.",
  "history-first": "Review the opening position.",
  "history-last": "Return to the end of the game.",
  "hide-spoilers": "Hide upcoming moves until you step past them.",
  "play-from":
    "Take over the player on turn; keep the original game in Undo.",
  "watch-from": "Let the computer continue from this position.",
  "review-game": "Analyze each turn and keep its results for review.",
  "stop-review": "Stop the review and keep completed analysis.",
  "fresh-analysis":
    "Discard this position’s previous results and start a new search.",
  "auto-analyze": "Analyze automatically when you select a turn in history.",
  "new-game":
    "Start a game with named players and enter each rack as you go.",
  "load-gcg": "Open a GCG game record.",
  "save-gcg": "Save the complete game, including moves and notes.",
  import: "Paste a CGP position. Lowercase letters are blanks.",
  copy: "Copy this position as CGP. Tile ownership is not included.",
  sample: "Load an example position.",
  recent: "Return to a recent position.",
  "history-position": "Choose a position in the game history.",
  "history-previous": "Go back one event.",
  "history-next": "Go forward one event.",
  "edit-history-move":
    "Edit this move. Replacing later history requires confirmation.",
  "undo-game": "Restore the game before the last recorded change.",
  "event-note": "Add a note to the selected event.",
  "save-note": "Save this note without changing later moves.",
  "challenge-move": "Challenge the last play using the current lexicon.",
  "play-move": "Enter: play the pending move.",
  "recall-tiles": "Esc: return unplayed tiles to the rack.",
  "pass-move": "Pass the turn without playing tiles.",
  "exchange-move": "Choose rack tiles to exchange.",
  "shuffle-rack": "Shuffle the tiles in your rack.",
  "sort-rack": "Sort the tiles in your rack alphabetically.",
  edit: "Edit board tiles directly. Shift + letter: blank · Space: direction · Delete: remove",
  undo: "Undo the last board edit.",
  clear: "Clear the board.",
  "tile-owner": "Choose the player whose tiles you are adding or recoloring.",
  "assign-owner": "Apply the chosen owner to the selected board tile.",
  "on-turn-player": "Choose the player on turn.",
  rack: "Enter up to seven tiles. Use ? for a blank.",
  "opponent-rack":
    "Enter known opponent tiles; leave unknown tiles empty. Use ? for a blank.",
  score: "Score for the player on turn.",
  "opponent-score": "Opponent’s score.",
  "open-settings": "Open game and display settings.",
  "open-lexicon-settings": "Change the word list.",
  "open-analysis-settings":
    "Adjust analysis time, candidates, lookahead and threads.",
  "back-to-game": "Return to the current game and analysis.",
  lexicon:
    "Changing the word list keeps the position and clears the analysis.",
  "premium-labels": "Show 2L, 3L, 2W and 3W on empty premium squares.",
  seconds: "Maximum seconds for a simulation or solve.",
  candidates: "Number of candidate moves compared in a simulation.",
  plies: "Number of turns ahead each simulation looks.",
  threads: "Worker threads used for simulations and solves.",
  "tt-memory": "Total transposition-table budget. PEG divides it between workers.",
  stop: "Esc: stop the current analysis.",
};
for (const [id, hint] of Object.entries(controlHints)) {
  $(id).dataset.contextHint = hint;
}
for (const button of document.querySelectorAll("[data-mode]")) {
  button.dataset.contextHint = button.title;
}
function contextTarget(element) {
  return element instanceof Element
    ? element.closest("[data-context-hint], #board, #rack-tiles, #results")
    : null;
}
function renderContextHint() {
  const target =
    hintTarget?.isConnected && hintTarget.getClientRects().length
      ? hintTarget
      : null;
  let hint = target?.dataset.contextHint || "";
  if (target?.id === "board") {
    hint = busy
      ? "Stop analysis to edit the position."
      : $("edit").checked
        ? "Type to edit · Shift + letter: blank · Space: direction · Delete: remove · Assign owner: recolor"
        : recordedGame
          ? rackChoice
            ? "Click a square to place the selected tile · Esc: recall"
            : "Type or drag tiles · Shift + letter: blank · Space: direction · Enter: play · Esc: recall"
          : "Select an analysis move to preview it, or enable Edit board to change tiles.";
  } else if (target?.id === "rack-tiles") {
    hint = recordedGame
      ? "Drag to arrange tiles or place them on the board · Alt + arrows: arrange selected tile"
      : "Drag to arrange tiles · Alt + arrows: arrange selected tile · Edit the Rack field to change tiles";
  } else if (target?.id === "results") {
    hint =
      "Select a move to preview it on the board; select it again to clear the preview.";
  }
  $("context-status").textContent = hint;
}
for (const type of ["pointerover", "focusin"]) {
  document.addEventListener(type, (event) => {
    if (event.pointerType === "touch") return;
    hintTarget = contextTarget(event.target);
    renderContextHint();
  });
}
for (const type of ["pointerout", "focusout"]) {
  document.addEventListener(type, (event) => {
    if (event.pointerType === "touch") return;
    hintTarget = contextTarget(event.relatedTarget);
    if (!hintTarget && type === "pointerout") {
      hintTarget = contextTarget(document.activeElement);
    }
    renderContextHint();
  });
}
window.addEventListener("blur", () => {
  hintTarget = null;
  renderContextHint();
});

for (const id of ["auto-analyze", "hide-spoilers", "wmp-source", "wmp-cache-enabled", "tt-memory"]) {
  try {
    const value = localStorage.getItem(`magpie-${id}`);
    if (value !== null) {
      if ($(id).type === "checkbox") $(id).checked = value === "true";
      else $(id).value = id === "wmp-source" && value === "download" ? "build" : value;
    }
  } catch {}
  $(id).addEventListener("change", () => {
    try {
      localStorage.setItem(
        `magpie-${id}`,
        $(id).type === "checkbox" ? $(id).checked : $(id).value,
      );
    } catch {}
  });
}
renderRecent();
renderPosition();
renderResults();
async function startEngine() {
  setBusy(true);
  $("stop").disabled = true;
  message("Preparing browser engine…");
  try {
    if (!(await ensureIsolation())) return;
    engine = new EngineClient();
    engine.addEventListener("message", ({ detail }) => {
      if (detail.type === "fatal") {
        positionGeneration++;
        canceled = true;
        if (isLive()) {
          chargeClock();
          session.paused = true;
          clearTimeout(botTimer);
        }
        setBusy(false);
        error(detail.text);
        message("Engine unavailable — reload to restart");
        return;
      }
      if (gcgLoading && !["output", "status", "log"].includes(detail.type)) {
        logGCG("engine.message", { type: detail.type });
      }
      if (detail.type === "wmp_progress") {
        message(detail.text);
        $("wmp-cache-status").textContent = detail.text;
        $("stop-wmp").hidden = false;
        $("stop-wmp").disabled = canceled;
      }
      if (detail.type === "wmp_ready" && !detail.unavailable && !$("wmp-cache-status").textContent.startsWith("WMP cache unavailable"))
        $("wmp-cache-status").textContent = detail.stopped ? "Stopped" : "WMP ready";
      if (detail.type === "wmp_cache_warning") $("wmp-cache-status").textContent = detail.text;
      if (detail.type === "output" || detail.type === "status")
        acceptOutput(detail.text, detail.command || "");
    });
    await engine.ready;
    setBusy(false);
    message("Engine ready");
    refreshWMPCache();
  } catch (failure) {
    error(failure.message);
    message("Engine unavailable");
    // Keep analysis disabled when initialization fails instead of offering
    // buttons whose engine promise can never resolve.
  }
}
startEngine();

function renderWMPCache() {
  const host = $("wmp-files");
  host.replaceChildren();
  for (const lexicon of ["CSW24", "NWL23"]) {
    const file = cachedWMPFiles.find((entry) => entry.lexicon === lexicon);
    const row = document.createElement("div");
    row.className = "setting-row wmp-file";
    const label = document.createElement("span");
    label.textContent = `${lexicon} · ${file ? `WMP ${(file.bytes / 1048576).toFixed(1)} MiB · WIT ${file.wit_bytes ? `${(file.wit_bytes / 1048576).toFixed(1)} MiB` : "not cached"}` : "Not cached"}`;
    if (file) label.title = `${(file.bytes + (file.wit_bytes || 0)).toLocaleString()} bytes total`;
    const button = document.createElement("button");
    button.textContent = file ? "Remove" : "Cache";
    button.disabled = busy || (!file && $("wmp-source").value === "off");
    button.setAttribute("aria-label", `${file ? "Remove" : "Cache"} ${lexicon} WMP`);
    button.addEventListener("click", async () => {
      if (busy) return;
      setBusy(true);
      renderWMPCache();
      $("wmp-cache-status").textContent = "";
      canceled = false;
      try {
        if (file) await engine.wmpCache(lexicon);
        else {
          await engine.prepare(lexicon);
          if (!canceled) {
            // Reprepare when explicitly saving an already resident, uncached map.
            engine.wmpKey = null;
            await engine.prepareWMP(lexicon, $("wmp-source").value, Number($("threads").value), true);
          }
          // File management does not need to retain a second lexicon in RAM.
          await engine.run(["set -wit false -wmp false"]);
          engine.wmpKey = null;
        }
        const status = canceled ? "Stopped" : file ? `${lexicon} WMP removed` : `${lexicon} WMP ready`;
        message(status);
        if (!$("wmp-cache-status").textContent.startsWith("WMP cache unavailable"))
          $("wmp-cache-status").textContent = status;
      } catch (failure) { $("wmp-cache-status").textContent = failure.message; }
      finally { setBusy(false); await refreshWMPCache(); }
    });
    row.append(label, button);
    host.append(row);
  }
}
async function refreshWMPCache() {
  try {
    cachedWMPFiles = await listWMPs();
    renderWMPCache();
  } catch (failure) { $("wmp-cache-status").textContent = `WMP cache unavailable: ${failure.message}`; }
}
$("wmp-source").addEventListener("change", renderWMPCache);

$("stop-wmp").addEventListener("click", stop);
