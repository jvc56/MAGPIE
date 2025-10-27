# Current Task: Gameplay Functionality Implementation

## Status: In Progress

## Overview
Implementing full gameplay functionality for QtPie including move submission, computer play, turn management, and game history tracking.

## Completed Features

### 1. Gameplay Buttons ✅
- **Pass Button**: Always enabled (blue) during player's turn, disabled during computer's turn
- **Exchange Button**: Always enabled (blue) during player's turn (assumes exchanges always allowed)
- **Play Button**: Only enabled (blue) when move is validated for well-formedness
- All buttons styled with blue (#6B9BD1) when enabled, grey (#D0D0D0) when disabled
- Buttons automatically disable during computer's turn

### 2. Move Submission & Computer Play ✅
- **Player Move**: Clicking Play button submits move to MAGPIE game state
- **Tile Drawing**: After playing, player's rack is automatically refilled from bag
- **Computer Move**: Automatically triggered after player's move
  - Uses `get_top_equity_move()` for best move selection
  - Plays instantly
  - Also draws tiles to refill rack
- **Score Tracking**: Correctly calculates and displays scores before/after each move

### 3. Board Display ✅
- **Committed Tiles**: Player's played tiles (e.g., FEVER) appear on board in yellow/gold
- **Computer Tiles**: Computer's tiles (e.g., I2 NUNCLES) also appear on board
- **Board Re-rendering**: Board view correctly updates by parsing CGP and rendering committed tiles
- Fixed critical bug where board was cleared instead of updated

### 4. Turn Management ✅
- **Turn Tracking**: System tracks whose turn it is (player 0 or computer player 1)
- **Timer Control**: Timer switches between players when turns change
- **Input Control**:
  - Keyboard input disabled during computer's turn
  - Keyboard input disabled when debug window has focus
  - Keyboard entry mode cleared when turn switches

### 5. Game History Panel ✅
- **Turn Commits**: When a move is played, the turn entry:
  - Background changes to white (committed state)
  - Timer freezes at the time of commitment
  - Shows final move notation, score, and rack
- **New Turn Creation**: After committing, a new turn entry is automatically created
- **Active Timer**: Only the current turn's timer ticks, previous turns are frozen
- **Move Display**: Shows both player and computer moves in their respective turn entries

## Technical Implementation

### Files Modified

#### Core Gameplay
- `board_panel_view.h/cpp`:
  - Added button state management
  - Added turn tracking (m_isPlayerTurn, m_debugWindowHasFocus)
  - Added `shouldAllowKeyboardInput()` to control input
  - Added `setPlayerTurn()` to manage turn transitions
  - Added `moveCommitted` signal
  - Emit signals for turn changes and move commits

#### MAGPIE Wrapper
- `magpie_wrapper.h/c`:
  - `magpie_play_move()`: Validates, plays move, and draws tiles to refill rack
  - `magpie_get_top_equity_move()`: Gets best move for computer
  - `magpie_get_rack()`: Gets player's rack
  - Uses `draw_to_full_rack()` after each move

#### Game History
- `game_history_panel.h/cpp`:
  - `commitTurnAndCreateNext()`: Commits current turn, freezes timer, creates new turn
  - Properly manages turn entry lifecycle

- `turn_entry_widget.h/cpp`:
  - `setCommittedMove()`: Sets white background for committed turns
  - `setValidatedMove()`: Green background for uncommitted valid moves
  - `setUnvalidatedMove()`: Default background for uncommitted invalid moves

#### Main Window
- `main.cpp`:
  - Connected `playerTurnChanged` signal to `startTimer()`
  - Connected `moveCommitted` signal to `commitTurnAndCreateNext()`
  - Event filter tracks debug window focus

#### Build System
- `run_with_monitor.sh`: Now kills existing Magpie instances before launching

## Known Issues & Next Steps

### Issues to Fix
1. None currently reported - awaiting user testing

### Potential Enhancements
1. **Pass Implementation**: Implement pass button functionality
2. **Exchange Implementation**: Implement exchange button functionality
3. **Turn Entry Background**: User mentioned turn entry should turn white after submission (✅ DONE)
4. **Exchange Validation**: Check if exchanges are actually allowed based on bag tiles remaining
5. **Game End Detection**: Detect when game is over and disable further moves
6. **Undo Functionality**: Allow undoing uncommitted moves

## Testing Notes

### Test Case: Playing FEVER at 8D
1. Place tiles F-E-V-E-R at 8D
2. Turn entry shows green background (validated)
3. Click Play button
4. Expected results:
   - ✅ FEVER appears on board in yellow/gold committed tiles
   - ✅ Turn entry turns white with frozen timer
   - ✅ Rack refills with new tiles
   - ✅ Computer plays (e.g., I2 NUNCLES)
   - ✅ Computer's move appears on board
   - ✅ Computer's turn entry also white with move shown
   - ✅ New turn entry created for player
   - ✅ New turn entry's timer is active

## Architecture Notes

### Signal Flow
```
User clicks Play
  → BoardPanelView::onPlayClicked()
  → magpie_play_move(player 0)
  → draw_to_full_rack(player 0)
  → emit moveCommitted(0, ...)
  → GameHistoryPanel::commitTurnAndCreateNext()
    → Freeze current turn (white background)
    → Create new turn entry
  → setPlayerTurn(false) // Computer's turn
  → emit playerTurnChanged(1)
  → GameHistoryPanel::startTimer(1)
  → makeComputerMove()
  → magpie_get_top_equity_move(player 1)
  → magpie_play_move(player 1)
  → draw_to_full_rack(player 1)
  → emit moveCommitted(1, ...)
  → GameHistoryPanel::commitTurnAndCreateNext()
  → setPlayerTurn(true) // Player's turn
  → emit playerTurnChanged(0)
  → GameHistoryPanel::startTimer(0)
```

### Key Design Patterns
- **Signal-Slot**: Clean separation between move logic and UI updates
- **C Wrapper**: MAGPIE's C code wrapped for C++ consumption
- **Turn Lifecycle**: Uncommitted → Validated → Committed → New Turn

## Build & Run
```bash
cd /Users/john/sources/oct21-qtpie/MAGPIE
make libmagpie.a

cd qtpie
make
bash run_with_monitor.sh
```
