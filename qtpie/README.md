# QtPie - Qt Frontend for MAGPIE

QtPie is a Qt-based graphical user interface for the MAGPIE crossword game engine.

## Building

### Prerequisites

- Qt 6 installed (via Homebrew or Qt installer)
- C compiler (Xcode Command Line Tools on macOS)
- MAGPIE library built

### Build Steps

1. **Build the MAGPIE library first:**
   ```bash
   cd ..
   make libmagpie.a
   ```

   This creates `lib/libmagpie.a` which QtPie links against.

2. **Generate the Qt Makefile:**
   ```bash
   cd qtpie
   qmake QtPie.pro
   ```

3. **Build the application:**
   ```bash
   make
   ```

4. **Run the application:**
   ```bash
   open Magpie.app
   ```
   Or on Linux:
   ```bash
   ./Magpie
   ```

## Architecture

### C/C++ Integration

QtPie uses a wrapper layer to interface between C++ (Qt) and C (MAGPIE):

- **`magpie_wrapper.h/c`**: C wrapper functions that call MAGPIE C code
  - This avoids C++ compilation issues with MAGPIE's inline functions that return `void*`
  - The wrapper is compiled as C, while Qt code is compiled as C++

- **Qt Components**:
  - `main.cpp`: Application entry point and main widget
  - `board_view.h/cpp`: Widget for displaying the game board
  - `board_panel_view.h/cpp`: Panel containing board, rack, and controls
  - `responsive_layout.h/cpp`: Responsive layout manager
  - `colors.h/cpp`: Dark theme color palette

### Linking

The application links against:
- `libmagpie.a` - MAGPIE static library (built with release flags)
- Qt frameworks (QtWidgets, QtGui, QtCore)

## Current Status

The basic Qt skeleton is working and builds successfully. The UI structure is in place with:
- Board display widget
- History panel
- Analysis panel
- Responsive layout

**TODO**: Integrate actual MAGPIE game logic:
- Initialize a game with config
- Display the board state
- Handle user input for moves
- Show move generation results

## Development Notes

### Why a C Wrapper?

MAGPIE uses inline functions in headers with `malloc_or_die()` which returns `void*`. In C, this implicitly casts to typed pointers, but C++ requires explicit casts. Rather than modifying MAGPIE or adding casts everywhere, we use a C wrapper file (`magpie_wrapper.c`) that:

1. Compiles as C (no casting issues)
2. Provides clean function interfaces to C++
3. Forward-declares types in the header for C++ use

### Adding New MAGPIE Functionality

To add new MAGPIE functions to QtPie:

1. Add a wrapper function to `magpie_wrapper.c`
2. Add its declaration to `magpie_wrapper.h`
3. Call it from C++ code

Example:
```c
// magpie_wrapper.c
#include "../src/ent/game.h"

void magpie_play_move(Game *game, Move *move) {
    game_play_move(game, move);
}
```

```c
// magpie_wrapper.h
void magpie_play_move(Game *game, Move *move);
```

## Files

- `QtPie.pro` - Qt project file (qmake)
- `main.cpp` - Application entry and main window
- `magpie_wrapper.h/c` - C/C++ bridge
- `board_view.h/cpp` - Board display widget
- `board_panel_view.h/cpp` - Board panel container
- `responsive_layout.h/cpp` - Layout manager
- `colors.h/cpp` - Theme colors
- `QtPie-Info.plist` - macOS app bundle metadata
