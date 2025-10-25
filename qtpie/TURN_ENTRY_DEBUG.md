# Turn Entry Widget - Complete Implementation ✅

## Summary

Turn entry widgets now display complete move information with proper formatting:
- **Move notation** (e.g., "8D FEVER") with time and rack on bottom row
- **Compact right-aligned scores** ("0" or "0 +30")
- **Cumulative scores** shown for validated and committed moves
- **Color-coded backgrounds**: Yellow (unvalidated), Green (validated), White (committed)
- **Player header scores** update after each move
- **Computer thinking delay**: 3-second non-blocking delay with yellow placeholder

## Original Problem
Turn entry widgets showed blank white rectangles for committed moves. **ONLY the bottommost/active entry showed text** - all entries above were blank.

## ✅ SOLUTION (Fixed!)

The bug was **NOT** a Qt rendering issue at all! It was a **data lifecycle bug**:

1. `setCommittedMove()` correctly set all member variables (`m_paintNotation`, `m_paintPrevScore`, etc.)
2. **Then** `clearCurrentTurnEntry()` was called, which called `clear()` on the widget
3. `clear()` erased all the data we just set
4. `paintEvent()` ran and tried to paint empty strings

### The Fix

Changed `PlayerHistoryColumn::clearCurrentTurnEntry()` from:
```cpp
// BUGGY VERSION - erases widget data!
void PlayerHistoryColumn::clearCurrentTurnEntry() {
    if (m_currentTurnEntry) {
        m_currentTurnEntry->clear();  // ❌ This erases the committed move data!
    }
}
```

To:
```cpp
// FIXED VERSION - just clears the pointer
void PlayerHistoryColumn::clearCurrentTurnEntry() {
    // Just clear the pointer - DON'T clear the widget's data!
    // The widget now contains committed move data that should be displayed.
    m_currentTurnEntry = nullptr;
}
```

### How We Found It

After dozens of failed rendering attempts, we implemented diagnostic logging using Qt's signal/slot mechanism:

1. Added `debugMessage` signal to TurnEntryWidget
2. Added `debugMessage` signal to PlayerHistoryColumn
3. Connected the signal chain: TurnEntryWidget → PlayerHistoryColumn → GameHistoryPanel
4. Added `emit debugMessage()` calls in key functions

The logs revealed:
```
TurnEntry::setCommittedMove ptr=105553168851968 notation='8D.FEVER' converted='8D FEVER' len=8
[olaugh] Entry committed and pointer cleared
TurnEntry::paintEvent ptr=105553168851968 notation='' len=0  ← DATA WAS ERASED!
```

This showed that data was being set correctly but then cleared before painting.

## What Works vs What Doesn't

### ✅ WORKS on ALL entries:
- White/yellow/green backgrounds (via stylesheet)
- Rounded corners (via `border-radius: 8px`)
- Borders (via stylesheet or QPainter)
- `fillRect()` - colored rectangles appear on all entries
- `drawLine()` - lines appear on all entries
- `drawRect()` - rectangles appear on all entries
- **Text in yellow placeholder (bottom entry)**

### ❌ FAILS on committed entries (only works on bottom):
- `drawText()` with QRect + alignment flags
- `drawText()` with absolute x,y positioning
- QLabel text (with or without stylesheets)
- QLabel text (with QPalette forcing black)
- Text with ANY font (Arial, Consolas, etc.)
- Text with ANY color (black, red, etc.)

## Critical Discovery: It's Not About Color or Stylesheets

The issue is **NOT**:
- White backgrounds specifically
- Qt stylesheets
- QLabel widgets
- Font selection
- Text color
- QPalette settings
- `Qt::WA_StyledBackground` attribute

The issue **IS**:
- **Position in VBoxLayout**: Only the bottommost widget can render text
- **Qt text rendering state**: Something about earlier widgets blocks text rendering
- **Persistent across all approaches**: QLabel, manual QPainter, everything fails the same way

## What We Tried (All Failed)

1. ❌ QLabel with black text via stylesheet
2. ❌ QLabel with QPalette forcing black text
3. ❌ QLabel with `setAutoFillBackground(true)`
4. ❌ Manual QPainter with `drawText(QRect, alignment, text)`
5. ❌ Manual QPainter with `drawText(x, y, text)` absolute positioning
6. ❌ Removing `Qt::WA_StyledBackground`
7. ❌ Using QPalette instead of stylesheets for background
8. ❌ Transparent backgrounds
9. ❌ Red text in stylesheet (to test visibility)
10. ❌ Arial font instead of Consolas
11. ❌ Explicit clipping: `painter.setClipRect(rect())` + `setClipping(true)`
12. ❌ Calling `QWidget::paintEvent(event)` first
13. ❌ Disabling antialiasing
14. ❌ Using QPainterPath for rounded rectangles
15. ❌ Setting text at different Y positions (28, 35, 60)

## The "V0 TEST" Mystery

At one point, we got "V0 TEST" and "V1 TEST" to appear on ALL white entries using:
```cpp
void TurnEntryWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);  // Draw stylesheet background first

    QPainter painter(this);
    painter.setClipRect(rect());
    painter.setClipping(true);

    QFont font("Arial", 16, QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor(0, 0, 0));
    painter.drawText(15, 35, QString("V%1 TEST").arg(m_variant));
}
```

**But when we use the same approach with actual move data (m_paintNotation, m_paintPrevScore, etc.), text only shows on the bottom entry again.**

## Current Working Code (Text Only on Bottom Entry)

```cpp
void TurnEntryWidget::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);  // CRITICAL: Call base first

    QPainter painter(this);
    painter.setClipRect(rect());
    painter.setClipping(true);

    QFont notationFont("Consolas", 19, QFont::Bold);
    painter.setFont(notationFont);
    painter.setPen(QColor(0, 0, 0));
    painter.drawText(15, 35, m_paintNotation);

    // Score, time, rack drawing...
}

void TurnEntryWidget::setCommittedMove(...)
{
    m_paintNotation = convertToStandardNotation(notation);
    // ... set other m_paint* variables

    setStyleSheet(
        "#turnEntry {"
        "  background-color: #FFFFFF;"
        "  border: 2px solid #E0E0E0;"
        "  border-radius: 8px;"
        "}"
    );

    update();  // Trigger repaint
}
```

## Theories to Explore

### Theory 1: VBoxLayout Clipping Issue
The entries are in a VBoxLayout with a stretch at the end. Maybe Qt is applying some clip region that affects text rendering but not shapes?

**Test**: Try moving entries to different positions in the layout dynamically.

### Theory 2: Widget Update/Repaint Order
Maybe earlier widgets aren't getting their text repainted properly when new entries are added below them?

**Test**: Call `update()` or `repaint()` on ALL existing entries when adding a new one.

### Theory 3: Text Rendering Context State
Maybe QPainter text rendering state is being corrupted/inherited between widgets?

**Test**: Try `painter.save()` / `painter.restore()` around text drawing. Reset all painter state explicitly.

### Theory 4: Z-Order / Stacking Issue
Maybe something transparent is being painted OVER the text in earlier entries?

**Test**: We already confirmed fillRect and drawLine work, so this seems unlikely. But could try `raise()` on widgets.

### Theory 5: Font Rendering Cache Bug
Maybe Qt's font cache is broken for widgets not at the bottom?

**Test**: Try `QFontDatabase::removeAllApplicationFonts()` and reload, or use different font for each entry.

### Theory 6: Scroll Area Clipping
The entries are in a scroll area. Maybe the scroll area's viewport is clipping text but not shapes?

**Test**: Try without scroll area. Or check viewport clip region.

## Diagnostic Commands

```bash
# Rebuild and run
make -j4 && { killall Magpie 2>/dev/null; sleep 0.5; open Magpie.app; }

# Check current paintEvent implementation
cat qtpie/turn_entry_widget.cpp | grep -A 30 "paintEvent"

# Check setCommittedMove
cat qtpie/turn_entry_widget.cpp | grep -A 20 "setCommittedMove"
```

## Files Modified
- `qtpie/turn_entry_widget.h` - Added RenderMode enum, variant parameter, paint data members
- `qtpie/turn_entry_widget.cpp` - Complete rewrite to manual painting
- `qtpie/game_history_panel.h` - Added RenderMode to PlayerHistoryColumn
- `qtpie/game_history_panel.cpp` - Pass RenderMode to TurnEntryWidget constructor

## Next Steps

1. **Investigate VBoxLayout**: Check if there's a layout or scroll area issue
2. **Force repaint on all entries**: When new entry added, call `update()` on all previous entries
3. **Try QGraphicsView**: Complete alternative to QWidget-based layout
4. **Debug paint events**: Add logging to see if paintEvent is called for all widgets
5. **Check Qt bug reports**: Search for "QLabel text not visible" or "QPainter drawText fails"

## Key Insight

~~The fact that **shapes work but text doesn't** on earlier entries strongly suggests this is a **Qt text rendering state bug**~~

**UPDATE**: This was a false lead! The real issue was that text data was being cleared after being set. The "only bottom entry shows text" pattern happened because:
- Bottom entry was the current turn (not committed yet) - data never cleared
- All entries above were committed moves - data was cleared by `clearCurrentTurnEntry()` bug

## Final Working Implementation

```cpp
// turn_entry_widget.cpp - paintEvent()
void TurnEntryWidget::paintEvent(QPaintEvent *event)
{
    // CRITICAL: Call base paintEvent FIRST - draws stylesheet background
    QWidget::paintEvent(event);

    // Now paint text on top
    QPainter painter(this);
    painter.setClipRect(rect());
    painter.setClipping(true);

    // Use Consolas font - monospaced for Scrabble
    QFont notationFont("Consolas", 19, QFont::Bold);
    painter.setFont(notationFont);
    painter.setPen(QColor(0, 0, 0));  // Black text

    // Draw move notation (e.g., "8D FEVER")
    painter.drawText(15, 35, m_paintNotation);

    // Draw scores on right side
    QFont scoreFont("Consolas", 19);
    painter.setFont(scoreFont);
    painter.drawText(width() - 140, 35, m_paintPrevScore);
    painter.drawText(width() - 70, 35, m_paintPlayScore);

    // Draw cumulative score
    if (m_showCumulative) {
        painter.setFont(scoreFont);
        painter.drawText(width() - 70, 60, m_paintCumulative);
    }
}
```

Key points:
- Call `QWidget::paintEvent(event)` first to render the stylesheet background
- Use manual QPainter with absolute positioning (`drawText(x, y, text)`)
- Store text in QString member variables that persist across repaints
- **Don't clear the widget's data after committing!**

## Lessons Learned

1. **Always implement diagnostic logging early** - We wasted many iterations trying rendering fixes when the real issue was data lifecycle
2. **Use Qt's signal/slot for logging** - `qDebug()`, `qWarning()`, and `fprintf()` were all suppressed in our build
3. **Trace data flow, not just rendering** - The bug was between `setCommittedMove()` and `paintEvent()`, not in the rendering itself
4. **"Only bottom widget works" can be a data pattern, not a rendering bug** - The position-dependent behavior was a red herring
