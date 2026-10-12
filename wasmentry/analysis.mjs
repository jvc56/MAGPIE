import { moveTiles } from "./move-tiles.mjs";
import { computerSearchPlan } from "./computer-time.mjs";
import { TILE_ORDERS, DEFAULT_TILE_ORDER, tileAlphabet, sortTiles, formatTileGroups } from "./tile-order.mjs";
import { GameAction } from "./game-actions.mjs";
import { installTileDrag } from "./tile-drag.mjs";
import { loadAnalyses, saveAnalysis } from "./analysis-cache.mjs";
import { deviceBudget } from "./device-budget.mjs";
import { listWMPs } from "./wmp-cache.mjs";
import { parsePegProgress, mergePegRows } from "./peg-review.mjs";
import { ensureIsolation } from "./isolation.mjs";
import { EngineClient } from "./engine-client.mjs";
import { readGCG } from "./gcg-loader.mjs?v=2";
import { logGCG, gcgLogText } from "./gcg-diagnostics.mjs";
import {
  emptyPosition,
  VALUES,
  parseCGP,
  toCGP,
  snapshotPosition,
  restorePosition,
  inventory,
  parseResults,
  formatBoardPlay,
  historyPlayParts,
  previewMove,
  analysisCommands,
  combineSim,
  analysisPositionKey,
  simulationMoveKey,
  endgameSummary,
  principalVariation,
} from "./analysis-model.mjs";
import {
  remainingRack,
  rackSlots,
  reorderRack,
  sortRack,
  shuffleRack,
  placeTile,
  moveNotation,
  normalizeMove,
} from "./move-entry.mjs";
let cachedWMPFiles = [];
let positionGeneration = 0, analysisGeneration = -1, historyClickTimer, pendingHistoryIndex = null;
const $ = (id) => document.getElementById(id);
let position = emptyPosition();
let engineInitialized = false;
let mode = "kibitz",
  results = [],
  rawAnalysis = "",
  selectedMove = "",
  busy = false,
  canceled = false;
let cursor = null,
  vertical = false,
  undo = [],
  engine,
  timer;
let tileDrag = null;
let hoveredEntry = null, hoverEntryReady = false;
let entryAnchored = false, lastRackTap = null;
let draftTiles = [],
  rackChoice = null,
  gameUndo = [];
let draftScore = null;
let previewTimer,
  previewGeneration = 0,
  previewPromise = Promise.resolve();
let recordedGame = null;
let historyIndex = 0;
let hintTarget = null;
let continuationPosition = null;
let continuationTiles = new Set();
let continuationSteps = 0;
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
let activeSolve = null;
let tileOrder = DEFAULT_TILE_ORDER;
try {
  const saved = localStorage.getItem("magpie-tile-order");
  if (Object.hasOwn(TILE_ORDERS, saved)) tileOrder = saved;
} catch { /* Storage is optional. */ }
$("tile-order").value = tileOrder;
let unseenOrder = "vowels";
try {
  const saved = localStorage.getItem("magpie-unseen-order");
  if (saved === "alphabetical" || saved === "vowels") unseenOrder = saved;
} catch {}
$("unseen-order").value = unseenOrder;
const displayTiles = tiles => sortTiles(tiles, tileOrder);
const displayMove = move => formatTileGroups(move, tileOrder).replace(/^\(exch ([A-Z?]+)\)$/i, "-$1").replace(/^ex(?:ch)? ([A-Z?]+)$/i, "-$1");
position.racks = position.racks.map(displayTiles);
function displayedRackIndex() {
  return isLive() && session.kind === "play" && session.human !== position.onTurn ? 1 : 0;
}
function canArrangeRack() {
  return position.racks[displayedRackIndex()].length > 0;
}
function sortCurrentRack(index = 0) {
  const sorted = sortRack(position.racks[index], index ? [] : draftTiles, tileOrder);
  position.racks[index] = sorted.rack;
  if (!index) draftTiles = sorted.tiles;
  rackChoice = null;
}

let cacheWrites = Promise.resolve();
const privateAnalysisKeys = new Set();
function analysisPosition() {
  const analyzed = structuredClone(position);
  if (isLive() && inventory(position).bagCount > 0) analyzed.racks[1] = "";
  return analyzed;
}
const randomSeed = () => crypto.getRandomValues(new Uint32Array(1))[0];
const searchMode = () => {
  const { bagCount } = inventory(position);
  return bagCount === 0 ? "endgame" : bagCount <= 4 ? "peg" : "sim";
};
const cacheKey = (kind = mode) =>
  `${analysisPositionKey(analysisPosition())}|${kind}|${$("plies").value}|${$("hide-spoilers").checked ? "hidden" : ""}`;
function retainAnalysis(key = cacheKey()) {
  const saved = {
    mode, results: structuredClone(results), raw: rawAnalysis,
    meta: analysisMeta, iterations: analysisIterations,
    pegProgress: structuredClone(pegProgress),
    plies: Number($("plies").value), savedAt: Date.now(),
  };
  analysisCache.set(key, saved);
  if (hiddenLiveGame()) {
    privateAnalysisKeys.add(key);
    return; // Keep live-game analysis in memory until game end.
  }
  cacheWrites = cacheWrites.then(() => saveAnalysis(key, saved)).catch(() => {
    $("analysis-storage-warning").hidden = false;
  });
}
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
    challengePoints: session?.challengePoints ?? 5,
    challengeUnit: session?.challengeUnit || "play",
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
  if (!engineInitialized) $("new-game-status").textContent = text;
}
function error(text) {
  if (/\(error 149\).*phony/i.test(text)) text = `This play contains a word outside ${position.lexicon}.`;
  $("error").hidden = !text;
  const match = text.match(/^(Play )([A-O]\d+|\d+[A-O]) ([A-Za-z().?]+)( contains at least one phony word)$/);
  if (match) {
    const gap = document.createElement("span");
    gap.className = "move-message-gap";
    gap.textContent = " ";
    $("error").replaceChildren(match[1], moveTiles(`${match[2]} ${match[3]}`, position.onTurn, position), gap, match[4].trimStart());
  }
  else $("error").textContent = text;
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
  const time = isLive()
    ? session.strategy === "kibitz" ? "Static equity"
      : session.manageTime && session.time ? "Automatic time management" : `${session.seconds}s per turn`
    : `${$("seconds").value}s`;
  $("settings-summary").textContent = !isLive() && searchMode() !== "sim"
    ? `No time limit · ${$("threads").value} threads`
    : `${time} · ${$("plies").value} plies · ${$("threads").value} threads`;
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
function openSettings(section = "appearance", opener = document.activeElement) {
  if (!$("settings-view")) return;
  settingsReturnFocus = opener;
  gameScrollY = window.scrollY;
  settingsOpen = true;
  $("game-view").hidden = true;
  $("game-header-actions").hidden = true;
  $("tile-pool").hidden = true;
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
  $("tile-pool").hidden = false;
  settingsReturnFocus?.focus({ preventScroll: true });
  window.scrollTo(0, gameScrollY);
}
$("open-settings")?.addEventListener("click", (event) => openSettings("appearance", event.currentTarget));
$("open-lexicon-settings")?.addEventListener("click", (event) =>
  openSettings("lexicon", event.currentTarget),
);
$("open-analysis-settings")?.addEventListener("click", (event) =>
  openSettings("analysis", event.currentTarget),
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
// Hold the settings lock across the gaps between automated searches and
// recording a computer move, not just while an individual command is busy.
function analysisSettingsLocked() {
  return Boolean(busy || reviewRunning || botRunning ||
    (isLive() && !session.paused && !humanTurn()));
}
function renderSettingsAvailability() {
  const locked = analysisSettingsLocked();
  document.querySelectorAll("#analysis-settings input, #analysis-settings select")
    .forEach(element => { element.disabled = locked; });
  document.querySelectorAll("#wmp-files button")
    .forEach(button => { button.disabled = locked || button.dataset.unavailable === "true"; });
  $("settings-busy").hidden = !locked;
  $("settings-busy").textContent = isLive() && session.kind === "watch" && !session.paused
    ? "Pause the game to change these settings."
    : "Stop analysis to change these settings.";
  $("lexicon").disabled = locked || Boolean(isLive());
  $("lexicon-busy").hidden = !$("lexicon").disabled;
  $("lexicon-busy").textContent = isLive()
    ? "Finish the game to change the lexicon."
    : "Stop analysis to change the lexicon.";
}
function setBusy(value) {
  value = value || Boolean(engine?.fatalError);
  if (value) tileDrag?.cancel();
  busy = value;
  validateNewGameCGP();
  $("new-game-open").disabled = value;
  if (!value) $("stop-wmp").hidden = true;
  document.body.classList.toggle("busy", value);
  document
    .querySelectorAll(
      "#game-view input, #game-view select, #game-view button, #choose-gcg, #submit-gcg, #gcg-text",
    )
    .forEach((element) => {
      element.disabled = value;
    });
  if ($("open-analysis-settings"))
    $("open-analysis-settings").disabled = false;
  $("stop").disabled = !value || Boolean(engine?.fatalError);
  $("stop").hidden = botRunning;
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
  settingsSummary();
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
function setRawAnalysis(text) {
  rawAnalysis = text;
  $("raw").textContent = concealComputerAnalysis() ? "" : text;
}
function clearResults() {
  pegProgress = null;
  continuationPosition = null;
  analysisMeta = "";
  analysisIterations = 0;
  results = [];
  selectedMove = "";
  setRawAnalysis("");
  $("elapsed").textContent = "";
  renderResults();
}
function setPosition(next, fromHistory = false) {
  tileDrag?.cancel();
  entryAnchored = false;
  lastRackTap = null;
  hoveredEntry = null;
  hoverEntryReady = false;
  positionGeneration++;
  clearTimeout(historyClickTimer);
  pendingHistoryIndex = null;
  if (!fromHistory) {
    recordedGame = null;
    session = null;
    clearTimeout(botTimer);
  }
  cancelDraftCheck();
  draftTiles = [];
  rackChoice = null;
  $("move-feedback").textContent = "";
  position = {...next, racks:next.racks.map(displayTiles)};
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
  message("Position ready");
  restoreAnalysis();
  renderSession();
}
function renderMoveScore() {
  document.querySelectorAll(".move-score").forEach(node => node.remove());
  document.querySelectorAll(".has-move-score").forEach(node => node.classList.remove("has-move-score"));
  const candidate = !draftTiles.length && results.find(row => row.move === selectedMove);
  const score = draftTiles.length ? draftScore : candidate?.score;
  if (!Number.isFinite(score) || continuationPosition) return;
  const tiles = draftTiles.length ? draftTiles : previewMove(position, selectedMove);
  if (!tiles.length) return;
  const down = draftTiles.length ? vertical : /^[A-O]/.test(selectedMove);
  const ordered = [...tiles].sort((a, b) => down ? a.row - b.row : a.col - b.col);
  const first = ordered[0];
  // Anchor before the word, including any leading playthrough. The label
  // gutter supplies a square even when the word starts at the board edge.
  let row = first.row - (down ? 1 : 0), col = first.col - (down ? 0 : 1);
  while (position.board[row]?.[col]) { row -= down ? 1 : 0; col -= down ? 0 : 1; }
  const spot = {row, col};
  const square = document.querySelector(`#board [data-row="${spot.row}"][data-col="${spot.col}"]`);
  if (!square) return;
  square.classList.add("has-move-score");
  const badge = document.createElement("span");
  badge.className = `move-score${String(score).length >= 3 ? " move-score-wide" : ""}`;
  badge.textContent = score;
  badge.setAttribute("aria-label", `${score} points`);
  square.append(badge);
}
function offBoardCursor() {
  if (cursor || !draftTiles.length) return null;
  const last = draftTiles.at(-1);
  let row = last.row + (vertical ? 1 : 0), col = last.col + (vertical ? 0 : 1);
  while (row < 15 && col < 15 && position.board[row][col]) {
    row += vertical ? 1 : 0;
    col += vertical ? 0 : 1;
  }
  return row === 15 || col === 15 ? {row, col} : null;
}
function renderBoard() {
  tileDrag?.cancel();
  const drawPosition = continuationPosition || position;
  const board = $("board");
  board.classList.toggle("position-editing", $("edit").checked);
  board.dataset.direction = vertical ? "down" : "right";
  board.classList.toggle("entry-anchored", entryAnchored);
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
        label.dataset.row = row;
        label.dataset.col = col;
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
      if (continuationPosition && continuationTiles.has(`${row},${col}`))
        cell.classList.add("pv-preview");
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
  const edge = offBoardCursor();
  if (edge) {
    const marker = document.createElement("span");
    marker.className = `edge-cursor${vertical ? " down" : ""}`;
    marker.tabIndex = 0;
    marker.setAttribute("aria-label", `End of board, typing ${vertical ? "down" : "right"}`);
    marker.dataset.row = edge.row;
    marker.dataset.col = edge.col;
    board.querySelector(`[data-row="${Math.min(edge.row,14)}"][data-col="${Math.min(edge.col,14)}"]`).append(marker);
  }
  renderMoveScore();
  renderContextHint();
}
function terminalPosition() {
  return (
    recordedGame?.ended && historyIndex === recordedGame.positions.length - 1
  );
}
let panelFitFrame;
let liveBoardFit = null;
function fitGamePanels() {
  cancelAnimationFrame(panelFitFrame);
  panelFitFrame = requestAnimationFrame(() => {
    // Validation feedback may arrive mid-gesture. Keep drop targets fixed
    // until the pointer is released or the drag is canceled.
    if (tileDrag?.active) return;
    const panel = document.querySelector(".position"), analysis = document.querySelector(".analysis");
    const narrow = innerWidth <= 1000;
    const analysisHeight = analysis.getBoundingClientRect().height;
    const viewportHeight = innerHeight - document.querySelector(".app-header").getBoundingClientRect().height - 60;
    const height = narrow || !analysisHeight ? viewportHeight : analysisHeight;
    const style = getComputedStyle(panel);
    let occupied = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom) + 2;
    for (const child of panel.children) {
      if (["board-viewport", "unseen-home"].includes(child.id) || !child.getBoundingClientRect().height) continue;
      const childStyle = getComputedStyle(child);
      // Expanded editing fields can scroll below the playing area without shrinking the board.
      const childHeight = child.id === "position-editor"
        ? child.querySelector("summary").getBoundingClientRect().height
        : child.getBoundingClientRect().height;
      occupied += childHeight + parseFloat(childStyle.marginTop) + parseFloat(childStyle.marginBottom);
    }
    if (narrow || !analysisHeight) panel.style.removeProperty("--analysis-panel-height");
    else panel.style.setProperty("--analysis-panel-height", `${height}px`);
    let boardWidth = Math.max(260, height - occupied);
    if (isLive()) {
      const layout = `${innerWidth}:${innerHeight}:${document.body.classList.contains("show-analysis")}`;
      if (liveBoardFit?.session === session && liveBoardFit.layout === layout) {
        // Turn transitions briefly remove controls or status text. Keep their space
        // reserved instead of growing the board for those intermediate renders.
        boardWidth = Math.min(boardWidth, liveBoardFit.width);
      }
      liveBoardFit = {session, layout, width: boardWidth};
    } else liveBoardFit = null;
    panel.style.setProperty("--fitted-board-width", `${boardWidth}px`);
    const availableWidth = panel.clientWidth - parseFloat(style.paddingLeft) - parseFloat(style.paddingRight);
    panel.style.setProperty("--zoom-base-width", `${Math.min(availableWidth, boardWidth)}px`);
  });
}
const panelResize = new ResizeObserver(fitGamePanels);
panelResize.observe(document.querySelector(".analysis"));
panelResize.observe(document.querySelector(".app-header"));
for (const child of document.querySelector(".position").children) {
  if (child.id !== "board-viewport") panelResize.observe(child);
}
window.addEventListener("resize", fitGamePanels);
const mobileBoard = matchMedia("(pointer: coarse)");
function setBoardZoom(zoomed) {
  $("board-viewport").classList.toggle("zoomed", zoomed);
  $("board-zoom").setAttribute("aria-pressed", String(zoomed));
  $("board-zoom").textContent = zoomed ? "Fit board" : "Zoom board";
  if (zoomed) {
    requestAnimationFrame(() => focusCursor());
  } else {
    $("board-viewport").scrollTo(0, 0);
  }
}
$("board-zoom").addEventListener("click", () => {
  if (mobileBoard.matches) setBoardZoom(!$("board-viewport").classList.contains("zoomed"));
});
mobileBoard.addEventListener("change", () => {
  if (!mobileBoard.matches) setBoardZoom(false);
});
document.addEventListener("pointerdown", event => {
  if (!event.target.closest("#rack-tiles")) lastRackTap = null;
});
$("game-overflow").addEventListener("click", () => {
  const button = $("game-overflow");
  button.setAttribute("aria-expanded", String(button.getAttribute("aria-expanded") !== "true"));
});
$("game-file-actions").addEventListener("click", (event) => {
  if (event.target.closest("button")) $("game-overflow").setAttribute("aria-expanded", "false");
});
document.addEventListener("click", (event) => {
  if (!event.target.closest("#game-actions")) $("game-overflow").setAttribute("aria-expanded", "false");
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape" && $("game-overflow").getAttribute("aria-expanded") === "true") {
    $("game-overflow").setAttribute("aria-expanded", "false");
    $("game-overflow").focus();
  }
});
const wideLayout = matchMedia("(min-width: 1360px)");
function placeUnseenPool() {
  $(wideLayout.matches ? "brand-rail" : "unseen-home").append(
    $("tile-pool"),
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
  // Actual rack/position changes invalidate a drag; preview updates do not.
  tileDrag?.cancel();
  $("rack").value = hiddenLiveGame() && !humanTurn() ? "" : position.racks[0];
  $("opponent-rack").value = hiddenLiveGame() ? "" : position.racks[1];
  $("score").value = position.scores[0];
  $("opponent-score").value = position.scores[1];
  $("lexicon").value = position.lexicon;
  if ($("current-lexicon"))
    $("current-lexicon").textContent = position.lexicon;
  $("on-turn-player").value = position.onTurn;
  $("ownership-tools").hidden = !$("edit").checked;
  $("rack-tiles").dataset.owner = position.onTurn;
  const rackIndex = displayedRackIndex();
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
    tally.textContent = `${count} ${count === 1 ? kind.slice(0, -1) : kind}`;
    $("unseen-counts").append(tally, " ");
  }
  $("unseen-tiles").replaceChildren();
  for (const letter of tileAlphabet(unseenOrder)) {
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
    tile.disabled = !canArrangeRack();
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
function updateSimSelection() {
  $("continue-selected").hidden = mode !== "sim" || !selectedMove;
  $("continue-selected").textContent = "Continue selected play";
  $("continue-selected").disabled = busy;
}
function concealComputerAnalysis() {
  return hiddenLiveGame() && position.onTurn !== session.human && !$("show-live-analysis").checked;
}
function renderResults() {
  if (concealComputerAnalysis()) {
    $("results").tBodies[0].replaceChildren();
    $("raw").textContent = "";
    $("analysis-meta").textContent = "";
    $("selection").replaceChildren();
    renderPegProgress();
    return;
  }
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
  updateSimSelection();
  $("analyze-move").hidden = !recordedGame || !!isLive();
  $("analyze-move").disabled = busy || (!draftTiles.length && !selectedMove);
  const header = document.createElement("tr");
  for (const [key, label] of columns) {
    const th = document.createElement("th");
    th.dataset.column = key;
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
      td.dataset.column = key;
      if (key === "move") {
        const button = document.createElement("button");
        button.append(moveTiles(displayMove(row.move), position.onTurn, position));
        button.type = "button";
        button.setAttribute(
          "aria-pressed",
          String(row.move === selectedMove),
        );
        if (mode === "endgame") {
          button.classList.add("endgame-expand");
          button.setAttribute("aria-expanded", String(row.move === selectedMove));
          button.setAttribute("aria-controls", `endgame-pv-${row.rank}`);
        }
        button.addEventListener("click", () => selectMove(row.move));
        td.append(button);
      } else if (key === "leave") td.textContent = displayTiles(row.leave || "");
      else if (key === "outcomes")
        td.textContent = Number.isFinite(row.wins)
          ? `${row.wins} / ${row.ties} / ${row.losses}`
          : "—";
      else if (key === "outcome")
        td.textContent =
          row.spread === 0 ? "Tie" : `${row.spread > 0 ? "Win" : "Loss"} by ${Math.abs(row.spread)}`;
      else if (key.startsWith("ply")) {
        const index = +key.slice(3);
        td.textContent = format(row.plyAverages?.[index], 1);
        td.classList.add(
          (index + position.onTurn) % 2 ? "ply-odd" : "ply-even",
        );
      } else if (mode === "sim" && (key === "win" || key === "equity")) {
        td.textContent = format(row[key], 1);
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
            Math.max(...results.map((value) => Number.isFinite(value[key]) ? value[key] : -Infinity)),
        );
      if (["spread", "outcome"].includes(key))
        td.classList.add(
          row.spread > 0 ? "winning" : row.spread < 0 ? "losing" : "tie",
        );
      tr.append(td);
    }
    body.append(tr);
    if (mode === "endgame" && row.move === selectedMove) {
      const details = document.createElement("tr");
      details.className = "endgame-details";
      details.id = `endgame-pv-${row.rank}`;
      const cell = document.createElement("td");
      cell.colSpan = columns.length;
      const title = document.createElement("strong");
      title.textContent = "Principal variation";
      const line = document.createElement("div");
      line.className = "pv-line";
      const steps = principalVariation(row);
      steps.forEach((step, index) => {
        const player = (position.onTurn + index) % 2;
        const button = document.createElement("button");
        button.type = "button";
        button.className = `pv-player-${player}`;
        button.dataset.pvStep = index + 1;
        button.setAttribute("aria-pressed", String(continuationSteps === index + 1));
        button.append(`${index + 1}. `, moveTiles(displayMove(step.move), player), step.score === null ? "" : ` · ${step.score}`);
        button.setAttribute("aria-label", `${recordedGame?.players[player] || `Player ${player + 1}`}: ${displayMove(step.move)}${step.score === null ? "" : `, ${step.score} points`}`);
        button.dataset.contextHint = "Preview the principal variation through this move.";
        button.addEventListener("click", () => previewContinuation(steps.slice(0, index + 1).map(step => step.move)));
        line.append(button);
      });
      cell.append(title, line);
      if (recordedGame && !terminalPosition()) {
        const play = document.createElement("button");
        play.type = "button";
        play.textContent = "Play selected move";
        play.disabled = busy || (isLive() && (!humanTurn() || session.paused));
        play.addEventListener("click", () => submitMove(row.move));
        cell.append(play);
      }
      if (row.continuation?.includes("Search in progress")) {
        const pending = document.createElement("p");
        pending.textContent = "Searching for the continuation…";
        cell.append(pending);
      }
      details.append(cell);
      body.append(details);
    }
  });
  $("results").tBodies[0].replaceChildren(body);
  $("empty").hidden = results.length > 0;
  $("result-note").textContent = note[mode];
  const row = results.find((r) => r.move === selectedMove);
  $("selection").hidden = !row || mode === "endgame";
  $("selection").replaceChildren();
  if (row) {
    const title = document.createElement("strong");
    title.append(moveTiles(displayMove(row.move), position.onTurn, position));
    $("selection").append(
      title,
      ` · ${row.leave ? `Leave ${displayTiles(row.leave)}` : "Board preview"}`,
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
    if (row.continuation && mode !== "endgame") {
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
        button.append(`${index + 1}. `, moveTiles(displayMove(move), (position.onTurn + index) % 2));
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
  panel.hidden = concealComputerAnalysis() || mode !== "peg" || !pegProgress?.stages.length;
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
  if (mode === "endgame" && selectedMove) {
    const row = results.find(row => row.move === selectedMove);
    if (row) previewContinuation(principalVariation(row).map(step => step.move));
  }
  renderResults();
  renderBoard();
}
function acceptOutput(text, command) {
  if (analysisGeneration !== positionGeneration) return;
  // Preparatory set/CGP/generate output must not masquerade as sim results.
  const relevant =
    mode === "kibitz"
      ? /^(generate|shmoves)/.test(command)
      : mode === "sim"
        ? /^(sim|snoprune)(?: |$)/.test(command)
        : command.startsWith(mode);
  if (!relevant) return;
  setRawAnalysis(text);
  let next = parseResults(text, mode);
  if (mode === "kibitz") {
    const manual = analysisCache.get(cacheKey("kibitz"))?.results.filter(row => row.manual) || [];
    const unique = new Map([...manual, ...next].map(row => [moveIdentity(row.move), row]));
    for (const row of manual) unique.get(moveIdentity(row.move)).manual = true;
    next = [...unique.values()].sort((a, b) => b.equity - a.equity).map((row, index) => ({...row, rank:index + 1}));
  }
  if (mode === "peg") {
    pegProgress = parsePegProgress(text);
    next = mergePegRows(results, next, pegProgress);
    renderPegProgress();
  }
  if (next.length) {
    results =
      analysisBase && mode === "sim" ? combineSim(analysisBase, next, position) : next;
    if (mode === "sim") {
      const manual = new Set(["sim", "kibitz"].flatMap(kind => analysisCache.get(cacheKey(kind))?.results
        .filter(row => row.manual).map(row => moveIdentity(row.move)) || []));
      results = results.map(row => ({...row, manual:manual.has(moveIdentity(row.move))}));
    }
    if (mode === "sim") {
      const batchIterations = [...text.matchAll(/Iters:\s+(\d+)/g)].at(-1)?.[1];
      analysisIterations = analysisBaseIterations + (batchIterations !== undefined
        ? Number(batchIterations)
        : next.reduce((total, row) => total + (row.iterations || 0), 0));
    }
    analysisMeta = analysisSummary(text, mode);
    // Search statistics do not change the board or the selected preview.
    // Keep an in-progress rack drag intact while the bot thinks.
    renderResults();
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
    if (options.seconds !== undefined) settings.seconds = options.seconds;
    settings.useFullBudget = !!options.useFullBudget;
    settings.solveToCompletion = !isLive() && ["peg", "endgame"].includes(nextMode);
    const analyzed = analysisPosition();
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
    activeAnalysisKey = cacheKey(nextMode);
    const resuming = options.resume ?? nextMode === "sim";
    analysisBase = resuming
      ? analysisCache.get(activeAnalysisKey)?.results
      : null;
    analysisBaseIterations = resuming
      ? analysisCache.get(activeAnalysisKey)?.iterations || 0
      : 0;
    if (nextMode === "sim" && options.moves) settings.moves = options.moves.map(normalizeMove);
    const manual = ["kibitz", "sim"].flatMap(kind =>
      analysisCache.get(cacheKey(kind))?.results.filter(row => row.manual) || []);
    settings.protectedMoves = manual.map(row => normalizeMove(row.move));
    let commands = analysisCommands(analyzed, nextMode, settings);
    const extra = new Map([...manual, ...(nextMode === "sim" ? analysisBase || [] : [])]
      .map(row => [moveIdentity(row.move), normalizeMove(row.move)]));
    if (extra.size && ["kibitz", "sim"].includes(nextMode) && !settings.moves) {
      const generate = commands.indexOf("generate");
      if (generate >= 0) commands.splice(generate + 1, 0,
        `addmoves ${[...extra.values()].join(",")}`,
        ...(nextMode === "kibitz" ? ["shmoves"] : []));
      commands[0] = commands[0].replace(/-maxnumdplays \d+/, `-maxnumdplays ${100 + extra.size}`);
    }
    if (nextMode === "sim" && manual.length && !settings.moves) {
      const protectedMoves = [...new Set([...manual.map(row => normalizeMove(row.move)),
        ...(settings.playedMove ? [normalizeMove(settings.playedMove)] : [])])];
      commands[commands.length - 1] = `snoprune ${analyzed.racks[1] || "-"},${protectedMoves.join(",")}`;
    }
    commands[0] += ` -seed ${randomSeed()}`;
    const keepSelection = mode === nextMode && analysisBase?.some(row => row.move === selectedMove);
    const previewChanged = (!keepSelection && !!selectedMove) || !!continuationPosition;
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
      setRawAnalysis(saved.raw);
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
    if (previewChanged) renderBoard();
    message(`Loading ${position.lexicon}…`);
    await previewPromise;
    await engine.prepare(position.lexicon);
    if (!canceled) await engine.prepareWMP(position.lexicon, settings.wmpSource, settings.threads, $("wmp-cache-enabled").checked);
    if (generation !== positionGeneration) return false;
    if (canceled) {
      message("Stopped");
      return;
    }
    if (options.computerTime) {
      // Downloads/builds and watch delays count against the clock too.
      chargeClock();
      const plan = currentComputerSearchPlan(mode);
      settings.seconds = plan.seconds;
      settings.useFullBudget = plan.useFullBudget;
      if (plan.mode !== mode) {
        mode = plan.mode;
        activeAnalysisKey = cacheKey(mode);
        analysisBase = null;
        analysisBaseIterations = 0;
        clearResults();
        document.querySelectorAll("[data-mode]").forEach(button =>
          button.classList.toggle("active", button.dataset.mode === mode));
      }
      commands = analysisCommands(analyzed, mode, settings);
      commands[0] += ` -seed ${randomSeed()}`;
    }
    message(
      `${{ kibitz: "Generating moves", sim: "Simulating", peg: "Solving preendgame", endgame: "Solving endgame" }[mode]}…`,
    );
    const started = performance.now();
    if (settings.solveToCompletion) {
      activeSolve = {paused:false, pausedAt:0, pausedDuration:0};
    }
    timer = setInterval(() => {
      $("elapsed").textContent =
        `${(((activeSolve?.paused ? activeSolve.pausedAt : performance.now()) - started - (activeSolve?.pausedDuration || 0)) / 1000).toFixed(1)}s`;
    }, 100);
    const response = await engine.run(commands);
    if (generation !== positionGeneration) return false;
    message(response.stopped ? "Stopped · partial results" : "Complete");
    succeeded = !response.stopped && results.length > 0;
    if (results.length) retainAnalysis(activeAnalysisKey);
  } catch (e) {
    if (generation !== positionGeneration) return false;
    error(e.message);
    message("Analysis could not run");
  } finally {
    clearInterval(timer);
    activeSolve = null;
    $("pause-solve").hidden = true;
    $("pause-solve").textContent = "Pause";
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
  $("pause-solve").disabled = true;
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
$("undo").addEventListener("click", () => {
  if (undo.length) {
    position = restorePosition(undo.pop());
    error("");
    clearResults();
    renderPosition();
  }
});
$("clear").addEventListener("click", () =>
  commitEdit(emptyPosition(position.lexicon)),
);
$("edit").addEventListener("change", () => {
  recallTiles(false);
  selectedMove = "";
  renderResults();
  renderPosition();
});
$("tile-order").addEventListener("change", () => {
  tileOrder = $("tile-order").value;
  try { localStorage.setItem("magpie-tile-order", tileOrder); } catch {}
  sortCurrentRack();
  position.racks[1] = displayTiles(position.racks[1]);
  renderPosition();
  renderResults();
  renderHistoryOptions();
  renderHistory();
});
$("unseen-order").addEventListener("change", () => {
  unseenOrder = $("unseen-order").value;
  try { localStorage.setItem("magpie-unseen-order", unseenOrder); } catch {}
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
  hoverEntryReady = false;
  if (busy) return;
  if (!$("edit").checked) {
    if (recordedGame) entryClick(event);
    return;
  }
  const cell = event.target.closest(".square[data-row]");
  if (!cell) return;
  const next = { row: +cell.dataset.row, col: +cell.dataset.col };
  if (cursor?.row === next.row && cursor?.col === next.col)
    vertical = !vertical;
  cursor = next;
  renderBoard();
  focusCursor();
});
function focusCursor() {
  const target = cursor
    ? $("board").querySelector(`[data-row="${cursor.row}"][data-col="${cursor.col}"]`)
    : $("board").querySelector(".edge-cursor") || $("board").querySelector('[tabindex="0"]');
  const viewport = $("board-viewport");
  if (!target) return;
  if (!viewport.classList.contains("zoomed")) {
    target.focus();
    return;
  }
  target.focus({preventScroll:true});
  // Pan only the enlarged board, without scrolling the rack out of reach.
  const bounds = viewport.getBoundingClientRect(), square = target.getBoundingClientRect();
  viewport.scrollLeft += Math.min(0, square.left - bounds.left) + Math.max(0, square.right - bounds.right);
  viewport.scrollTop += Math.min(0, square.top - bounds.top) + Math.max(0, square.bottom - bounds.bottom);
}

$("board").addEventListener("keydown", (event) => {
  if (!$("edit").checked && recordedGame) {
    if (!hoverEntryKey(event)) entryKey(event);
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
    setPosition(parseCGP($("cgp").value, position.lexicon));
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
function challengeTarget() {
  if (!recordedGame || !historyIndex) return "";
  if (session && (!isLive() || session.challenge === "void" || !humanTurn() || session.paused)) return "";
  const line = recordedGame.gcg.split(/\r?\n/)
    .filter(line => line.startsWith(">"))[historyIndex - 1];
  if (!line) return "";
  const {move} = historyPlayParts(line, restorePosition(recordedGame.positions[historyIndex - 1]));
  return /^(?:[A-O]\d+|\d+[A-O]) /.test(move) ? displayMove(move) : "";
}
function renderHistory() {
  const target = challengeTarget();
  $("challenge-move").hidden = !target;
  $("challenge-move").disabled = busy || !target;
  $("challenge-move").replaceChildren("Challenge");
  if (target) $("challenge-move").append(" ", moveTiles(target, 1 - position.onTurn, restorePosition(recordedGame.positions[historyIndex - 1])));
  $("game-history").hidden = !recordedGame;
  $("copy").disabled = busy || hiddenLiveGame();
  $("save-gcg").disabled = !canExportGame();
  $("copy-gcg").disabled = !canExportGame() || hiddenLiveGame();
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
    true,
  );
  if (auto && !concealComputerAnalysis() && $("auto-analyze").value !== "off" && !reviewRunning)
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
$("new-game-open").addEventListener("click", () => {
  $("new-game-dialog").close();
  openGameImport();
});
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
  const previousSession = session;
  const resumeSession = isLive() && !session.paused;
  if (resumeSession) {
    chargeClock();
    session.paused = true;
    clearTimeout(botTimer);
    renderSession();
  }
  if (isLive() && !(await confirmGame("Open another game?",
      "The current game will end and be replaced.", "Open game"))) {
    if (resumeSession && session === previousSession) {
      session.paused = false;
      session.started = performance.now();
      renderSession();
      scheduleComputer();
    }
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
    for (const saved of game.positions) parseCGP(saved.cgp, lexicon);
    restoreFinishedSession(game, text);
    game.filename = (file?.name || "pasted-game.gcg").replace(/\.gcg$/i, "") + ".gcg";
    gameUndo = [];
    session = null;
    clearTimeout(botTimer);
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
    setBusy(false);
    if (resumeSession && session === previousSession) {
      session.paused = false;
      session.started = performance.now();
      renderSession();
      scheduleComputer();
    }
    clearInterval(watchdog);
    $("gcg-load-status").textContent = "";
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
$("pause-solve").addEventListener("click", () => {
  if (!activeSolve || canceled) return;
  $("pause-solve").disabled = true;
  engine.setSearchPaused(!activeSolve.paused);
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
  if (event.key === "Escape" && busy && !botRunning && !$("stop").disabled) {
    event.preventDefault();
    stop();
    return;
  }
  hoverEntryKey(event);
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
function privateHistoryLine(line) {
  if (!hiddenLiveGame()) return line;
  const nick = recordedGame.gcg.match(new RegExp(`^#player${2 - session.human}\\s+(\\S+)`, "m"))?.[1];
  if (!line.startsWith(`>${nick}:`)) return line;
  return line.replace(/(\s)-([A-Z?]+)(?=\s)/, (_, space, tiles) => `${space}-${tiles.length}`);
}
function historyRackLabel(player, rack, index, bingo = false) {
  const hideUpcoming = $("hide-spoilers").checked;
  const publicRack = bingo && (!hideUpcoming || index < historyIndex);
  if ((hiddenLiveGame() && player !== session.human && !publicRack) ||
      (hideUpcoming && index > historyIndex)) return "Rack hidden";
  return rack ? displayTiles(rack) : "Rack unknown";
}
function renderHistoryOptions() {
  if (!recordedGame) return;
  const entries = historyEntries();
  $("history-position").replaceChildren(
    ...recordedGame.positions.map((saved, index) => {
      const next = restorePosition(saved);
      const last = index === recordedGame.positions.length - 1;
      const finished = last && (recordedGame.ended || session?.finished) &&
        !($("hide-spoilers").checked && index > historyIndex);
      const timeoutTurn = entries[index]?.parts.move === "(time)" && !entries[index - 1]?.parts.adjustment;
      const turn = timeoutTurn ? (entries[index - 1]?.turn ?? 0) + 1 : entries[index]?.turn ?? (entries.at(-1)?.turn ?? 0) + 1;
      const rack = historyRackLabel(next.onTurn, next.racks[0], index, entries[index]?.bingo);
      const prefix = entries[index]?.parts.challenge ? `Challenge result · ${turn}.` : entries[index]?.parts.adjustment && !timeoutTurn ? "Final scoring" : `${turn}.`;
      const label = finished ? "Game over" : `${prefix} ${recordedGame.players[next.onTurn]} · ${rack}`;
      return new Option(label, String(index));
    }),
  );
  $("history-position").value = String(historyIndex);
}
function adoptGame(game, lexicon, index = game.index || 0) {
  for (const saved of game.positions) {
    if (!saved.cgp.includes(" -lex ")) saved.cgp += ` -lex ${lexicon}`;
    restorePosition(saved);
  }
  recordedGame = game;
  historyIndex = Math.min(index, game.positions.length - 1);
  $("game-players").textContent = game.players.join(" vs ");
  renderHistoryOptions();
  setPosition(restorePosition(game.positions[historyIndex]), true);
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
function clearFinishedGameResult(game) {
  // A postmortem branch is a new record, not the finished timed session.
  // Keep the original result only in the undo snapshot, including on export.
  session = null;
  game.gcg = game.gcg.replace(/^#description Magpie finished: [^\n]*\n?/gm, "");
}
let resolveGameConfirmation = null;
function finishGameConfirmation(confirmed) {
  const resolve = resolveGameConfirmation;
  resolveGameConfirmation = null;
  $("game-confirm").close();
  resolve?.(confirmed);
}
function confirmGame(title, text, label = "Continue") {
  if (resolveGameConfirmation) return Promise.resolve(false);
  $("confirm-title").textContent = title;
  $("confirm-message").textContent = text;
  $("confirm-message").hidden = !text;
  $("confirm-yes").textContent = label;
  return new Promise((resolve) => {
    resolveGameConfirmation = resolve;
    $("game-confirm").showModal();
  });
}
$("confirm-yes").addEventListener("click", () => finishGameConfirmation(true));
$("confirm-no").addEventListener("click", () => finishGameConfirmation(false));
$("game-confirm").addEventListener("cancel", (event) => {
  event.preventDefault();
  finishGameConfirmation(false);
});
function recallTiles(render = true) {
  cancelDraftCheck();
  draftTiles = [];
  rackChoice = null;
  error("");
  $("move-feedback").textContent = "";
  if (render) renderPosition();
}
function cancelDraftCheck() {
  clearTimeout(previewTimer);
  previewGeneration++;
  draftScore = null;
  renderMoveScore();
}
function moveErrorMessage(text) {
  const formatted = text.replace(/(\bplay:\s*)([A-O]\d+ [A-Za-z.]+|\d+[A-O] [A-Za-z.]+)/g,
    (_, prefix, play) => prefix + formatBoardPlay(play, position));
  const phony = formatted.trim().match(/expected valid play but detected at least one phony for play:\s*(.+)$/i);
  return phony ? `Play ${phony[1]} contains at least one phony word` : formatted;
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
        action: GameAction.COMMIT,
      })
      .then(({ game }) => {
        if (generation !== previewGeneration || busy) return;
        draftScore = game.score;
        $("move-feedback").textContent = "";
        renderMoveScore();
      })
      .catch((failure) => {
        if (generation === previewGeneration && !busy)
          $("move-feedback").textContent = moveErrorMessage(failure.message);
      });
  }, 220);
}
function renderDraft() {
  continuationPosition = null;
  selectedMove = "";
  error("");
  try {
    moveNotation(position, draftTiles, vertical);
    $("move-feedback").textContent = "";
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
  const cell = event.target.closest(".square[data-row]");
  if (!cell) return;
  const row = +cell.dataset.row,
    col = +cell.dataset.col;
  entryAnchored = true;
  lastRackTap = null;
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
$("board").addEventListener("pointermove", (event) => {
  if (entryAnchored || event.pointerType !== "mouse" || event.buttons) return;
  const square = event.target.closest(".square");
  if (!square) return;
  const next = {row:+square.dataset.row, col:+square.dataset.col};
  if (hoveredEntry?.row !== next.row || hoveredEntry?.col !== next.col) {
    hoveredEntry = next;
    hoverEntryReady = true;
  }
});
$("board").addEventListener("pointerleave", () => {
  hoveredEntry = null;
  hoverEntryReady = false;
});
function hoverEntryKey(event) {
  if (entryAnchored || !hoverEntryReady || !hoveredEntry || !recordedGame || !canDragTiles() ||
      settingsOpen || $("edit").checked || document.querySelector("dialog[open]") ||
      document.body.classList.contains("tile-dragging") || event.isComposing ||
      event.ctrlKey || event.metaKey || event.altKey || !/^[a-zA-Z]$/.test(event.key) ||
      event.target.closest("input, textarea, select, [contenteditable]")) return false;
  const {row, col} = hoveredEntry;
  if (position.board[row][col] || draftTiles.some(tile => tile.row === row && tile.col === col)) return false;
  if (!hasTypedTile(event)) {
    event.preventDefault();
    return true;
  }
  // Use the hovered square only to begin typing. Subsequent letters follow
  // the advancing cursor until the mouse moves to a different square.
  hoverEntryReady = false;
  cursor = {row, col};
  entryKey(event);
  return true;
}
function hasTypedTile(event) {
  const rack = remainingRack(position.racks[0], draftTiles);
  return rack.includes("?") || (!event.shiftKey && rack.includes(event.key.toUpperCase()));
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
    if (event.key === "Delete") {
      const last = draftTiles.pop();
      if (last) cursor = {row:last.row, col:last.col};
    } else {
      const last = draftTiles.at(-1);
      let previous = cursor
        ? {row:cursor.row - (vertical ? 1 : 0), col:cursor.col - (vertical ? 0 : 1)}
        : last && {row:last.row, col:last.col};
      while (previous && previous.row >= 0 && previous.col >= 0 && position.board[previous.row][previous.col]) {
        previous.row -= vertical ? 1 : 0;
        previous.col -= vertical ? 0 : 1;
      }
      if (previous && previous.row >= 0 && previous.col >= 0) {
        cursor = previous;
        draftTiles = draftTiles.filter(tile => tile.row !== cursor.row || tile.col !== cursor.col);
      }
    }
    renderDraft();
    focusCursor();
    return;
  }
  if (event.key === " ") {
    event.preventDefault();
    vertical = !vertical;
    let reflowError = "";
    // Like native entry, toggling direction reflows uncommitted tiles from
    // their first square, skipping fixed board tiles.
    if (draftTiles.length) {
      const previous = [...draftTiles];
      const previousCursor = cursor && {...cursor};
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
        cursor = previousCursor;
        reflowError = failure.message;
      }
    }
    renderDraft();
    if (reflowError) error(reflowError);
    focusCursor();
    return;
  }
  if (arrows[event.key]) {
    event.preventDefault();
    cursor ||= offBoardCursor() || { row: 7, col: 7 };
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
    if (!hasTypedTile(event) || offBoardCursor()) return;
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
    const pendingMove = value || (draftTiles.length ? moveNotation(position, draftTiles, vertical) : selectedMove);
    if (!pendingMove) throw new Error("Place tiles on the board first.");
    const move = normalizeMove(pendingMove);
    if (move === "pass" && !computer && !(await confirmGame(
      "Are you sure you wish to pass?",
      "",
      "Pass",
    ))) return;
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
        ? liveAction(GameAction.LIVE_MOVE, move)
        : {
            text: recordedGame.gcg,
            lexicon: position.lexicon,
            index: historyIndex,
            rack: position.racks[0],
            move,
            action: GameAction.COMMIT,
            cgp: physicalCGP(),
            onTurn: position.onTurn,
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
    } else clearFinishedGameResult(game);
    adoptGame(game, position.lexicon);
    if (isLive()) {
      session.started = performance.now();
      if (game.ended) finishSession("Game over");
    }
    message("Move recorded");
  } catch (failure) {
    if (generation !== positionGeneration) return;
    error(moveErrorMessage(failure.message));
    message("Move could not be recorded");
  } finally {
    setBusy(false);
    if ($("status").textContent === "Move recorded" && !session)
      $("rack").focus();
    renderSession();
    // Disabling the board during validation can blur it. A rejected or
    // canceled submission leaves a draft that should remain keyboard editable.
    if (!computer && generation === positionGeneration && draftTiles.length &&
        !busy && !settingsOpen && !document.querySelector("dialog[open]")) {
      focusCursor();
    }
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
  if (!canArrangeRack()) return;
  const index = displayedRackIndex();
  const shuffled = shuffleRack(position.racks[index], index ? [] : draftTiles);
  position.racks[index] = shuffled.rack;
  if (!index) draftTiles = shuffled.tiles;
  rackChoice = null;
  renderPosition();
});
$("sort-rack").addEventListener("click", () => {
  if (!canArrangeRack()) return;
  sortCurrentRack(displayedRackIndex());
  renderPosition();
});
$("new-game").addEventListener("click", () => {
  restoreGameTime();
  if (isLive()) {
    chargeClock();
    session.paused = true;
    clearTimeout(botTimer);
    if (busy) stop();
    renderPosition();
  }
  $("new-game-cgp").value = "";
  $("new-game-error").textContent = "";
  validateNewGameCGP();
  $("new-game-dialog").showModal();
});
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
  const { valid, startingPosition } = validateNewGameCGP();
  if (!valid) return;
  $("new-game-error").textContent = "";
  const lexicon = startingPosition?.lexicon || position.lexicon;
  const previousSession = session;
  const resumeSession = isLive() && !session.paused;
  if (resumeSession) {
    chargeClock();
    session.paused = true;
    clearTimeout(botTimer);
    renderSession();
  }
  if (isLive() && !(await confirmGame("Start a new game?", "The current game will end and be replaced.", "New game"))) {
    if (resumeSession && session === previousSession) {
      session.paused = false;
      session.started = performance.now();
      renderSession();
      scheduleComputer();
    }
    return;
  }
  if (busy) return;
  $("new-game-dialog").close();
  let focusRack = false;
  try {
    setBusy(true);
    $("stop").disabled = true;
    error("");
    message("Starting game…");
    await previewPromise;
    await engine.prepare(lexicon);
    const kind = $("game-kind").value;
    const first =
      kind === "record" || startingPosition
        ? 0
        : $("first-player").value === "random"
          ? randomSeed() % 2
          : +$("first-player").value;
    if (first) players.reverse();
    const text = `#character-encoding UTF-8\n#lexicon ${lexicon}\n#player1 p1 ${players[0]}\n#player2 p2 ${players[1]}\n`;
    const gameText = text + (startingPosition ? `#cgp ${toCGP(startingPosition)}\n` : "");
    let { game } = await engine.importGCG(gameText, lexicon);
    if (game.ended) throw new Error("This position has already ended.");
    let nextSession = null;
    if (kind !== "record") {
      const response = await engine.gameAction({
        text: gameText,
        lexicon,
        index: 0,
        action: GameAction.DEAL,
        seed: randomSeed(),
        onTurn: 0,
      });
      game = response.game;
      nextSession = sessionOptions(kind);
      if (first) nextSession.human = 1 - nextSession.human;
      game.positions[0].clocks = [...nextSession.remaining];
    }
    saveGameUndo();
    session = nextSession;
    if (nextSession) {
      try { localStorage.setItem("magpie-game-time", $("game-time").value); } catch {}
    }
    game.filename = "magpie-game.gcg";
    adoptGame(game, lexicon, 0);
    message(session || startingPosition?.racks[0] ? "Game ready" : "Game ready · enter the first rack");
    if (!session && !startingPosition?.racks[0]) {
      $("position-editor").open = true;
      focusRack = true;
    }
  } catch (failure) {
    $("new-game-error").textContent = failure.message;
    message("Game could not be started");
    $("new-game-dialog").showModal();
  } finally {
    setBusy(false);
    renderSession();
    if (focusRack) $("rack").focus();
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
  position.racks[0] = displayTiles(match[1]);
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
  if (action === GameAction.CHALLENGE && !challengeTarget()) return;
  const note = $("event-note").value;
  if (
    action === GameAction.CHALLENGE &&
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
    const liveChallenge = isLive() && action === GameAction.CHALLENGE;
    const { game } = await engine.gameAction(liveChallenge ? liveAction(GameAction.LIVE_CHALLENGE) : {
      text: recordedGame.gcg,
      lexicon: restorePosition(recordedGame.positions[0]).lexicon,
      index: historyIndex,
      note,
      action,
    });
    if (liveChallenge) {
      chargeClock();
      game.positions.at(-1).clocks = [...session.remaining];
      for (let index = 0; index <= historyIndex; index++)
        game.positions[index] = structuredClone(recordedGame.positions[index]);
      session.started = performance.now();
    }
    saveGameUndo();
    game.filename = recordedGame.filename;
    if (action === GameAction.CHALLENGE && !liveChallenge) clearFinishedGameResult(game);
    adoptGame(game, restorePosition(recordedGame.positions[0]).lexicon);
    if (liveChallenge && game.ended) finishSession("Game over");
    message(action === GameAction.NOTE ? "Note saved" : "Challenge recorded");
  } catch (failure) {
    error(failure.message);
  } finally {
    setBusy(false);
    renderSession();
    scheduleComputer();
  }
}
$("save-note").addEventListener("click", () => changeRecordedEvent(GameAction.NOTE));
$("challenge-move").addEventListener("click", () => changeRecordedEvent(GameAction.CHALLENGE));
// During play, only draft tiles move. Explicit position editing can also move
// recorded tiles, preserving ownership and participating in editor undo.
function canDragTiles() {
  return !busy && !terminalPosition() && !(isLive() && (!humanTurn() || session.paused));
}
function rackDrop(source, gap) {
  const index = source.playerRack ?? 0;
  const next = reorderRack(position.racks[index], index ? [] : draftTiles, source.rackIndex, gap);
  position.racks[index] = next.rack;
  if (!index) draftTiles = next.tiles.filter(tile => !source.square || tile.row !== source.row || tile.col !== source.col);
  rackChoice = null;
  if (source.square) renderDraft();
  else renderPosition();
}
tileDrag = installTileDrag({
  onFinish: fitGamePanels,
  board:$("board"), rack:$("rack-tiles"),
  sourceFor(target) {
    const rackTile = target.closest("#rack-tiles .rack-tile");
    if (rackTile && canArrangeRack()) return {element:rackTile, letter:rackTile.dataset.letter,
      rackIndex:+rackTile.dataset.rackIndex, playerRack:displayedRackIndex()};
    if (!canDragTiles()) return null;
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
    const target = document.elementFromPoint(x, y);
    const rack = target?.closest("#rack-tiles");
    if (rack) {
      if (source.square ? !canDragTiles() : !canArrangeRack()) return null;
      if ($("edit").checked && source.square && position.racks[0].length >= 7) return null;
      const tiles = [...rack.children];
      const before = tiles.findIndex(tile => { const rect = tile.getBoundingClientRect(); return x < rect.left + rect.width / 2; });
      return {rack, gap:before < 0 ? tiles.length : before};
    }
    if (!canDragTiles() || (!recordedGame && !$("edit").checked)) return null;
    const square = target?.closest("#board .square");
    if (!square) return null;
    const row = +square.dataset.row, col = +square.dataset.col;
    if (source.square === square) return {square, row, col};
    if (position.board[row][col] || draftTiles.some(tile => tile.row === row && tile.col === col)) return null;
    return {square, row, col};
  },
  onTap(source, pointerType) {
    if (!canDragTiles() || source.playerRack || source.square || $("edit").checked || !recordedGame) return;
    const now = performance.now();
    const doubleTap = pointerType === "touch" && lastRackTap?.index === source.rackIndex &&
      lastRackTap.letter === source.letter && lastRackTap.generation === positionGeneration &&
      now - lastRackTap.time < 400;
    lastRackTap = pointerType === "touch" && !doubleTap
      ? {index:source.rackIndex, letter:source.letter, generation:positionGeneration, time:now} : null;
    if (doubleTap && entryAnchored && cursor) {
      const {row, col} = cursor;
      rackChoice = null;
      if (source.letter === "?") chooseBlank(row, col, null, source.rackIndex);
      else addEntryTile(row, col, source.letter, false, null, source.rackIndex);
      return true;
    }
    rackChoice = rackChoice?.index === source.rackIndex ? null : {letter:source.letter, index:source.rackIndex};
    renderPosition();
  },
  onDrop(source, target) {
    lastRackTap = null;
    if (!source.square && target.rack) {
      if (canArrangeRack() && source.playerRack === displayedRackIndex()) rackDrop(source, target.gap);
      return;
    }
    if (!canDragTiles() || source.square === target.square && source.square) return;
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
  continuationTiles = new Set();
  continuationSteps = steps.length;
  document.querySelectorAll("[data-pv-step]").forEach(button => button.setAttribute("aria-pressed", String(Number(button.dataset.pvStep) === steps.length)));
  steps.forEach((move, index) => {
    for (const tile of previewMove(continuationPosition, move)) {
      continuationPosition.board[tile.row][tile.col] = tile.letter;
      continuationTiles.add(`${tile.row},${tile.col}`);
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
    challengePoints: +$("challenge-points").value,
    challengeUnit: $("challenge-unit").value,
    strategy: $("bot-analysis").value,
    seconds: Math.max(1, Math.min(120, +$("bot-seconds").value || 5)),
    manageTime: $("bot-auto-time").checked,
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
  renderSettingsAvailability();
  $("board-zoom").disabled = false;
  $("board").classList.toggle("entry-available", canDragTiles() && !!($("edit").checked || recordedGame));
  $("show-live-analysis").disabled = false;
  $("new-game").disabled = busy && !botRunning;
  const live = isLive();
  for (const id of [
    "rack", "opponent-rack", "score", "opponent-score", "edit", "clear",
    "on-turn-player", "import",
  ]) $(id).disabled = busy || !!live;
  const entryLocked = busy || terminalPosition() ||
    (live && (session.paused || !humanTurn()));
  for (const id of ["play-move", "pass-move", "exchange-move", "recall-tiles"])
    $(id).disabled = !!entryLocked;
  for (const element of document.querySelectorAll("#shuffle-rack, #sort-rack, #rack-tiles .rack-tile"))
    element.disabled = !canArrangeRack();
  document.body.classList.toggle("playing", !!live);
  document.body.classList.toggle(
    "show-analysis",
    !live || session.kind === "watch" || $("show-live-analysis").checked,
  );
  $("play-session").hidden = !session;
  $("session-result").classList.toggle("thinking", !!(live && botRunning && !session.paused));
  $("session-controls").hidden = !live;
  $("session-result").hidden = !!session?.finished &&
    $("hide-spoilers").checked && !terminalPosition();
  $("position-editor").hidden = !!live;
  for (const [index, suffix] of ["one", "two"].entries()) {
    const playerName = recordedGame?.players[index] || `Player ${index + 1}`;
    const thinking = !!(live && botRunning && !session.paused && index === position.onTurn);
    const name = $("name-" + suffix);
    const label = thinking ? `${playerName} is thinking…` : playerName;
    if (name.textContent !== label) name.textContent = label;
    name.classList.toggle("thinking", thinking);
    name.title = label;
    $("points-" + suffix).textContent =
      position.scores[index === position.onTurn ? 0 : 1];
    $("player-" + suffix).classList.toggle(
      "on-turn",
      !!recordedGame && index === position.onTurn && !terminalPosition() &&
        !(session?.finished && historyIndex === recordedGame.positions.length - 1),
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
  // Clock refreshes must not replace the text node under a pressed mouse.
  // WebKit otherwise drops the click between pointerdown and pointerup.
  const pauseLabel = botRunning ? "Pause" : session.paused ? "Resume game" : "Pause";
  if ($("pause-game").textContent !== pauseLabel)
    $("pause-game").textContent = pauseLabel;
  $("pause-game").disabled = session.finished;
  const finishLabel = session.kind === "play" ? "Resign" : "End game";
  if ($("finish-game").textContent !== finishLabel)
    $("finish-game").textContent = finishLabel;
  $("finish-game").disabled = session.finished || busy;
  $("finish-game").title = busy && botRunning ? "Pause the computer before ending the game." : "";
  const sessionMessage =
    session.result ||
    (session.paused
      ? "Paused"
      : botRunning
        ? `${recordedGame.players[position.onTurn]} is thinking…`
        : recordedGame.players[position.onTurn] === "You"
          ? "Your turn"
          : `${recordedGame.players[position.onTurn]}’s turn`);
  if ($("session-result").textContent !== sessionMessage)
    $("session-result").textContent = sessionMessage;
  if (live) {
    $("edit-history-move").disabled = true;
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
  for (const key of privateAnalysisKeys) {
    const saved = analysisCache.get(key);
    if (saved) cacheWrites = cacheWrites.then(() => saveAnalysis(key, saved)).catch(() => {
      $("analysis-storage-warning").hidden = false;
    });
  }
  privateAnalysisKeys.clear();
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
function currentComputerSearchPlan(mode) {
  return computerSearchPlan(
    {...session, remaining:session.remaining[position.onTurn]},
    inventory(position).unseenCount + position.racks[0].length,
    mode,
  );
}
async function computerTurn() {
  if (!isLive() || busy || botRunning || session.paused) return;
  botRunning = true;
  const currentSession = session;
  try {
    chargeClock();
    const plan = currentComputerSearchPlan(session.strategy === "kibitz" ? "kibitz" : searchMode());
    const complete = await analyze(plan.mode, {...plan, computerTime:true});
    if (
      session !== currentSession ||
      session.finished ||
      canceled || session.paused
    )
      return;
    if (!complete) throw new Error($("error").textContent || "Computer analysis failed. Resume to retry.");
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
function validateNewGameCGP() {
  const input = $("new-game-cgp");
  const cgp = input.value.trim();
  let startingPosition = null;
  let failure = "";
  try {
    if (cgp) {
      startingPosition = parseCGP(cgp, position.lexicon);
      const { unseen, unseenCount } = inventory(startingPosition);
      if (unseenCount <= 7 && startingPosition.racks.every(rack => !rack)) throw new Error("Enter at least one rack to start from this position.");
      if (!startingPosition.racks[0] && startingPosition.racks[1]) {
        const pool = {...unseen};
        for (const tile of startingPosition.racks[1]) pool[tile]--;
        const remainingTiles = Object.entries(pool).map(([tile, count]) => tile.repeat(count)).join("");
        if (remainingTiles.length <= 7) startingPosition.racks[0] = remainingTiles;
      } else if (unseenCount <= 7) {
        startingPosition.racks[1] = Object.entries(unseen).map(([tile, count]) => tile.repeat(count)).join("");
      }
      const remaining = inventory(startingPosition).unseenCount - startingPosition.racks[1].length;
      if (remaining === 0 && startingPosition.racks.some(rack => !rack)) {
        throw new Error("This position has already ended.");
      }
      if (startingPosition.zeros >= 6) throw new Error("This position has already ended.");
    }
  } catch (error) {
    failure = error.message;
  }
  input.setAttribute("aria-invalid", String(Boolean(failure)));
  input.setCustomValidity(failure);
  const status = $("new-game-cgp-status");
  status.textContent = failure || (cgp ? `Valid CGP · ${startingPosition.lexicon}` : "");
  status.classList.toggle("invalid", Boolean(failure));
  $("first-player").disabled = Boolean(cgp);
  $("start-game").disabled = busy || Boolean(engine?.fatalError) || Boolean(failure);
  return { valid: !failure, startingPosition };
}
$("new-game-cgp").addEventListener("input", () => {
  $("new-game-error").textContent = "";
  validateNewGameCGP();
});
function updateGameSetup() {
  const kind = $("game-kind").value;
  const penalty = $("challenge-rule").value === "penalty";
  $("challenge-points-label").hidden = !penalty;
  $("challenge-unit-label").hidden = !penalty;
  $("challenge-help").textContent = {
    void: "Phony plays cannot be played.",
    single: "A failed challenge has no penalty.",
    double: "A failed challenge loses your turn.",
    penalty: "A failed challenge awards the bonus to the player who made the play. Per word counts every word formed; Challenge checks all of them.",
  }[$("challenge-rule").value];
  $("computer-setup").hidden = kind === "record";
  $("human-side").closest("label").hidden = kind !== "play";
  $("watch-delay").closest("label").hidden = kind !== "watch";
  const staticPlay = $("bot-analysis").value === "kibitz";
  const timed = Number($("game-time").value) > 0;
  $("bot-auto-time").disabled = !timed || staticPlay;
  $("bot-seconds").disabled = staticPlay || (timed && $("bot-auto-time").checked);
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
    for (const selector of ["first-player", "human-side"])
      $(selector).options[index].textContent = input.value.trim() || `Player ${index + 1}`;
  }
}
for (const id of ["player-one-name", "player-two-name"]) {
  $(id).addEventListener("input", () => {
    $(id).dataset.custom = "true";
    updateGameSetup();
  });
}
for (const id of ["game-time", "bot-analysis", "bot-auto-time", "challenge-rule"])
  $(id).addEventListener("change", updateGameSetup);
$("game-kind").addEventListener("change", updateGameSetup);
$("human-side").addEventListener("change", updateGameSetup);
document.querySelectorAll("[data-game-kind]").forEach((button) => {
  button.addEventListener("click", () => {
    $("game-kind").value = button.dataset.gameKind;
    updateGameSetup();
  });
});
function restoreGameTime() {
  try {
    const time = localStorage.getItem("magpie-game-time");
    if ([...$("game-time").options].some(option => option.value === time)) $("game-time").value = time;
  } catch {}
}
restoreGameTime();
updateGameSetup();
$("analysis-details").addEventListener("click", () =>
  $("analysis-details-dialog").showModal(),
);
$("close-analysis-details").addEventListener("click", () =>
  $("analysis-details-dialog").close(),
);
$("pause-game").addEventListener("click", () => {
  if (!isLive()) return;
  chargeClock();
  session.paused = botRunning || !session.paused;
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
$("show-live-analysis").addEventListener("change", () => { renderSession(); renderResults(); });
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
    const { game } = await engine.gameAction(liveAction(GameAction.DEAL));
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
  if (!canExportGame() || hiddenLiveGame()) return;
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
  renderSession();
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
  return simulationMoveKey(position, move);
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
  if (mode === "peg" && busy) return `Played ${row.rank == null ? "(unranked)" : `#${row.rank}`}`;
  if (mode === "peg" && row.depth !== results[0].depth)
    return `Played ${row.rank == null ? "(unranked)" : `#${row.rank}`} · comparison needs equal depth`;
  const loss = Math.max(0, results[0][metric] - row[metric]);
  return `Played ${row.rank == null ? "(unranked)" : `#${row.rank}`} · ${format(loss, mode === "endgame" ? 0 : 2)} ${metric === "win" ? "win %-points" : metric} lost`;
}
function gameEvents() {
  return (
    recordedGame?.gcg.split(/\r?\n/).filter((line) => line.startsWith(">")) ||
    []
  );
}
// Count each play as a turn, while pairing players in the table. Score
// adjustments belong to the preceding turn and row.
function historyEntries() {
  const secondPlayer = recordedGame.gcg.match(/^#player2\s+(\S+)/m)?.[1];
  let turn = 0, rowNumber = 0, previousPlayer = 1;
  return gameEvents().map((line, index) => {
    const nick = line.slice(1, line.indexOf(":"));
    const player = nick === secondPlayer ? 1 : 0;
    const before = restorePosition(recordedGame.positions[index]);
    const parts = historyPlayParts(privateHistoryLine(line), before);
    const bingo = previewMove(before, parts.move).length === 7;
    if (parts.move === "--" || parts.move === "(challenge)") {
      parts.adjustment = true;
      parts.challenge = true;
      parts.move = parts.move === "--" ? "Withdrawn after challenge" : "Challenge bonus";
    }
    if (!rowNumber || (!parts.adjustment && player <= previousPlayer)) rowNumber++;
    if (!turn || !parts.adjustment) turn++;
    if (!parts.adjustment) previousPlayer = player;
    const rack = line.match(/^>[^:]+:\s+([A-Z?]+)\s/)?.[1] || "";
    return {index, nick, player, turn, rowNumber, parts, rack, bingo};
  });
}
function renderReviewTable() {
  if (!recordedGame) return;
  renderHistoryOptions();
  const hidden = $("hide-spoilers").checked;
  const header = document.createElement("tr");
  for (const name of recordedGame.players) {
    const cell = document.createElement("th");
    cell.textContent = name;
    header.append(cell);
  }
  $("history-table").tHead.replaceChildren(header);
  const rows = [];
  let cells;
  for (const {index, player, turn, rowNumber, parts, rack, bingo} of historyEntries()) {
    if (rowNumber > rows.length) {
      const row = document.createElement("tr");
      cells = [document.createElement("td"), document.createElement("td")];
      row.append(...cells);
      rows.push(row);
    }
    const cell = cells[player];
    if (historyIndex === index) cell.classList.add("current");
    const button = document.createElement("button");
    button.dataset.eventIndex = index;
    if (hidden && index >= historyIndex) {
      button.textContent = "•••";
    } else {
      for (const [key, value] of [["play", `${parts.adjustment ? "" : `${turn}. `}${displayMove(parts.move)}`], ["score", parts.score], ["cumulative", parts.cumulative]]) {
        const span = document.createElement("span");
        span.className = `history-${key}`;
        if (key === "play") {
          if (!parts.adjustment) {
            const number = document.createElement("span");
            number.textContent = `${turn}.`;
            span.append(number, " ");
          }
          span.append(moveTiles(displayMove(parts.move), player, restorePosition(recordedGame.positions[index])));
          if (span.querySelector(".move-word")) span.classList.add("history-tile-play");
        } else span.textContent = value;
        if (button.childNodes.length) button.append(" ");
        button.append(span);
      }
    }
    if (!parts.adjustment && !(hidden && index > historyIndex)) {
      const rackLine = document.createElement("span");
      rackLine.className = "history-rack";
      rackLine.textContent = historyRackLabel(player, rack, index, bingo);
      rackLine.setAttribute("aria-label", `Rack: ${rackLine.textContent}`);
      button.append(rackLine);
    }
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
  if (concealComputerAnalysis()) { clearResults(); return; }
  let saved =
    analysisCache.get(cacheKey(searchMode())) ||
    analysisCache.get(cacheKey("kibitz"));
  if (!saved) {
    const prefix = `${analysisPositionKey(analysisPosition())}|${searchMode()}|`;
    const suffix = `|${$("hide-spoilers").checked ? "hidden" : ""}`;
    saved = [...analysisCache.entries()]
      .filter(([key]) => key.startsWith(prefix) && key.endsWith(suffix))
      .map(([, value]) => value)
      .sort((a, b) => b.savedAt - a.savedAt)[0];
    if (saved) {
      $("plies").value = saved.plies;
      settingsSummary();
    }
  }
  if (!saved) return;
  mode = saved.mode;
  results = structuredClone(saved.results);
  analysisMeta = saved.meta;
  analysisIterations = saved.iterations || 0;
  pegProgress = structuredClone(saved.pegProgress || null);
  setRawAnalysis(saved.raw);
  renderResults();
}
async function analyzeDraft() {
  if (busy || isLive() || !recordedGame) return;
  const generation = positionGeneration;
  let listener;
  try {
    const move = normalizeMove(draftTiles.length ? moveNotation(position, draftTiles, vertical) : selectedMove);
    const targetMode = mode === "sim" ? "sim" : "kibitz";
    const key = cacheKey(targetMode);
    const saved = analysisCache.get(key);
    const existing = saved?.results.find(row => moveIdentity(row.move) === moveIdentity(move));
    if (existing) {
      existing.manual = true;
      const visible = results.find(row => moveIdentity(row.move) === moveIdentity(move));
      if (visible) visible.manual = true;
      message("Play added");
      return;
    }
    cancelDraftCheck();
    setBusy(true);
    $("stop").disabled = true;
    error("");
    message("Adding play…");
    await previewPromise;
    await engine.prepare(position.lexicon);
    let text = "";
    listener = ({detail}) => {
      if (detail.type === "output" && detail.command === "shmoves") text = detail.text;
    };
    engine.addEventListener("message", listener);
    const settings = readSettings();
    // Adding one candidate needs no expensive map preparation. Reuse an
    // installed map; otherwise use the ordinary generator until the next sim.
    if (engine.wmpKey !== `${position.lexicon}:${settings.wmpSource}`) {
      settings.wmpSource = "off";
      settings.wmp = false;
      engine.wmpKey = null;
    }
    const commands = analysisCommands(analysisPosition(), "kibitz", settings).slice(0, 2);
    const extras = new Map([...(saved?.results || []),
      ...(analysisCache.get(cacheKey("kibitz"))?.results.filter(row => row.manual) || []),
      {move}].map(row => [moveIdentity(row.move), normalizeMove(row.move)]));
    commands[0] = commands[0].replace(/-numplays \d+/, `-numplays ${settings.candidates}`)
      .replace(/-maxnumdplays \d+/, `-maxnumdplays ${settings.candidates + extras.size}`);
    await engine.run([...commands, "generate", `addmoves ${[...extras.values()].join(",")}`, "shmoves"]);
    if (generation !== positionGeneration) return;
    const generated = parseResults(text, "kibitz");
    const candidate = generated.find(row => moveIdentity(row.move) === moveIdentity(move));
    if (!candidate) throw new Error("Could not analyze this play.");
    mode = targetMode;
    const previous = new Map((saved?.results || []).map(row => [moveIdentity(row.move), row]));
    results = generated.map((row, index) => {
      const old = previous.get(moveIdentity(row.move));
      return {...row,
        ...(mode === "sim" ? {iterations:0, win:NaN, winSE:NaN, equity:NaN, equitySE:NaN, plyAverages:[]} : {}),
        ...old, manual:!!old?.manual || moveIdentity(row.move) === moveIdentity(move), rank:index + 1};
    });
    analysisIterations = saved?.iterations || 0;
    analysisMeta = mode === "sim" ? analysisSummary("", "sim") : "Static equity";
    setRawAnalysis(saved?.raw || "");
    pegProgress = null;
    recallTiles(false);
    selectedMove = candidate.move;
    retainAnalysis(key);
    message("Play added");
  } catch (failure) {
    if (generation === positionGeneration) error(failure.message);
  } finally {
    if (listener) engine.removeEventListener("message", listener);
    setBusy(false);
    renderResults();
    renderPosition();
  }
}
$("analyze-move").addEventListener("click", analyzeDraft);
$("continue-selected").addEventListener("click", () => {
  const moves = selectedMove ? [selectedMove] : [];
  if (moves.length) analyze("sim", {resume:true, moves});
});
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
    return endgameSummary(text);
  }
  if (kind === "peg")
    return `${inventory(position).bagCount} in bag · ${results[0]?.depth || "Solving"}`;
  return "Static equity";
}

const controlHints = {
  "copy-gcg": "Copy the complete game as GCG.",
  "pause-game": "Pause the game clock and computer; resume when ready.",
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
  "analyze-move": "Add this move to the kibitz list or simulation without recording a turn.",
  "continue-selected": "Add iterations only to the selected play; keep the other results.",
  "auto-analyze": "Analyze automatically when you select a turn in history.",
  "new-game":
    "Start a game with named players and enter each rack as you go.",
  "load-gcg": "Open a GCG game record.",
  "save-gcg": "Save the complete game, including moves and notes.",
  import: "Paste a CGP position. Lowercase letters are blanks.",
  copy: "Copy this position as CGP. Tile ownership is not included.",
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
  "sort-rack": "Sort your rack using the tile order chosen in Appearance settings.",
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
  "tile-order": "Order racks, leaves and exchanges throughout Magpie.",
  "unseen-order": "Order the unseen tile pool independently of racks and leaves.",
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
    hint = !canDragTiles()
      ? "Drag to arrange tiles."
      : recordedGame
        ? mobileBoard.matches
          ? "Tap a starting square, then double-tap rack tiles to place them · Drag to arrange tiles"
          : "Drag to arrange tiles or place them on the board."
        : "Drag to arrange tiles · Edit the Rack field to change tiles";
  } else if (target?.id === "results") {
    hint =
      "Select a move to preview it on the board; select it again to clear the preview.";
  }
  $("context-status").textContent = hint;
}
for (const type of ["pointerover", "focusin"]) {
  document.addEventListener(type, (event) => {
    if (event.pointerType === "touch") return;
    hintTarget = contextTarget(event.target) || contextTarget(document.activeElement);
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
try {
  // Older builds used a shared marker, which could belong to another tab.
  localStorage.removeItem("magpie-wmp-building");
  const crashed = sessionStorage.getItem("magpie-wmp-aborted") ||
    (performance.getEntriesByType("navigation")[0]?.type === "reload" && sessionStorage.getItem("magpie-wmp-building"));
  if (crashed) {
    $("wmp-source").value = "off";
    localStorage.setItem("magpie-wmp-source", "off");
    const notice = "A word-map build stopped unexpectedly. Word maps are off; you can enable them again in Settings.";
    $("wmp-cache-status").textContent = notice;
    $("wmp-recovery-notice").textContent = notice;
    $("wmp-recovery-notice").hidden = false;
  }
  sessionStorage.removeItem("magpie-wmp-aborted");
  sessionStorage.removeItem("magpie-wmp-building");
} catch {}
let wmpWarning = "";
renderPosition();
renderResults();
$("new-game-dialog").showModal();
async function startEngine() {
  setBusy(true);
  $("stop").disabled = true;
  message("Preparing browser engine…");
  try {
    if (!(await ensureIsolation())) return;
    engine = new EngineClient();
    try {
      analysisCache = await loadAnalyses();
      restoreAnalysis();
    } catch {
      $("analysis-storage-warning").hidden = false;
    }
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
      if (activeSolve && !canceled) {
        if (detail.type === "command_started" && /^(peg|endgame)(?: |$)/.test(detail.command)) {
          $("pause-solve").hidden = false;
          $("pause-solve").disabled = false;
        }
        if (detail.type === "search_pause") {
          if (detail.accepted && detail.paused !== activeSolve.paused) {
            if (detail.paused) activeSolve.pausedAt = performance.now();
            else activeSolve.pausedDuration += performance.now() - activeSolve.pausedAt;
            activeSolve.paused = detail.paused;
            message(detail.paused ? "Paused" : `Solving ${mode === "peg" ? "preendgame" : "endgame"}…`);
          }
          $("pause-solve").textContent = activeSolve.paused ? "Resume" : "Pause";
          $("pause-solve").disabled = !detail.accepted;
        }
      }
      if (gcgLoading && !["output", "status", "log"].includes(detail.type)) {
        logGCG("engine.message", { type: detail.type });
      }
      if (detail.type === "wmp_progress") {
        message(detail.text);
        $("wmp-cache-status").textContent = detail.text;
        $("stop-wmp").hidden = botRunning;
        $("stop-wmp").disabled = canceled || botRunning;
      }
      if (detail.type === "wmp_prepare_started") wmpWarning = "";
      if (detail.type === "wmp_ready") {
        $("stop-wmp").hidden = true;
        $("wmp-cache-status").textContent = wmpWarning || (detail.stopped ? "Stopped" : detail.unavailable ? "Word maps unavailable" : detail.cached ? "Word maps cached" : "Word maps ready for this session");
      }
      if (detail.type === "wmp_cache_warning") {
        wmpWarning = detail.text;
        $("wmp-cache-status").textContent = detail.text;
      }
      if (detail.type === "output" || detail.type === "status")
        acceptOutput(detail.text, detail.command || "");
    });
    await engine.ready;
    engineInitialized = true;
    $("new-game-status").hidden = true;
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
    button.dataset.unavailable = String(!file && $("wmp-source").value === "off");
    button.disabled = analysisSettingsLocked() || button.dataset.unavailable === "true";
    button.setAttribute("aria-label", `${file ? "Remove" : "Cache"} ${lexicon} WMP`);
    button.addEventListener("click", async () => {
      if (busy) return;
      setBusy(true);
      renderWMPCache();
      $("wmp-cache-status").textContent = "";
      wmpWarning = "";
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
        const status = canceled ? "Stopped" : file ? `${lexicon} WMP removed` : wmpWarning || (engine.wmpCached ? `${lexicon} word maps cached` : `${lexicon} word maps were not cached`);
        message(status);
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
$("wmp-source").addEventListener("change", () => {
  $("wmp-recovery-notice").hidden = true;
  renderWMPCache();
});

$("stop-wmp").addEventListener("click", () => { if (!botRunning) stop(); });
