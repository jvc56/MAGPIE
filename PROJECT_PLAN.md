# Project Plan: QtPie (MAGPIE Frontend)

## Objective
Create a modern, testable, and portable Qt/QML frontend for the MAGPIE Scrabble engine, replacing the previous "vibe coded" prototype.

## Design Philosophy
-   **Modern Aesthetics**: Use Qt Quick (QML) for a fluid, high-performance UI with animations and modern styling (Dark Mode, Glassmorphism).
-   **Separation of Concerns**: strict Model-View-ViewModel (MVVM) architecture.
    -   **Model**: The existing C `GameHistory` and related structs.
    -   **ViewModel**: C++ `QObject` wrappers that expose the C data to QML.
    -   **View**: QML files handling presentation and user interaction.
-   **Testability**: The C++ wrappers will be testable via `QTest` or `GoogleTest`. The UI logic will be decoupled from the engine logic.
-   **Portability**: CMake build system targeting macOS, Linux, Windows, and potentially WebAssembly (Qt for WebAssembly).

## Architecture

### 1. Build System (CMake)
We will introduce a `CMakeLists.txt` at the root to manage the build.
-   **Library Target**: `magpie_core` (Static or Shared library from existing C source).
-   **App Target**: `qtpie` (Qt Quick Application linking against `magpie_core`).

### 2. Data Integration (The "Bridge")
Since MAGPIE is C and Qt is C++, we need a robust bridge.
-   **`GameHistoryModel`**: A `QAbstractListModel` or `QObject` wrapper around `struct GameHistory`.
    -   Properties: `currentScore`, `playerNames`, `boardState`.
    -   Methods: `next()`, `previous()`, `jumpTo(index)`.
    -   Signals: `boardChanged`, `historyChanged`.
-   **`BoardModel`**: A representation of the board for the QML Grid.

### 3. UI Components (QML)
-   **`MainWindow.qml`**: The main application shell.
-   **`BoardView.qml`**: A scalable, responsive grid for the board.
    -   Uses a `Repeater` or `TableView` for high performance.
    -   Animations for tiles entering/leaving.
-   **`RackView.qml`**: The player's current tiles.
-   **`HistoryPanel.qml`**: A scrollable list of moves (using `ListView`).
-   **`AnalysisDashboard.qml`**: Charts and stats (Win %, Score Margin) using `QtCharts` or custom canvas drawing.

## Implementation Steps

### Phase 1: Foundation
1.  **Setup CMake**: Create `CMakeLists.txt` to build the C code and a basic Qt app.
2.  **Basic Wrapper**: Create `GameHistoryWrapper` class that can load a game and print stats to console.
3.  **Hello World UI**: A simple QML window that displays data from `GameHistoryWrapper`.

### Phase 2: Core Gameplay Visualization
4.  **Board Rendering**: Implement `BoardModel` and `BoardView` to show the grid and tiles.
5.  **Navigation**: Implement `next`/`prev` buttons in QML connected to the C++ wrapper.
6.  **Rack & Score**: Display current rack and scores.

### Phase 3: Analysis & Polish
7.  **Move List**: Show the full history of moves with clickable items to jump to that state.
8.  **Analysis Metrics**: Integrate the "Move Analyzer" features (Win %, Expected Margin).
9.  **Styling**: Apply the "premium" look (colors, fonts, shadows) as per inspiration.

## Directory Structure
```
/
├── CMakeLists.txt       # Main build script
├── src/
│   ├── ent/             # Existing C code
│   ├── ...
│   └── qt/              # New Qt Frontend
│       ├── main.cpp     # Entry point
│       ├── models/      # C++ Wrappers (GameHistoryModel.cpp, etc.)
│       ├── views/       # QML Files (BoardView.qml, etc.)
│       └── assets/      # Fonts, Images
└── test/
    └── qt_tests/        # Unit tests for wrappers
```
