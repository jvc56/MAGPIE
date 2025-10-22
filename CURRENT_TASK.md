# Current Task: Debugging Drag-and-Drop Coordinate System

## Problem
Investigating drag-and-drop tile placement accuracy issues. Specifically trying to track whether tile positions in the rack (in screen coordinates) remain constant during drag operations.

## Goal
Log the absolute screen coordinates of the 'N' tile in the rack during:
1. Regular mouse movement over the rack (no buttons pressed)
2. During drag operations (while dragging the N tile or any other tile)

## Current Status

### What We've Done
1. **Added mouse tracking to RackView** (`setMouseTracking(true)` in constructor)
2. **Added N-tile coordinate logging in `mouseMoveEvent`** (rack_view.cpp:321-339)
   - Calculates N tile center position in rack
   - Converts to screen coordinates using `mapToGlobal()`
   - Emits debug message: `"N tile (index X) at screen coords: (X,Y)"`

3. **Added N-tile coordinate logging in `dragMoveEvent`** (rack_view.cpp:453-471)
   - Same calculation as mouseMoveEvent
   - Emits debug message: `"DRAG: N tile (index X) at screen coords: (X,Y)"`

### What's NOT Working
- The `mouseMoveEvent` logging is **not appearing** when moving the mouse over the rack
- Only seeing the "Mouse at..." logs during the initial mouse press before drag starts
- After that, only seeing drag-related events from BoardPanelView

### Observations from Logs
From the most recent test run:
- **N tile original position**: index 4, drawn at rack x=321, screen coords (1002, 940)
- **Mouse press position**: (1001, 939) - very close to N tile center
- **Drag starts** after moving ~5 pixels
- Once drag starts, control transfers to BoardPanelView's dragMoveEvent
- The RackView's dragMoveEvent is called briefly, then drag leaves the widget

### Why mouseMoveEvent Might Not Be Working
1. **Mouse tracking may need to be enabled on parent widgets too**
2. **Events might be getting consumed by child widgets** (the alphabetize/shuffle buttons)
3. **Qt may not deliver mouse events when cursor is outside the widget bounds**
4. **Need to check if the widget is actually receiving focus/hover**

## Next Steps

### Option 1: Add Event Filter
Install an event filter to intercept all mouse events at a higher level:
```cpp
// In constructor
installEventFilter(this);

// Override
bool RackView::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() == QEvent::MouseMove) {
        // Log N tile coords here
    }
    return QWidget::eventFilter(obj, event);
}
```

### Option 2: Use Global Event Monitoring
Track cursor position globally using QCursor::pos() in a timer:
```cpp
// In constructor
m_trackingTimer = new QTimer(this);
connect(m_trackingTimer, &QTimer::timeout, this, &RackView::logNTilePosition);
m_trackingTimer->start(100); // Every 100ms
```

### Option 3: Add Logging to paintEvent
Since paintEvent is called frequently, we could log N tile coords there (but this is inefficient).

### Option 4: Debug Event Delivery
Add logging to verify events are being delivered:
```cpp
void RackView::mouseMoveEvent(QMouseEvent *event) {
    emit debugMessage("mouseMoveEvent CALLED!");
    // ... rest of code
}
```

## Key Files
- `qtpie/rack_view.cpp` - Main rack widget implementation
- `qtpie/rack_view.h` - RackView header
- `qtpie/board_panel_view.cpp` - Handles drag events when over board area
- `qtpie/main.cpp` - MainWidget handles top-level drag preview rendering

## Test Procedure
1. Load a game with rack containing 'N' tile
2. Move mouse over the rack (should see N tile coords logging continuously)
3. Click and drag the N tile
4. Watch for N tile screen coords to stay constant at (1002, 940)
5. Drag to board and observe coordinate transformations

## Expected Behavior
The N tile's screen coordinates should remain **constant** at (1002, 940) throughout:
- Mouse hovering over rack
- Pressing down on N tile
- Dragging N tile around
- Returning to rack

If coords change unexpectedly, that indicates a widget geometry issue or coordinate system bug.

## Recent Commits
- `eeb76a3a` - "Improve drag-and-drop coordinate calculation for tile placement"
  - Switched to overlap-based square detection
  - Fixed coordinate system confusion between MainWidget/BoardView
  - Added extensive debug logging

## Branch
`qtpie-oct2025` (tracking `origin/qtpie-oct2025`)
