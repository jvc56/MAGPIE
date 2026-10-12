// Presentation only: lowercase remains the portable GCG encoding for blanks.
export function moveTiles(move, player = 0, position = null) {
  const element = document.createElement('span');
  element.className = 'move-tiles';
  const match = move.match(/^([A-O]\d+|\d+[A-O]) ([A-Za-z().?]+)$/);
  if (!match) {
    element.textContent = move;
    return element;
  }
  const coordinate = document.createElement('span');
  coordinate.className = 'move-coordinate';
  coordinate.textContent = match[1];
  element.append(coordinate, ' ');
  const tiles = document.createElement('span');
  tiles.className = 'move-word';
  let row = Number(match[1].match(/\d+/)[0]) - 1;
  let col = match[1].match(/[A-O]/)[0].charCodeAt(0) - 65;
  const down = /^[A-O]/.test(match[1]);
  let through = false;
  const description = [];
  for (const encoded of match[2]) {
    if (encoded === '(' || encoded === ')') {
      through = encoded === '(';
      const separator = document.createElement('span');
      separator.className = 'visually-hidden';
      separator.textContent = encoded;
      tiles.append(separator);
      continue;
    }
    const existing = position?.board[row]?.[col];
    const playedThrough = through || encoded === '.' || !!existing;
    const letter = playedThrough && existing ? existing : encoded;
    const blank = /^[a-z]$/.test(letter);
    const tile = document.createElement('span');
    tile.className = `move-tile ${playedThrough ? 'playthrough' : `owner-${player}`}${blank ? ' blank' : ''}`;
    const glyph = document.createElement("span");
    glyph.className = "tile-letter";
    glyph.textContent = letter.toUpperCase();
    tile.append(glyph);
    const label = `${blank ? 'blank ' : ''}${letter.toUpperCase()}${playedThrough ? ', played through' : ''}`;
    tile.title = label;
    tile.setAttribute('aria-label', label);
    description.push(label);
    tiles.append(tile);
    if (down) row++;
    else col++;
  }
  element.append(tiles);
  element.setAttribute('aria-label', `${match[1]} ${description.join(' ')}`);
  return element;
}
