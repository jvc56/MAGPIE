// Pending tiles never mutate the recorded board. Both typing and pointer input
// use the same rack accounting and notation builder.
export const tileKey = (row, col) => `${row},${col}`;
export function remainingRack(rack, tiles) {
  return rackSlots(rack, tiles).remaining.map(slot => slot.letter);
}
export function placeTile(
  position,
  tiles,
  row,
  col,
  letter,
  forceBlank = false,
  rackIndex = null,
) {
  if (row < 0 || row >= 15 || col < 0 || col >= 15 || position.board[row][col])
    throw new Error("Choose an empty board square.");
  if (tiles.some((tile) => tile.row === row && tile.col === col))
    throw new Error("There is already a pending tile on that square.");
  if (!/^[A-Za-z]$/.test(letter))
    throw new Error("Choose a letter for the blank.");
  const slots = rackSlots(position.racks[0], tiles).remaining;
  const rack = slots.map(slot => slot.letter);
  const upper = letter.toUpperCase();
  const blank = forceBlank || !rack.includes(upper);
  if (!rack.includes(blank ? "?" : upper))
    throw new Error(`The rack has no ${blank ? "blank" : upper} tile left.`);
  const slot = slots.find(slot => slot.letter === (blank ? "?" : upper) && (rackIndex == null || slot.index === rackIndex));
  if (!slot) throw new Error("That rack tile is no longer available.");
  return [...tiles, { row, col, letter: blank ? upper.toLowerCase() : upper, rackIndex:slot.index }];
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

// Keep the identity of duplicate letters while pending tiles reserve rack slots.
// Imported/preview moves have no slot yet; assign those deterministically.
export function rackSlots(rack, tiles = []) {
  const slots = [...rack].map((letter, index) => ({letter, index}));
  const reserved = new Set();
  const indexes = tiles.map(tile => {
    const token = tile.letter === tile.letter.toLowerCase() ? "?" : tile.letter;
    const slot = slots.find(slot => slot.index === tile.rackIndex && slot.letter === token && !reserved.has(slot.index));
    if (slot) reserved.add(slot.index);
    return slot?.index;
  });
  tiles.forEach((tile, index) => {
    if (indexes[index] != null) return;
    const token = tile.letter === tile.letter.toLowerCase() ? "?" : tile.letter;
    const slot = slots.find(slot => slot.letter === token && !reserved.has(slot.index));
    if (!slot) throw new Error(`The rack has no ${token} tile left.`);
    indexes[index] = slot.index;
    reserved.add(slot.index);
  });
  return {remaining: slots.filter(slot => !reserved.has(slot.index)), indexes};
}

// Destination is a gap in the visible rack, before removing the dragged tile.
export function reorderRack(rack, tiles, sourceIndex, gap) {
  const {remaining, indexes} = rackSlots(rack, tiles);
  if (!Number.isInteger(sourceIndex) || sourceIndex < 0 || sourceIndex >= rack.length ||
      !Number.isInteger(gap) || gap < 0 || gap > remaining.length) throw new Error("Invalid rack drop.");
  const order = [...rack].map((_, index) => index);
  const destination = remaining[gap]?.index ?? rack.length;
  order.splice(sourceIndex, 1);
  order.splice(destination - (sourceIndex < destination ? 1 : 0), 0, sourceIndex);
  return {
    rack: order.map(index => rack[index]).join(""),
    tiles: tiles.map((tile, index) => ({...tile, rackIndex:order.indexOf(indexes[index])})),
  };
}
