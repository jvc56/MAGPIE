// Board view + editor for the MAGPIE analysis board.
//
// Owns the visual 15x15 (or 21x21) grid, the fixed premium-square layout, the
// editable letters/blanks, the mouse entry cursor, and move previews. The app
// drives it: load() adopts engine state, the palette calls placeLetter(), and
// before each analysis run the app reads cgpBoard() to sync the engine.

const COL_LETTERS = 'ABCDEFGHIJKLMNOPQRSTUVWXY';

// English tile point values, used to render Woogles-style subscripts. Letters
// not in the map (other distributions) simply render without a value.
export const TILE_POINTS = {
  A: 1, B: 3, C: 3, D: 2, E: 1, F: 4, G: 2, H: 4, I: 1, J: 8, K: 5, L: 1, M: 3,
  N: 1, O: 1, P: 3, Q: 10, R: 1, S: 1, T: 1, U: 1, V: 4, W: 4, X: 8, Y: 4, Z: 10,
};

// Returns the point value for a displayed tile, or null when unknown / blank.
export function tilePoints(letterUpper, isBlank) {
  if (isBlank) {
    return 0;
  }
  const value = TILE_POINTS[letterUpper];
  return value === undefined ? null : value;
}

export class Board {
  constructor(rootEl, { onCursorChange, onEdit } = {}) {
    this.root = rootEl;
    this.onCursorChange = onCursorChange || (() => {});
    this.onEdit = onEdit || (() => {});
    this.editable = true; // set false while an engine op is in flight
    this.dim = 15;
    this.layout = []; // [row][col] -> bonus code ("", "dls", ...)
    this.cells = []; // [row][col] -> { letter, blank }
    this.center = { row: 7, col: 7 };
    this.cursor = null; // { row, col, dir: 'h' | 'v' }
    this.preview = null; // array of { row, col, l, k } ghost tiles
    this.highlight = null; // array of { row, col } persistent highlight
    this.cellEls = [];
    this._build(15);
  }

  // (Re)builds the DOM grid for a given dimension and wires cell events.
  _build(dim) {
    this.dim = dim;
    this.root.style.setProperty('--dim', String(dim));
    this.root.innerHTML = '';
    this.cellEls = [];

    const grid = document.createElement('div');
    grid.className = 'grid';

    // top-left corner spacer
    grid.appendChild(this._labelCell(''));
    // column letters
    for (let col = 0; col < dim; col++) {
      grid.appendChild(this._labelCell(COL_LETTERS[col]));
    }
    for (let row = 0; row < dim; row++) {
      grid.appendChild(this._labelCell(String(row + 1)));
      const rowEls = [];
      for (let col = 0; col < dim; col++) {
        const cell = document.createElement('button');
        cell.type = 'button';
        cell.className = 'cell';
        cell.dataset.row = String(row);
        cell.dataset.col = String(col);
        cell.addEventListener('click', () => this._onCellClick(row, col));
        cell.addEventListener('contextmenu', (e) => {
          e.preventDefault();
          this.eraseCell(row, col);
        });
        grid.appendChild(cell);
        rowEls.push(cell);
      }
      this.cellEls.push(rowEls);
    }
    this.root.appendChild(grid);

    // Start with a consistent empty model so cgpBoard()/render() are safe to
    // call before the first load() (e.g. when boot syncs an empty position).
    this.cells = [];
    this.layout = [];
    for (let row = 0; row < dim; row++) {
      const cellsRow = [];
      const layoutRow = [];
      for (let col = 0; col < dim; col++) {
        cellsRow.push({ letter: '', blank: false });
        layoutRow.push('');
      }
      this.cells.push(cellsRow);
      this.layout.push(layoutRow);
    }
  }

  _labelCell(text) {
    const el = document.createElement('div');
    el.className = 'label';
    el.textContent = text;
    return el;
  }

  // Adopts engine state: fixed premium layout + letters + center + dimension.
  load(state) {
    const board = state.board || [];
    const dim = state.dim || board.length || 15;
    if (dim !== this.dim || this.cellEls.length !== dim) {
      this._build(dim);
    }
    this.center = state.center || { row: (dim / 2) | 0, col: (dim / 2) | 0 };
    this.layout = [];
    this.cells = [];
    for (let row = 0; row < dim; row++) {
      const layoutRow = [];
      const cellsRow = [];
      for (let col = 0; col < dim; col++) {
        const c = (board[row] && board[row][col]) || { l: '', b: '' };
        layoutRow.push(c.b || '');
        if (c.l) {
          cellsRow.push({ letter: c.k ? c.l.toUpperCase() : c.l, blank: !!c.k });
        } else {
          cellsRow.push({ letter: '', blank: false });
        }
      }
      this.layout.push(layoutRow);
      this.cells.push(cellsRow);
    }
    this.preview = null;
    this.highlight = null;
    this.render();
    this.fit();
  }

  setEditable(editable) {
    this.editable = editable;
  }

  _onCellClick(row, col) {
    if (!this.editable) {
      return;
    }
    if (this.cursor && this.cursor.row === row && this.cursor.col === col) {
      this.cursor.dir = this.cursor.dir === 'h' ? 'v' : 'h';
    } else {
      this.cursor = { row, col, dir: this.cursor ? this.cursor.dir : 'h' };
    }
    this.onCursorChange(this.cursor);
    this.render();
  }

  setCursor(row, col, dir) {
    this.cursor = { row, col, dir: dir || 'h' };
    this.onCursorChange(this.cursor);
    this.render();
  }

  clearCursor() {
    this.cursor = null;
    this.onCursorChange(null);
    this.render();
  }

  _advance(step) {
    if (!this.cursor) {
      return;
    }
    if (this.cursor.dir === 'h') {
      this.cursor.col = Math.max(0, Math.min(this.dim - 1, this.cursor.col + step));
    } else {
      this.cursor.row = Math.max(0, Math.min(this.dim - 1, this.cursor.row + step));
    }
  }

  // Places a letter at the cursor (blank => lowercase designated tile) and
  // advances. Returns false if there is no active cursor.
  placeLetter(letter, blank) {
    if (!this.cursor) {
      return false;
    }
    const { row, col } = this.cursor;
    this.cells[row][col] = { letter: letter.toUpperCase(), blank: !!blank };
    this._advance(1);
    this.render();
    this.onEdit();
    return true;
  }

  // Removes the tile just before the cursor and steps back onto it.
  backspace() {
    if (!this.cursor) {
      return;
    }
    this._advance(-1);
    const { row, col } = this.cursor;
    this.cells[row][col] = { letter: '', blank: false };
    this.render();
    this.onEdit();
  }

  eraseCell(row, col) {
    if (!this.editable) {
      return;
    }
    this.cells[row][col] = { letter: '', blank: false };
    this.render();
    this.onEdit();
  }

  clearBoard() {
    for (let row = 0; row < this.dim; row++) {
      for (let col = 0; col < this.dim; col++) {
        this.cells[row][col] = { letter: '', blank: false };
      }
    }
    this.render();
    this.onEdit();
  }

  // Applies a played move's tiles to the board model (used when advancing the
  // game). Unlike the editing mutators this does not fire onEdit.
  applyPlaced(placed) {
    if (!placed) {
      return;
    }
    for (const t of placed) {
      this.cells[t.row][t.col] = { letter: t.l.toUpperCase(), blank: !!t.k };
    }
    this.render();
  }

  setPreview(placed) {
    this.preview = placed && placed.length ? placed : null;
    this.render();
  }

  setHighlight(placed) {
    this.highlight = placed && placed.length ? placed : null;
    this.render();
  }

  // Renders the board portion of a CGP string: run-length-encoded rows joined
  // by '/'. Blanks are emitted lowercase, played tiles uppercase.
  cgpBoard() {
    const rows = [];
    for (let row = 0; row < this.dim; row++) {
      let str = '';
      let run = 0;
      for (let col = 0; col < this.dim; col++) {
        const cell = this.cells[row][col];
        if (!cell.letter) {
          run++;
          continue;
        }
        if (run > 0) {
          str += run;
          run = 0;
        }
        str += cell.blank ? cell.letter.toLowerCase() : cell.letter.toUpperCase();
      }
      if (run > 0) {
        str += run;
      }
      rows.push(str);
    }
    return rows.join('/');
  }

  isEmpty() {
    for (let row = 0; row < this.dim; row++) {
      for (let col = 0; col < this.dim; col++) {
        if (this.cells[row][col].letter) {
          return false;
        }
      }
    }
    return true;
  }

  render() {
    const previewMap = new Map();
    if (this.preview) {
      for (const t of this.preview) {
        previewMap.set(t.row * this.dim + t.col, t);
      }
    }
    const highlightSet = new Set();
    if (this.highlight) {
      for (const t of this.highlight) {
        highlightSet.add(t.row * this.dim + t.col);
      }
    }
    for (let row = 0; row < this.dim; row++) {
      for (let col = 0; col < this.dim; col++) {
        const el = this.cellEls[row][col];
        const cell = this.cells[row][col];
        const bonus = this.layout[row] ? this.layout[row][col] || '' : '';
        const key = row * this.dim + col;
        const ghost = previewMap.get(key);

        el.className = 'cell';
        if (bonus) {
          el.classList.add('b-' + bonus);
        }
        if (this.center && this.center.row === row && this.center.col === col) {
          el.classList.add('center');
        }

        let text = '';
        let points = null;
        if (cell.letter) {
          text = cell.blank ? cell.letter.toLowerCase() : cell.letter;
          points = tilePoints(cell.letter.toUpperCase(), cell.blank);
          el.classList.add('tile');
          if (cell.blank) {
            el.classList.add('blank');
          }
        } else if (ghost) {
          text = ghost.k ? ghost.l.toLowerCase() : ghost.l;
          points = tilePoints(ghost.l.toUpperCase(), ghost.k);
          el.classList.add('ghost');
          if (ghost.k) {
            el.classList.add('blank');
          }
        } else if (bonus && bonus !== 'brk') {
          text = bonus.toUpperCase();
          el.classList.add('premium-label');
        }

        if (highlightSet.has(key)) {
          el.classList.add('highlight');
        }
        if (this.cursor && this.cursor.row === row && this.cursor.col === col) {
          el.classList.add('cursor');
          el.classList.add(this.cursor.dir === 'h' ? 'cursor-h' : 'cursor-v');
        }
        if (points != null && points > 0) {
          el.textContent = text;
          const pt = document.createElement('span');
          pt.className = 'pt';
          pt.textContent = String(points);
          el.appendChild(pt);
        } else {
          el.textContent = text;
        }
      }
    }
  }

  // Sizes the grid to the largest square that fits its container, and exposes
  // the per-cell pixel size as --cell so text scales with the board.
  fit() {
    const grid = this.root.querySelector('.grid');
    if (!grid) {
      return;
    }
    const avail = Math.min(this.root.clientWidth, this.root.clientHeight);
    if (avail <= 0) {
      return;
    }
    const side = Math.floor(avail);
    grid.style.width = side + 'px';
    grid.style.height = side + 'px';
    // The grid is one 16px label track + `dim` cell tracks separated by 1px
    // gaps (dim gaps total). Subtract both so --cell (used only for font sizing)
    // tracks the real rendered cell size.
    const labelPx = 16;
    const gaps = this.dim;
    const cellPx = Math.max(8, (side - labelPx - gaps) / this.dim);
    grid.style.setProperty('--cell', cellPx + 'px');
  }
}
