# Board Rendering Implementation

This document describes the board rendering system implemented in QtPie, based on the Python rendering code from the reference implementation.

## Overview

The board rendering system pre-renders all tile types at the window's resolution and composites them to create a pixel-perfect Scrabble board display.

## Architecture

### 1. TileRenderer (`tile_renderer.h/cpp`)

Pre-renders all possible tile types at a given pixel size:

- **Letter tiles (A-Z)**: Beige background with black letter and point value
- **Blank tiles (a-z)**: Same as letters but with outlined letter (no fill)
- **Premium squares**:
  - DL (Double Letter) - Light blue
  - TL (Triple Letter) - Blue
  - DW (Double Word) - Pink
  - TW (Triple Word) - Red
  - Star (Center) - Pink with star symbol
- **Empty squares**: Light background with subtle border

#### Key Features

- **Rounded corners**: Uses QPainterPath with rounded rectangles
- **Concave gradient**: Darker at edges, lighter in center (matches Python implementation)
- **Exact sizing**: All tiles are exactly the same pixel size
- **Font rendering**: Uses Arial Bold for letters, smaller font for point values
- **Caching**: All tiles pre-rendered and stored as QPixmaps

#### Constants (matching Python)

```cpp
static const double TILE_FRACTION = 0.88;        // Tile size vs square size
static const double CORNER_RADIUS_FRACTION = 0.25;  // Corner rounding
static const double GRADIENT_FRACTION = 0.98;    // Gradient area size
```

### 2. BoardRenderer (`board_renderer.h/cpp`)

Manages the complete 15x15 board layout:

- **Premium square positioning**: Standard Scrabble board layout
- **Pixel-perfect rendering**: Board is exactly 15×squareSize pixels
- **No margins**: The board itself has no margins (margins are handled by BoardView)

#### Premium Square Layout

The layout uses symmetry to simplify the code:

```cpp
// Map positions to edges
int rowFromEdge = std::min(row, BOARD_DIM - 1 - row);
int colFromEdge = std::min(col, BOARD_DIM - 1 - col);
```

This allows defining the layout for one quadrant and mirroring it.

### 3. BoardView (`board_view.h/cpp`)

The Qt widget that displays the board:

- **Pixel-perfect sizing**: Calculates exact square size that divides evenly
- **Responsive**: Re-renders on resize to maintain quality
- **Centered**: Calculates margins to center the board in available space
- **Square aspect ratio**: Always maintains 1:1 aspect ratio

#### Resize Logic

```cpp
// Calculate square size (rounded down to ensure it fits)
int boardArea = availableSize - marginTotal;
m_squareSize = boardArea / BOARD_DIM;

// Recalculate actual board size (pixel-perfect)
int actualBoardSize = m_squareSize * BOARD_DIM;

// Calculate margins to center the board
m_marginX = (availableSize - actualBoardSize) / 2;
m_marginY = (availableSize - actualBoardSize) / 2;
```

This ensures:
1. All squares are exactly the same size (integer pixels)
2. The board fits within the available space
3. The board is centered with equal margins

## Differences from Python Implementation

### Improvements

1. **Dynamic resolution**: Tiles are rendered at the actual display resolution, not pre-generated at fixed sizes
2. **Responsive**: Board re-renders on window resize
3. **Native rendering**: Uses Qt's native 2D graphics instead of PIL
4. **Memory efficient**: Only one set of tiles cached at current resolution

### Simplified

1. **No file I/O**: Tiles rendered in-memory, not saved to disk
2. **No heatmap support**: (Can be added later if needed)
3. **Single color scheme**: No multiple color variants (yet)

## Color Scheme

Matches the Python implementation:

| Element | Color (RGB) | Description |
|---------|-------------|-------------|
| Tile background | (245, 230, 190) | Beige |
| Board background | (235, 232, 217) | Light tan |
| Letter/text | (0, 0, 0) | Black |
| Double Letter | (173, 216, 230) | Light blue |
| Triple Letter | (0, 0, 255) | Blue |
| Double Word | (255, 192, 203) | Pink |
| Triple Word | (255, 0, 0) | Red |

## Usage

The board is automatically displayed when the application starts:

```cpp
BoardView *boardView = new BoardView(this);
// Board will render itself on first show/resize
```

## Future Enhancements

Potential additions to match full Python functionality:

1. **Letter compositing**: Display actual game state with letters on board
2. **Heatmap support**: Color-coded squares for move analysis
3. **Animation**: Smooth transitions when placing tiles
4. **Export**: Save board as PNG image
5. **Multiple themes**: Light/dark board themes

## Performance

- **Initial render**: ~50-100ms (depends on window size)
- **Resize**: Re-renders board at new resolution
- **Paint**: <1ms (just blits cached pixmap)
- **Memory**: ~1-5MB for cached tiles (depends on resolution)

Each tile is rendered once and cached as a QPixmap. The complete board is also cached and only re-rendered on resize.
