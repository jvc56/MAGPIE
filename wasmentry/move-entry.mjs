// Pending tiles never mutate the recorded board. Both typing and pointer input
// use the same rack accounting and notation builder.
export const tileKey = (row, col) => `${row},${col}`;
export function remainingRack(rack, tiles) {
  const remaining = [...rack];
  for (const { letter } of tiles) {
    const token = letter === letter.toLowerCase() ? "?" : letter;
    const index = remaining.indexOf(token);
    if (index < 0) throw new Error(`The rack has no ${token} tile left.`);
    remaining.splice(index, 1);
  }
  return remaining;
}
export function placeTile(
  position,
  tiles,
  row,
  col,
  letter,
  forceBlank = false,
) {
  if (row < 0 || row >= 15 || col < 0 || col >= 15 || position.board[row][col])
    throw new Error("Choose an empty board square.");
  if (tiles.some((tile) => tile.row === row && tile.col === col))
    throw new Error("There is already a pending tile on that square.");
  if (!/^[A-Za-z]$/.test(letter))
    throw new Error("Choose a letter for the blank.");
  const rack = remainingRack(position.racks[0], tiles);
  const upper = letter.toUpperCase();
  const blank = forceBlank || !rack.includes(upper);
  if (!rack.includes(blank ? "?" : upper))
    throw new Error(`The rack has no ${blank ? "blank" : upper} tile left.`);
  return [...tiles, { row, col, letter: blank ? upper.toLowerCase() : upper }];
}
export function moveNotation(position, tiles, vertical = false) {
  if (!tiles.length) return "";
  if (tiles.length > 1) {
    if (tiles.every((tile) => tile.row === tiles[0].row)) vertical = false;
    else if (tiles.every((tile) => tile.col === tiles[0].col)) vertical = true;
    else throw new Error("Place all tiles in one row or column.");
  } else {
    const { row, col } = tiles[0];
    const across = position.board[row][col - 1] || position.board[row][col + 1];
    const down =
      position.board[row - 1]?.[col] || position.board[row + 1]?.[col];
    if (across && !down) vertical = false;
    if (down && !across) vertical = true;
  }
  const dr = vertical ? 1 : 0,
    dc = vertical ? 0 : 1;
  const sorted = [...tiles].sort((a, b) =>
    vertical ? a.row - b.row : a.col - b.col,
  );
  let { row, col } = sorted[0];
  const last = sorted.at(-1);
  while (position.board[row - dr]?.[col - dc]) {
    row -= dr;
    col -= dc;
  }
  const coordinate = vertical
    ? `${String.fromCharCode(65 + col)}${row + 1}`
    : `${row + 1}${String.fromCharCode(65 + col)}`;
  let word = "";
  const pending = new Map(
    tiles.map((tile) => [tileKey(tile.row, tile.col), tile.letter]),
  );
  while (row < 15 && col < 15) {
    const letter =
      pending.get(tileKey(row, col)) || (position.board[row][col] ? "." : "");
    if (!letter) {
      if (vertical ? row <= last.row : col <= last.col)
        throw new Error("Fill the gaps between your tiles.");
      break;
    }
    word += letter;
    row += dr;
    col += dc;
  }
  return `${coordinate} ${word}`;
}
export function normalizeMove(value) {
  const move = value.trim().replace(/[()]/g, "").replace(/\s+/g, " ");
  if (/^pass$/i.test(move)) return "pass";
  const exchange = move.match(/^(?:ex|exch|exchange) ([A-Za-z?]{1,7})$/i);
  if (exchange) return `ex ${exchange[1].toUpperCase()}`;
  const play = move.match(
    /^([A-O](?:[1-9]|1[0-5])|(?:[1-9]|1[0-5])[A-O])[ .]([A-Za-z.]{1,15})$/i,
  );
  if (!play)
    throw new Error(
      "Enter a move such as 8H TRAIN, H8 TRAIN, ex AE, or pass. Lowercase tiles are blanks.",
    );
  return `${play[1].toUpperCase()} ${play[2]}`;
}
