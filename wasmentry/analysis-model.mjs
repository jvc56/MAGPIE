// Position and result adapters. Analysis remains in MAGPIE, never in the UI.
export const LEXICA = ["CSW24", "NWL23"];
export const VALUES = {
  A: 1,
  B: 3,
  C: 3,
  D: 2,
  E: 1,
  F: 4,
  G: 2,
  H: 4,
  I: 1,
  J: 8,
  K: 5,
  L: 1,
  M: 3,
  N: 1,
  O: 1,
  P: 3,
  Q: 10,
  R: 1,
  S: 1,
  T: 1,
  U: 1,
  V: 4,
  W: 4,
  X: 8,
  Y: 4,
  Z: 10,
  "?": 0,
};
const COUNTS = [
  9, 2, 2, 4, 12, 2, 3, 2, 9, 1, 1, 4, 2, 6, 8, 2, 1, 6, 4, 6, 4, 2, 2, 1, 2,
  1,
];
export const SAMPLES = {
  opening:
    "15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0 -lex CSW24",
  midgame:
    "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 -lex NWL23",
  peg: "15/3Q7U3/3U2TAURINE2/1CHANSONS2W3/2AI6JO3/DIRL1PO3IN3/E1D2EF3V4/F1I2p1TRAIK3/O1L2T4E4/ABy1PIT2BRIG2/ME1MOZELLE5/1GRADE1O1NOH3/WE3R1V7/AT5E7/G6D7 ENOSTXY/ACEISUY 356/378 0 -lex NWL23",
  endgame:
    "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW24",
};
export function parseCGP(text, defaultLexicon = "CSW24") {
  const tokens = text
    .trim()
    .replace(/^cgp\s+/i, "")
    .split(/\s+/);
  const [encoded, racks, scores, zeros, ...options] = tokens;
  if (
    !encoded ||
    !/^[A-Z?]{0,7}\/[A-Z?]{0,7}$/.test(racks || "") ||
    !/^-?\d+\/-?\d+$/.test(scores || "") ||
    !/^[0-6]$/.test(zeros || "")
  ) {
    throw new Error(
      "Use CGP: board rack/opponent score/score scoreless-turns -lex CSW24.",
    );
  }
  let lexicon = defaultLexicon;
  if (options.length) {
    if (options.length !== 2 || !["-lex", "lex"].includes(options[0])) {
      throw new Error("This preview accepts only the -lex option in a CGP.");
    }
    lexicon = options[1].replace(/;$/, "");
  }
  if (!LEXICA.includes(lexicon))
    throw new Error("This preview supports CSW24 and NWL23.");
  const rows = encoded.split("/");
  if (rows.length !== 15) throw new Error("The board must have 15 rows.");
  const board = rows.map((row) => {
    if (!/^(?:[A-Za-z]|[1-9]\d*)+$/.test(row))
      throw new Error(
        "Invalid board row. Lowercase letters designate blanks.",
      );
    const cells = [];
    for (const token of row.match(/[A-Za-z]|\d+/g)) {
      if (/^\d/.test(token)) {
        if (Number(token) > 15)
          throw new Error("A board row is longer than 15 squares.");
        cells.push(...Array(Number(token)).fill(""));
      } else cells.push(token);
    }
    if (cells.length !== 15)
      throw new Error("Every board row must contain 15 squares.");
    return cells;
  });
  const position = {
    board,
    owners: board.map((row) => row.map(() => -1)),
    onTurn: 0,
    racks: racks.split("/"),
    scores: scores.split("/").map(Number),
    zeros: Number(zeros),
    lexicon,
  };
  if (
    position.scores.some(
      (n) => !Number.isSafeInteger(n) || Math.abs(n) > 10000,
    )
  )
    throw new Error("Scores must be between -10000 and 10000.");
  inventory(position); // Reject impossible tile counts before handing them to C.
  return position;
}
// CGP cannot encode ownership. Keep it alongside CGP in local snapshots.
// Owner values mirror Board's 0/1/BOARD_OWNER_UNKNOWN convention.
export function snapshotPosition(position) {
  return {
    cgp: toCGP(position),
    owners: structuredClone(position.owners),
    onTurn: position.onTurn,
  };
}
export function restorePosition(saved) {
  if (typeof saved === "string") return parseCGP(saved);
  const position = parseCGP(saved.cgp);
  if (
    ![0, 1].includes(saved.onTurn) ||
    !Array.isArray(saved.owners) ||
    saved.owners.length !== 15 ||
    saved.owners.some(
      (row) =>
        !Array.isArray(row) ||
        row.length !== 15 ||
        row.some((owner) => ![-1, 0, 1].includes(owner)),
    )
  ) {
    throw new Error("Invalid saved tile ownership.");
  }
  position.onTurn = saved.onTurn;
  position.owners = saved.owners.map((row, r) =>
    row.map((owner, c) => (position.board[r][c] ? owner : -1)),
  );
  return position;
}
export function toCGP(position) {
  const encoded = position.board
    .map((row) => {
      let out = "",
        empty = 0;
      for (const tile of row) {
        if (!tile) empty++;
        else {
          if (empty) out += empty;
          empty = 0;
          out += tile;
        }
      }
      return out + (empty || "");
    })
    .join("/");
  return `${encoded} ${position.racks.join("/")} ${position.scores.join("/")} ${position.zeros} -lex ${position.lexicon}`;
}
export function inventory(position) {
  const unseen = Object.fromEntries(
    COUNTS.map((n, i) => [String.fromCharCode(65 + i), n]),
  );
  unseen["?"] = 2;
  for (const tile of [
    ...position.board.flat().filter(Boolean),
    ...position.racks[0],
  ]) {
    const letter = tile === tile.toLowerCase() ? "?" : tile;
    if (--unseen[letter] < 0)
      throw new Error(
        `Too many ${letter === "?" ? "blank" : letter} tiles in the position.`,
      );
  }
  const bag = { ...unseen };
  for (const tile of position.racks[1])
    if (--bag[tile] < 0)
      throw new Error(`Opponent rack contains unavailable ${tile} tiles.`);
  const unseenCount = Object.values(unseen).reduce((a, b) => a + b, 0);
  // A partial/unknown opponent rack still occupies up to seven unseen tiles.
  return { unseen, unseenCount, bagCount: Math.max(0, unseenCount - 7) };
}
const MOVE =
  "(?:[A-O](?:1[0-5]|[1-9])|(?:1[0-5]|[1-9])[A-O]) [A-Za-z().?]+|\\(exch [A-Z?]+\\)|pass";
const ranked = new RegExp(`^\\s*(\\d+):\\s+(${MOVE})\\s+(.*)$`);
export function parseResults(text, mode) {
  const rows = [];
  if (mode === "kibitz" || mode === "sim") {
    for (const line of text.split("\n")) {
      const match = line.match(ranked);
      if (!match) continue;
      const fields = match[3].trim().split(/\s+/);
      const leave = /^[A-Z?]+$/.test(fields[0]) ? fields.shift() : "";
      if (fields.length < (mode === "sim" ? 8 : 2)) continue;
      const row = {
        rank: +match[1],
        move: match[2],
        leave,
        score: +fields[0],
        equity: +fields[1],
      };
      if (mode === "sim")
        Object.assign(row, {
          win: +fields[2],
          winSE: +fields[3],
          equity: +fields[4],
          iterations: +fields[7],
          equitySE: +fields[5],
          ignored: fields[1] === "X",
          plyAverages: fields
            .slice(8)
            .filter((_, index) => index % 3 === 0)
            .map(Number),
        });
      rows.push(row);
    }
  } else if (mode === "peg") {
    const pattern = new RegExp(
      `^\\s*(?:(full|greedy|\\d+-ply)\\s+)?(\\d+)\\s+(${MOVE})\\s+(\\d+)\\s+(\\d+)\\s+(\\d+)\\s+([\\d.]+)\\s+([+\\-\\d.]+)(?:\\s|$)`,
    );
    for (const line of text.split("\n")) {
      const m = line.match(pattern);
      if (m)
        rows.push({
          rank: +m[2],
          move: m[3],
          wins: +m[4],
          ties: +m[5],
          losses: +m[6],
          win: +m[7],
          spread: +m[8],
          depth: m[1] || "current",
        });
    }
  } else if (mode === "endgame") {
    const pattern = new RegExp(
      `^\\s*(${MOVE})\\s+\\+?(\\d+)(?:\\s+\\([^)]*\\))?\\s+(-?\\d+)\\s+(-?\\d+)(.*)$`,
    );
    for (const line of text.split("\n")) {
      const pass = line.match(
        /^\s*(pass)\s+(?:\([^)]*\)\s+)?(-?\d+)\s+(-?\d+)(.*)$/,
      );
      const m = pass
        ? [pass[0], pass[1], "0", pass[2], pass[3], pass[4]]
        : line.match(pattern);
      if (m)
        rows.push({
          rank: rows.length + 1,
          move: m[1],
          score: +m[2],
          value: +m[3],
          spread: +m[4],
          continuation: m[5].trim(),
        });
    }
    if (!rows.length) {
      const pv = text.match(/PV 1 \(spread: (-?\d+), value: (-?\d+)/);
      const first = text.match(
        new RegExp(`^\\s*\\d+\\s+(${MOVE})\\s+\\+?(\\d+)`, "m"),
      );
      if (pv && first)
        rows.push({
          rank: 1,
          move: first[1],
          score: +first[2],
          value: +pv[2],
          spread: +pv[1],
          continuation:
            "Search in progress; see engine details for the current line.",
        });
    }
  }
  return rows;
}
// Display a GCG event using the board before that event, preserving scores.
export function formatHistoryPlay(line, position) {
  const play = line.replace(/^>[^:]+:\s+\S+\s+/, "");
  return play.replace(/^([A-O]\d+|\d+[A-O]) ([A-Za-z.]+)/, (_, coordinate, word) => {
    const vertical = /^[A-O]/.test(coordinate);
    let row = Number(coordinate.match(/\d+/)[0]) - 1;
    let col = coordinate.match(/[A-O]/)[0].charCodeAt(0) - 65;
    let formatted = "", through = "";
    for (const letter of word) {
      const existing = position.board[row]?.[col];
      if (existing) through += existing;
      else {
        if (through) formatted += `(${through})`;
        through = "";
        formatted += letter;
      }
      if (vertical) row++;
      else col++;
    }
    if (through) formatted += `(${through})`;
    return `${coordinate} ${formatted}`;
  });
}

export function previewMove(position, move) {
  const match = move.match(/^([A-O]\d+|\d+[A-O]) ([A-Za-z().?]+)$/);
  if (!match) return [];
  const vertical = /^[A-O]/.test(match[1]);
  let row = Number(match[1].match(/\d+/)[0]) - 1;
  let col = match[1].match(/[A-O]/)[0].charCodeAt(0) - 65;
  const tiles = [];
  for (const letter of match[2].replace(/[()]/g, "")) {
    if (row >= 15 || col >= 15) return [];
    if (!position.board[row][col] && letter !== ".")
      tiles.push({ row, col, letter });
    if (vertical) row++;
    else col++;
  }
  return tiles;
}
export function analysisCommands(position, mode, settings) {
  const { bagCount, unseenCount } = inventory(position);
  if (!position.racks[0]) throw new Error("Enter the rack to analyze.");
  if (position.zeros >= 6)
    throw new Error("This game has ended after six scoreless turns.");
  if (mode === "peg" && (bagCount < 1 || bagCount > 4))
    throw new Error("PEG needs 1–4 tiles in the bag.");
  if (mode === "endgame" && bagCount !== 0)
    throw new Error("Endgame needs an empty bag.");
  const prepared = structuredClone(position);
  if (mode === "endgame" && prepared.racks[1].length !== unseenCount) {
    const { unseen } = inventory(position);
    prepared.racks[1] = Object.entries(unseen)
      .map(([tile, n]) => tile.repeat(n))
      .join("");
  }
  const ttMiB = settings.ttMiB ?? 32;
  if (![16, 32].includes(ttMiB)) throw new Error("Search memory must be 16 or 32 MiB.");
  const common = `-threads ${settings.threads} -tlim ${settings.seconds}`;
  const commands = [
    `set -lex ${position.lexicon} -wmp ${settings.wmp ? "true" : "false"} -wit ${settings.wmp ? "true" : "false"} -numplays ${mode === "kibitz" ? 100 : settings.candidates} -s1 equity -s2 equity -r1 all -r2 all -maxnumdplays ${mode === "sim" && settings.playedMove ? 101 : 100} -shplies ${settings.plies} -ttfraction ${ttMiB / 256} -showbu false -shwithmoves false`,
    `cgp ${toCGP(prepared)}`,
  ];
  if (mode === "kibitz") commands.push("generate");
  if (mode === "sim") {
    commands.push("generate");
    if (settings.playedMove) commands.push(`addmoves ${settings.playedMove}`);
    const simulation = settings.playedMove
      ? `snoprune ${prepared.racks[1] || "-"},${settings.playedMove}`
      : "sim";
    commands.push(
      `set ${common} -plies ${settings.plies} -minplayiterations 500`,
    );
    commands.push(simulation);
  }
  if (mode === "peg") {
    // PEG's space-free move syntax uses a period between coordinate and word.
    // Expand GCG play-through dots before encoding that separator.
    let protectedMove = settings.playedMove || "-";
    const match = protectedMove.match(/^([A-O]\d+|\d+[A-O]) ([A-Za-z.]+)$/);
    if (match) {
      const vertical = /^[A-O]/.test(match[1]);
      const row = Number(match[1].match(/\d+/)[0]) - 1;
      const col = match[1].match(/[A-O]/)[0].charCodeAt(0) - 65;
      const word = [...match[2]]
        .map((tile, index) =>
          tile === "."
            ? prepared.board[row + (vertical ? index : 0)][
                col + (vertical ? 0 : index)
              ]
            : tile,
        )
        .join("");
      protectedMove = `${match[1]}.${word}`;
    }
    commands.push(
      `peg ${common} -pegtlim ${settings.seconds} -pegoutcomes false -pnoprune ${protectedMove}`,
    );
  }
  if (mode === "endgame") {
    commands.push(
      `set ${common} -etlim ${settings.seconds} -eplies 25 -etopk 5`,
    );
    commands.push(
      `endgame${settings.playedMove ? ` ${settings.playedMove}` : ""}`,
    );
  }
  return commands;
}

export function combineSim(previous, next) {
  const saved = new Map(previous.map((row) => [row.move, row]));
  const combined = new Map(saved);
  for (const row of next) {
    const old = saved.get(row.move);
    if (!row.iterations && old?.iterations) continue;
    combined.set(row.move, row);
  }
  return [...combined.values()]
    .map((row) => {
      const old = saved.get(row.move);
      if (row === old || !old?.iterations || !row.iterations) return row;
      const total = old.iterations + row.iterations;
      const weighted = (a, b) =>
        (a * old.iterations + b * row.iterations) / total;
      function sem(a, ase, b, bse) {
        const sumSquares =
          ase ** 2 * old.iterations * (old.iterations - 1) +
          bse ** 2 * row.iterations * (row.iterations - 1) +
          ((a - b) ** 2 * old.iterations * row.iterations) / total;
        return Math.sqrt(sumSquares / ((total - 1) * total));
      }
      return {
        ...row,
        win: weighted(old.win, row.win),
        equity: weighted(old.equity, row.equity),
        winSE: sem(old.win, old.winSE, row.win, row.winSE),
        equitySE: sem(old.equity, old.equitySE, row.equity, row.equitySE),
        iterations: total,
        plyAverages: (row.plyAverages || []).map((value, index) =>
          weighted(old.plyAverages?.[index] ?? value, value),
        ),
      };
    })
    .sort((a, b) => b.win - a.win || b.equity - a.equity)
    .map((row, index) => ({ ...row, rank: index + 1 }));
}

export function analysisPositionKey(position) {
  const canonical = structuredClone(position);
  canonical.racks = canonical.racks.map(rack => [...rack].sort().join(""));
  return toCGP(canonical);
}
