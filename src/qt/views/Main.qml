import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import QtPie 1.0

ApplicationWindow {
    id: mainWindow
    width: 1280
    height: 800
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: "QtPie - MAGPIE Frontend"
    color: "#1E1E2E"
    font.family: "Clear Sans"
    
    property real saturationLevel: 0.7 // Set to 70%
    property string recentFilesJson: "[]"

    function formatTime(seconds) {
        let m = Math.floor(seconds / 60);
        let s = seconds % 60;
        return m + ":" + (s < 10 ? "0" : "") + s;
    }

    Settings {
        id: appSettings
        property alias recentFilesJson: mainWindow.recentFilesJson
    }

    function addToRecentFiles(fileUrl) {
        var urlStr = fileUrl.toString();
        var files = JSON.parse(mainWindow.recentFilesJson);
        var index = files.indexOf(urlStr);
        if (index !== -1) files.splice(index, 1);
        files.unshift(urlStr);
        if (files.length > 10) files.pop();
        mainWindow.recentFilesJson = JSON.stringify(files);
    }

    Connections {
        target: gameModel
        function onGameLoadedFromFile(url) {
            addToRecentFiles(url);
        }
    }

    GameHistoryModel {
        id: gameModel
        Component.onCompleted: {
            // Load default empty game
            loadGame("#lexicon CSW24\n#player1 Player1 PlayerOne\n#player2 Player2 PlayerTwo");
        }
    }

    menuBar: MenuBar {
        Menu {
            title: "File"
            MenuItem {
                action: openGameAction
            }
            Menu {
                title: "Recent Games"
                enabled: JSON.parse(mainWindow.recentFilesJson).length > 0
                
                Repeater {
                    model: JSON.parse(mainWindow.recentFilesJson)
                    MenuItem {
                        text: decodeURIComponent(modelData.toString().split('/').pop())
                        onTriggered: gameModel.loadGameFromFile(modelData)
                    }
                }
                
                MenuSeparator {
                    visible: JSON.parse(mainWindow.recentFilesJson).length > 0
                }
                
                MenuItem {
                    text: "Clear Recent Games"
                    enabled: JSON.parse(mainWindow.recentFilesJson).length > 0
                    onTriggered: mainWindow.recentFilesJson = "[]"
                }
            }
            MenuSeparator {}
            MenuItem {
                text: "Quit"
                onTriggered: Qt.quit()
            }
        }
    }

    Action {
        id: openGameAction
        text: "Open Game..."
        shortcut: StandardKey.Open
        onTriggered: {
            console.log("Open Game triggered")
            gameModel.openGameDialog()
        }
    }

    Item {
        id: mainContent
        anchors.fill: parent
        visible: gameModel.gameMode !== GameHistoryModel.SetupMode

        RowLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 15

            // Left Column: Board, Rack, Controls
            Item {
                id: leftColumn
            Layout.fillHeight: true
            Layout.preferredWidth: gameModel.gameMode === GameHistoryModel.PlayMode ? parent.width * 0.55 : parent.width * 0.32

            // Cell size: fit the largest square board in available space
            // In Play mode: header(80) + margin(8) + board + margin(10) + rack(~50) + controls(50) + padding
            // In Review mode: board + margin(10) + rack(~50) + tracking + controls(50) + padding
            property int playModeVerticalSpace: height - 220
            property int reviewModeVerticalSpace: height - 200
            property int availableVertical: gameModel.gameMode === GameHistoryModel.PlayMode ? playModeVerticalSpace : reviewModeVerticalSpace
            // In play mode, divide by 16 to reserve one extra row for the cursor past the board edge
            property int verticalDivisor: gameModel.gameMode === GameHistoryModel.PlayMode ? 16 : 15
            property int computedCellSize: Math.max(10, Math.min(Math.floor((width - 12) / 15), Math.floor((availableVertical - 12) / verticalDivisor)))
            property bool exchangeMode: false

            Item {
                id: keyboardEntryController
                objectName: "keyboardEntryController"
                anchors.fill: parent
                focus: gameModel.gameMode === GameHistoryModel.PlayMode
                
                Keys.onPressed: (event) => {
                    if (gameModel.gameMode !== GameHistoryModel.PlayMode) return;

                    if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        if (leftColumn.exchangeMode) {
                            leftColumn.confirmExchange();
                        } else {
                            leftColumn.commitMove();
                        }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Escape) {
                        if (leftColumn.exchangeMode) {
                            leftColumn.cancelExchangeMode();
                        } else {
                            uncommittedTiles.clear();
                        }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Minus) {
                        if (!leftColumn.exchangeMode) {
                            leftColumn.enterExchangeMode();
                        }
                        event.accepted = true;
                    } else if (leftColumn.exchangeMode) {
                        // In exchange mode, ignore board navigation and letter keys
                        return;
                    } else if (event.key === Qt.Key_Right) {
                        if (keyboardCursor.col < 14) { keyboardCursor.col++; leftColumn.relocateCursor(0); }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Left) {
                        // Move left, skip occupied squares going left
                        if (keyboardCursor.col > 0) {
                            keyboardCursor.col--;
                            while (keyboardCursor.col > 0) {
                                let idx = keyboardCursor.row * 15 + keyboardCursor.col;
                                let sq = gameModel.board[idx];
                                if (!sq || sq.letter === "") break;
                                keyboardCursor.col--;
                            }
                        }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Down) {
                        if (keyboardCursor.row < 14) { keyboardCursor.row++; leftColumn.relocateCursor(1); }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Up) {
                        // Move up, skip occupied squares going up
                        if (keyboardCursor.row > 0) {
                            keyboardCursor.row--;
                            while (keyboardCursor.row > 0) {
                                let idx = keyboardCursor.row * 15 + keyboardCursor.col;
                                let sq = gameModel.board[idx];
                                if (!sq || sq.letter === "") break;
                                keyboardCursor.row--;
                            }
                        }
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Space) {
                        keyboardCursor.dir = 1 - keyboardCursor.dir;
                        event.accepted = true;
                    } else if (event.key >= Qt.Key_A && event.key <= Qt.Key_Z) {
                        leftColumn.addTile(String.fromCharCode(event.key), event.modifiers & Qt.ShiftModifier);
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Backspace) {
                        leftColumn.removeLastTile();
                        event.accepted = true;
                    }
                }
            }

            ListModel {
                id: uncommittedTiles
            }

            // Pending blank placement from drag-drop
            property int pendingBlankRow: -1
            property int pendingBlankCol: -1

            Popup {
                id: blankLetterPicker
                modal: true
                anchors.centerIn: parent
                width: 280
                height: 200
                padding: 12

                background: Rectangle {
                    color: "#313244"
                    radius: 12
                    border.color: "#585B70"
                    border.width: 1
                }

                contentItem: Column {
                    spacing: 8

                    Text {
                        text: "Choose a letter for the blank"
                        color: "#CDD6F4"
                        font.bold: true
                        font.pixelSize: 14
                        anchors.horizontalCenter: parent.horizontalCenter
                    }

                    Grid {
                        columns: 7
                        spacing: 4
                        anchors.horizontalCenter: parent.horizontalCenter

                        Repeater {
                            model: ["A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z"]
                            delegate: Rectangle {
                                width: 32
                                height: 32
                                radius: 6
                                color: blankLetterMouseArea.containsMouse ? "#585B70" : "#45475A"

                                Text {
                                    anchors.centerIn: parent
                                    text: modelData
                                    color: "#CDD6F4"
                                    font.bold: true
                                    font.pixelSize: 14
                                }

                                MouseArea {
                                    id: blankLetterMouseArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: {
                                        let r = leftColumn.pendingBlankRow;
                                        let c = leftColumn.pendingBlankCol;
                                        blankLetterPicker.close();
                                        // Pass lowercase to signal explicit blank
                                        leftColumn.addTileAtPosition(modelData.toLowerCase(), r, c);
                                        keyboardEntryController.forceActiveFocus();
                                    }
                                }
                            }
                        }
                    }
                }

                onClosed: {
                    keyboardEntryController.forceActiveFocus();
                }
            }

            // Ensure cursor is on an empty square. If occupied, advance
            // in the given direction until an empty square is found or
            // the cursor goes past the board edge (position 15).
            function relocateCursor(dir) {
                while (keyboardCursor.row < 15 && keyboardCursor.col < 15) {
                    let idx = keyboardCursor.row * 15 + keyboardCursor.col;
                    let sq = gameModel.board[idx];
                    if (!sq || sq.letter === "") return; // empty, we're good
                    // Also check uncommitted tiles
                    let hasUncommitted = false;
                    for (let i = 0; i < uncommittedTiles.count; i++) {
                        let t = uncommittedTiles.get(i);
                        if (t.row === keyboardCursor.row && t.col === keyboardCursor.col) {
                            hasUncommitted = true;
                            break;
                        }
                    }
                    if (hasUncommitted) return;
                    // Occupied by board tile, advance
                    if (dir === 0) keyboardCursor.col++;
                    else keyboardCursor.row++;
                }
                // Past the edge — that's OK, cursor shows off-board
            }

            function addTile(letter, forceBlank) {
                // Can't place tiles past the board edge
                if (keyboardCursor.row >= 15 || keyboardCursor.col >= 15) return;

                let rackStr = gameModel.currentRack;
                let upperLetter = letter.toUpperCase();

                // Count natural tiles of this letter in rack
                let naturalInRack = 0;
                let blanksInRack = 0;
                for (let i = 0; i < rackStr.length; i++) {
                    if (rackStr[i] === upperLetter) naturalInRack++;
                    else if (rackStr[i] === '?') blanksInRack++;
                }

                // Count used natural and blank tiles in uncommittedTiles
                let naturalUsed = 0;
                let blanksUsed = 0;
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    let t = uncommittedTiles.get(i);
                    if (t.isBlank) {
                        blanksUsed++;
                    } else if (t.letter === upperLetter) {
                        naturalUsed++;
                    }
                }

                let useBlank = false;
                if (forceBlank) {
                    if (blanksUsed >= blanksInRack) {
                        console.log("No blank tiles available");
                        return;
                    }
                    useBlank = true;
                } else {
                    // Prefer natural tile, fall back to blank
                    if (naturalUsed < naturalInRack) {
                        useBlank = false;
                    } else if (blanksUsed < blanksInRack) {
                        useBlank = true;
                    } else {
                        console.log("Letter " + upperLetter + " not available in rack");
                        return;
                    }
                }

                uncommittedTiles.append({
                    row: keyboardCursor.row,
                    col: keyboardCursor.col,
                    letter: upperLetter,
                    isBlank: useBlank
                });
                advanceCursor();
            }

            // Add a tile at a specific board position (for drag-drop)
            function addTileAtPosition(letter, row, col) {
                let rackStr = gameModel.currentRack;
                let upperLetter = letter.toUpperCase();

                // Check if position already has an uncommitted tile
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    let tile = uncommittedTiles.get(i);
                    if (tile.row === row && tile.col === col) {
                        console.log("Position already has an uncommitted tile");
                        return false;
                    }
                }

                // Check if position has a permanent tile on board
                let boardIndex = row * 15 + col;
                if (boardIndex >= 0 && boardIndex < gameModel.board.length) {
                    let square = gameModel.board[boardIndex];
                    if (square && square.letter !== "") {
                        console.log("Position already has a tile on board");
                        return false;
                    }
                }

                // Count natural tiles and blanks in rack
                let naturalInRack = 0;
                let blanksInRack = 0;
                for (let i = 0; i < rackStr.length; i++) {
                    if (rackStr[i] === upperLetter) naturalInRack++;
                    else if (rackStr[i] === '?') blanksInRack++;
                }

                // Count used natural and blank tiles in uncommittedTiles
                let naturalUsed = 0;
                let blanksUsed = 0;
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    let t = uncommittedTiles.get(i);
                    if (t.isBlank) {
                        blanksUsed++;
                    } else if (t.letter === upperLetter) {
                        naturalUsed++;
                    }
                }

                let useBlank = false;
                // Lowercase letter = explicit blank designation from letter picker
                if (letter >= 'a' && letter <= 'z') {
                    if (blanksUsed >= blanksInRack) {
                        console.log("No blank tiles available");
                        return false;
                    }
                    useBlank = true;
                } else {
                    // Prefer natural tile, fall back to blank
                    if (naturalUsed < naturalInRack) {
                        useBlank = false;
                    } else if (blanksUsed < blanksInRack) {
                        useBlank = true;
                    } else {
                        console.log("Letter " + upperLetter + " not available in rack");
                        return false;
                    }
                }

                uncommittedTiles.append({
                    row: row,
                    col: col,
                    letter: upperLetter,
                    isBlank: useBlank
                });

                // Move cursor to dropped position
                keyboardCursor.row = row;
                keyboardCursor.col = col;

                return true;
            }

            function removeLastTile() {
                if (uncommittedTiles.count > 0) {
                    let removed = uncommittedTiles.get(uncommittedTiles.count - 1);
                    keyboardCursor.row = removed.row;
                    keyboardCursor.col = removed.col;
                    uncommittedTiles.remove(uncommittedTiles.count - 1);
                } else {
                    // No tiles to remove — just move cursor back
                    if (keyboardCursor.dir === 0) {
                        if (keyboardCursor.col > 0) keyboardCursor.col--;
                    } else {
                        if (keyboardCursor.row > 0) keyboardCursor.row--;
                    }
                }
            }

            function advanceCursor() {
                // Advance one step, then skip over existing board tiles.
                // Never wrap around — stop at position 15 (past the edge).
                while (true) {
                    if (keyboardCursor.dir === 0) {
                        keyboardCursor.col++;
                    } else {
                        keyboardCursor.row++;
                    }
                    // Past the edge — stop here (cursor shows arrow past board)
                    if (keyboardCursor.col >= 15 || keyboardCursor.row >= 15) break;
                    let idx = keyboardCursor.row * 15 + keyboardCursor.col;
                    let sq = gameModel.board[idx];
                    if (!sq || sq.letter === "") break; // empty square, stop here
                    // occupied square, keep advancing
                }
            }

            function commitMove() {
                if (uncommittedTiles.count === 0) return;

                // Sort uncommitted tiles by position along the play direction
                let tiles = [];
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    tiles.push(uncommittedTiles.get(i));
                }
                let isHoriz = (keyboardCursor.dir === 0);
                tiles.sort(function(a, b) {
                    return isHoriz ? (a.col - b.col) : (a.row - b.row);
                });

                let first = tiles[0];
                let last = tiles[tiles.length - 1];
                let colChar = String.fromCharCode(97 + first.col);
                let rowStr = (first.row + 1).toString();

                // Walk the full span from first to last tile, including
                // existing board tiles as playthroughs and checking for
                // trailing board tiles that extend the word.
                let r = first.row;
                let c = first.col;
                let ri = isHoriz ? 0 : 1;
                let ci = isHoriz ? 1 : 0;
                let endR = last.row;
                let endC = last.col;
                let word = "";
                let tileIdx = 0;

                while (true) {
                    // Check if there's an uncommitted tile at (r, c)
                    let placed = null;
                    if (tileIdx < tiles.length) {
                        let t = tiles[tileIdx];
                        if (t.row === r && t.col === c) {
                            placed = t;
                            tileIdx++;
                        }
                    }

                    if (placed) {
                        word += placed.isBlank ? placed.letter.toLowerCase() : placed.letter;
                    } else {
                        // Must be an existing board tile (playthrough)
                        let idx = r * 15 + c;
                        let sq = gameModel.board[idx];
                        if (sq && sq.letter !== "") {
                            word += sq.letter;
                        }
                    }

                    // Check if we've passed the last placed tile
                    if (r === endR && c === endC) {
                        // Continue to include trailing board tiles
                        let nr = r + ri;
                        let nc = c + ci;
                        if (nr >= 0 && nr < 15 && nc >= 0 && nc < 15) {
                            let nIdx = nr * 15 + nc;
                            let nSq = gameModel.board[nIdx];
                            if (nSq && nSq.letter !== "") {
                                endR = nr;
                                endC = nc;
                            }
                        }
                    }

                    if (r === endR && c === endC) break;
                    r += ri;
                    c += ci;
                }

                // Also check for leading board tiles before the first placed tile
                let leadR = first.row - ri;
                let leadC = first.col - ci;
                let leading = "";
                while (leadR >= 0 && leadR < 15 && leadC >= 0 && leadC < 15) {
                    let idx = leadR * 15 + leadC;
                    let sq = gameModel.board[idx];
                    if (sq && sq.letter !== "") {
                        leading = sq.letter + leading;
                        leadR -= ri;
                        leadC -= ci;
                    } else {
                        break;
                    }
                }
                if (leading.length > 0) {
                    word = leading + word;
                    // Update start position to the first leading tile
                    let startR = first.row - ri * leading.length;
                    let startC = first.col - ci * leading.length;
                    colChar = String.fromCharCode(97 + startC);
                    rowStr = (startR + 1).toString();
                }

                let pos = isHoriz ? (rowStr + colChar) : (colChar + rowStr);
                let notation = pos + "." + word;

                let prevIndex = gameModel.currentHistoryIndex;
                gameModel.submitMove(notation);
                if (gameModel.currentHistoryIndex !== prevIndex) {
                    uncommittedTiles.clear();
                    humanMoveCompleted();
                }
            }

            function enterExchangeMode() {
                if (gameModel.bagCount < 7) {
                    gameModel.pass();
                    humanMoveCompleted();
                    return;
                }
                uncommittedTiles.clear();
                exchangeMode = true;
                rackView.clearSelection();
            }

            function cancelExchangeMode() {
                exchangeMode = false;
                rackView.clearSelection();
            }

            function confirmExchange() {
                if (!exchangeMode) return;
                var tiles = rackView.getSelectedTiles();
                if (tiles.length === 0) {
                    cancelExchangeMode();
                    return;
                }
                gameModel.exchange(tiles);
                cancelExchangeMode();
                humanMoveCompleted();
            }

            function humanMoveCompleted() {
                aiResponseTimer.start();
            }

            Timer {
                id: aiResponseTimer
                interval: 100
                onTriggered: {
                    var move = gameModel.getComputerMove();
                    if (move && move.length > 0) {
                        gameModel.submitMove(move);
                    }
                    // After computer plays, ensure cursor isn't on an occupied square
                    leftColumn.relocateCursor(keyboardCursor.dir);
                }
            }

            // Player Info Header (Play Mode)
            Rectangle {
                id: gameHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 80
                visible: false
                color: "#313244"
                radius: 10
            }

            // Board Area (anchored to header or top)
            // In play mode, extra cellSize height gives room for cursor past the board edge
            Item {
                id: boardArea
                anchors.top: gameHeader.visible ? gameHeader.bottom : parent.top
                anchors.topMargin: gameHeader.visible ? 8 : 0
                anchors.left: parent.left
                anchors.right: parent.right
                height: boardBackground.height + (gameModel.gameMode === GameHistoryModel.PlayMode ? gridContainer.cellSize : 0)

                // Background for Board (tight fit)
                Rectangle {
                    id: boardBackground
                    color: "#313244"
                    radius: 8
                    anchors.centerIn: parent
                    width: gridContainer.width + 12
                    height: gridContainer.height + 12
                    
                    Item {
                        id: gridContainer
                        objectName: "gridContainer"
                        width: leftColumn.computedCellSize * 15
                        height: width
                        anchors.centerIn: parent

                        property int cellSize: leftColumn.computedCellSize
                        property var bonusStyles: {
                            // Force dependency on saturationLevel so this re-evaluates
                            var s = saturationLevel;
                            return {
                                "1,3": { color: "#D20F39", text: "3W" }, // Strong Red
                                "1,2": { color: "#EA76CB", text: "2W" }, // Distinct Pink
                                "3,1": { color: "#1E66F5", text: "3L" }, // Strong Blue
                                "2,1": { color: "#04A5E5", text: "2L" }, // Light Blue
                                "1,1": { color: "#313244", text: "" }
                            };
                        }

                        // DropArea for tiles dragged from rack
                        DropArea {
                            id: boardDropArea
                            objectName: "boardDropArea"
                            anchors.fill: parent
                            keys: ["rackTile"]
                            enabled: gameModel.gameMode === GameHistoryModel.PlayMode

                            onEntered: (drag) => {
                                drag.accept();
                            }

                            onDropped: (drop) => {
                                let row = Math.floor(drop.y / gridContainer.cellSize);
                                let col = Math.floor(drop.x / gridContainer.cellSize);
                                if (row >= 0 && row < 15 && col >= 0 && col < 15) {
                                    let letter = drop.source ? drop.source.charStr : "";
                                    if (letter && letter !== " ") {
                                        if (letter === "?") {
                                            // Open letter picker for blank tile
                                            leftColumn.pendingBlankRow = row;
                                            leftColumn.pendingBlankCol = col;
                                            blankLetterPicker.open();
                                        } else {
                                            leftColumn.addTileAtPosition(letter, row, col);
                                            keyboardEntryController.forceActiveFocus();
                                        }
                                    }
                                    drop.accept();
                                }
                            }
                        }

                        // Keyboard Cursor click handler
                        MouseArea {
                            anchors.fill: parent
                            enabled: gameModel.gameMode === GameHistoryModel.PlayMode
                            onPressed: (mouse) => {
                                let r = Math.floor(mouse.y / gridContainer.cellSize);
                                let c = Math.floor(mouse.x / gridContainer.cellSize);
                                if (r >= 0 && r < 15 && c >= 0 && c < 15) {
                                    if (keyboardCursor.row === r && keyboardCursor.col === c) {
                                        // Click same square: toggle direction
                                        keyboardCursor.dir = 1 - keyboardCursor.dir;
                                    } else {
                                        keyboardCursor.row = r;
                                        keyboardCursor.col = c;
                                        // Default to horizontal, switch to vertical if no room right
                                        let hasRoomRight = false;
                                        for (let cc = c; cc < 15; cc++) {
                                            let idx = r * 15 + cc;
                                            let sq = gameModel.board[idx];
                                            if (!sq || sq.letter === "") {
                                                hasRoomRight = true;
                                                break;
                                            }
                                        }
                                        keyboardCursor.dir = hasRoomRight ? 0 : 1;
                                    }
                                    // Skip to next empty square if clicked on an occupied one
                                    leftColumn.relocateCursor(keyboardCursor.dir);
                                    keyboardEntryController.forceActiveFocus();
                                }
                            }
                        }

                        Item {
                            id: keyboardCursor
                            objectName: "keyboardCursor"
                            visible: gameModel.gameMode === GameHistoryModel.PlayMode
                            property int row: 7
                            property int col: 7
                            property int dir: 0 // 0=H, 1=V
                            z: 10

                            Rectangle {
                                width: gridContainer.cellSize
                                height: gridContainer.cellSize
                                x: keyboardCursor.col * gridContainer.cellSize
                                y: keyboardCursor.row * gridContainer.cellSize
                                color: "#B0000000"
                                border.color: "#A6E3A1"
                                border.width: 3
                                radius: 4

                                Text {
                                    anchors.centerIn: parent
                                    text: keyboardCursor.dir === 0 ? "▶" : "▼"
                                    color: "#A6E3A1"
                                    font.bold: true
                                    font.pixelSize: parent.height * 0.4
                                }
                            }
                        }

                        // Uncommitted Tiles Layer
                        Repeater {
                            model: uncommittedTiles
                            delegate: Rectangle {
                                id: uncommittedTile
                                width: gridContainer.cellSize - 2
                                height: gridContainer.cellSize - 2
                                x: model.col * gridContainer.cellSize + 1
                                y: model.row * gridContainer.cellSize + 1
                                color: "#F9E2AF"
                                radius: gridContainer.cellSize * 0.1
                                z: 5

                                property string letter: model.letter
                                property bool isBlank: model.isBlank

                                // Letter
                                Text {
                                    id: uncommittedLetterText
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: parent.height * (uncommittedTile.isBlank ? 0.45 : 0.42) - height / 2
                                    text: uncommittedTile.letter
                                    font.family: "Clear Sans"
                                    font.pixelSize: gridContainer.cellSize * 0.5
                                    font.weight: Font.Black
                                    color: "#1E1E2E"
                                }

                                // Blank designation box
                                Rectangle {
                                    width: parent.width * 0.6
                                    height: width
                                    anchors.centerIn: uncommittedLetterText
                                    color: "transparent"
                                    border.color: "#1E1E2E"
                                    border.width: 1.5
                                    visible: uncommittedTile.isBlank
                                }

                                // Score subscript (hidden for blanks)
                                Text {
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.rightMargin: text.length > 1 ? gridContainer.cellSize * 0.05 : gridContainer.cellSize * 0.12
                                    anchors.bottomMargin: gridContainer.cellSize * 0.05
                                    font.family: "Clear Sans"
                                    font.pixelSize: text.length > 1 ? gridContainer.cellSize * 0.22 : gridContainer.cellSize * 0.25
                                    color: "#1E1E2E"
                                    visible: !uncommittedTile.isBlank
                                    text: {
                                        var l = uncommittedTile.letter.toUpperCase();
                                        var scores = {
                                            'A':1, 'B':3, 'C':3, 'D':2, 'E':1, 'F':4, 'G':2, 'H':4, 'I':1, 'J':8, 'K':5, 'L':1, 'M':3,
                                            'N':1, 'O':1, 'P':3, 'Q':10, 'R':1, 'S':1, 'T':1, 'U':1, 'V':4, 'W':4, 'X':8, 'Y':4, 'Z':10
                                        };
                                        return scores[l] || "0";
                                    }
                                }
                            }
                        }

                        Canvas {
                            id: boardCanvas
                            anchors.fill: parent
                            
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.save();
                                ctx.clearRect(0, 0, width, height);

                                for (var i = 0; i < gameModel.board.length; i++) {
                                    var modelData = gameModel.board[i];
                                    var r = Math.floor(i / 15);
                                    var c = i % 15;
                                    var x = c * gridContainer.cellSize;
                                    var y = r * gridContainer.cellSize;
                                    var hasLetter = modelData.letter !== "";

                                    if (!hasLetter) {
                                        // Draw bonus square (Canvas only handles empty squares now)
                                        var style = gridContainer.bonusStyles[modelData.letterMultiplier + "," + modelData.wordMultiplier] || gridContainer.bonusStyles["1,1"];
                                        ctx.fillStyle = style.color;

                                        var tileX = x + 1;
                                        var tileY = y + 1;
                                        var tileW = gridContainer.cellSize - 2;
                                        var tileH = gridContainer.cellSize - 2;
                                        var radius = 6;
                                        ctx.beginPath();
                                        ctx.moveTo(tileX + radius, tileY);
                                        ctx.arcTo(tileX + tileW, tileY, tileX + tileW, tileY + tileH, radius);
                                        ctx.arcTo(tileX + tileW, tileY + tileH, tileX, tileY + tileH, radius);
                                        ctx.arcTo(tileX, tileY + tileH, tileX, tileY, radius);
                                        ctx.arcTo(tileX, tileY, tileX + tileW, tileY, radius);
                                        ctx.closePath();
                                        ctx.fill();

                                        if (style.text !== "") {
                                            ctx.font = "bold " + (gridContainer.cellSize * 0.35) + "px 'Clear Sans'";
                                            ctx.fillStyle = style.textColor;
                                            ctx.textAlign = "center";
                                            ctx.textBaseline = "middle";
                                            ctx.globalAlpha = 0.75;
                                            ctx.fillText(style.text, x + gridContainer.cellSize / 2, y + gridContainer.cellSize / 2);
                                            ctx.globalAlpha = 1.0;
                                        }
                                    }
                                }
                                
                                // Main grid lines (draw after bonus squares)
                                ctx.strokeStyle = "#1E1E2E";
                                ctx.lineWidth = 1;
                                for (var j = 1; j < 15; j++) {
                                    ctx.beginPath();
                                    ctx.moveTo(j * gridContainer.cellSize, 0);
                                    ctx.lineTo(j * gridContainer.cellSize, height);
                                    ctx.stroke();
                                    ctx.beginPath();
                                    ctx.moveTo(0, j * gridContainer.cellSize);
                                    ctx.lineTo(width, j * gridContainer.cellSize);
                                    ctx.stroke();
                                }

                                ctx.restore();
                            }
                            
                            // Request a repaint whenever the size changes or the board model is reset
                            onWidthChanged: requestPaint()
                            onHeightChanged: requestPaint()
                            Connections {
                                target: gameModel
                                function onBoardChanged() { boardCanvas.requestPaint(); }
                            }
                        }

                        // Board Tiles Layer — uses same QML elements as uncommitted tiles
                        // for pixel-perfect letter alignment
                        Repeater {
                            model: gameModel.board
                            delegate: Rectangle {
                                visible: modelData.letter !== ""
                                width: gridContainer.cellSize - 2
                                height: gridContainer.cellSize - 2
                                x: (index % 15) * gridContainer.cellSize + 1
                                y: Math.floor(index / 15) * gridContainer.cellSize + 1
                                color: modelData.isLastMove ? "#FF9933" : "#F9E2AF"
                                radius: gridContainer.cellSize * 0.1
                                z: 3

                                // Letter
                                Text {
                                    id: boardLetterText
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    y: parent.height * (modelData.isBlank ? 0.45 : 0.42) - height / 2
                                    text: modelData.letter
                                    font.family: "Clear Sans"
                                    font.pixelSize: gridContainer.cellSize * 0.5
                                    font.weight: Font.Black
                                    color: "#1E1E2E"
                                }

                                // Blank designation box
                                Rectangle {
                                    width: parent.width * 0.6
                                    height: width
                                    anchors.centerIn: boardLetterText
                                    color: "transparent"
                                    border.color: "#1E1E2E"
                                    border.width: 1.5
                                    visible: modelData.isBlank
                                }

                                // Score subscript (hidden for blanks)
                                Text {
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.rightMargin: text.length > 1 ? gridContainer.cellSize * 0.05 : gridContainer.cellSize * 0.12
                                    anchors.bottomMargin: gridContainer.cellSize * 0.05
                                    font.family: "Clear Sans"
                                    font.pixelSize: text.length > 1 ? gridContainer.cellSize * 0.22 : gridContainer.cellSize * 0.25
                                    color: "#1E1E2E"
                                    visible: !modelData.isBlank && modelData.score > 0
                                    text: modelData.score > 0 ? modelData.score.toString() : ""
                                }
                            }
                        }
                    }
                }
            }

            // Rack (below board)
            RackView {
                id: rackView
                anchors.top: boardArea.bottom
                anchors.topMargin: 10
                anchors.horizontalCenter: parent.horizontalCenter
                rack: gameModel.currentRack
                tileSize: leftColumn.computedCellSize
                exchangeMode: leftColumn.exchangeMode
            }

            // Controls (anchored to bottom)
            Item {
                id: controlsArea
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 50

                // Review Mode Controls (Existing)
                RowLayout {
                    id: reviewControls
                    anchors.centerIn: parent
                    visible: gameModel.gameMode === GameHistoryModel.ReviewMode
                    spacing: 15
                    
                    // Go to First Button
                    Button {
                        id: goToFirstButton
                        Layout.preferredWidth: leftColumn.computedCellSize
                        Layout.preferredHeight: leftColumn.computedCellSize
                        enabled: gameModel.currentHistoryIndex > 0
                        onClicked: gameModel.jumpToHistoryIndex(0)
                        background: Rectangle {
                            color: parent.down ? "#45475A" : (parent.hovered ? "#585B70" : "#313244")
                            radius: width / 2
                            opacity: enabled ? 1.0 : 0.5
                        }
                        contentItem: Text { text: "«"; color: "white"; font.pixelSize: 20; horizontalAlignment: Text.AlignHCenter }
                    }

                    Button {
                        id: previousButton
                        Layout.preferredWidth: leftColumn.computedCellSize
                        Layout.preferredHeight: leftColumn.computedCellSize
                        enabled: gameModel.currentHistoryIndex > 0
                        onClicked: gameModel.previous()
                        background: Rectangle {
                            color: parent.down ? "#45475A" : (parent.hovered ? "#585B70" : "#313244")
                            radius: width / 2
                            opacity: enabled ? 1.0 : 0.5
                        }
                        contentItem: Text { text: "‹"; color: "white"; font.pixelSize: 20; horizontalAlignment: Text.AlignHCenter }
                    }

                    Text {
                        text: "Turn " + (gameModel.currentHistoryIndex + 1) + " / " + gameModel.totalHistoryItems
                        color: "#CDD6F4"
                        font.bold: true
                    }

                    Button {
                        id: nextButton
                        Layout.preferredWidth: leftColumn.computedCellSize
                        Layout.preferredHeight: leftColumn.computedCellSize
                        enabled: gameModel.currentHistoryIndex < gameModel.totalHistoryItems - 1
                        onClicked: gameModel.next()
                        background: Rectangle {
                            color: parent.down ? "#45475A" : (parent.hovered ? "#585B70" : "#313244")
                            radius: width / 2
                            opacity: enabled ? 1.0 : 0.5
                        }
                        contentItem: Text { text: "›"; color: "white"; font.pixelSize: 20; horizontalAlignment: Text.AlignHCenter }
                    }

                    Button {
                        id: goToLastButton
                        Layout.preferredWidth: leftColumn.computedCellSize
                        Layout.preferredHeight: leftColumn.computedCellSize
                        enabled: gameModel.currentHistoryIndex < gameModel.totalHistoryItems - 1
                        onClicked: gameModel.jumpToHistoryIndex(gameModel.totalHistoryItems - 1)
                        background: Rectangle {
                            color: parent.down ? "#45475A" : (parent.hovered ? "#585B70" : "#313244")
                            radius: width / 2
                            opacity: enabled ? 1.0 : 0.5
                        }
                        contentItem: Text { text: "»"; color: "white"; font.pixelSize: 20; horizontalAlignment: Text.AlignHCenter }
                    }
                }

                // Play Mode Controls
                RowLayout {
                    id: playControls
                    anchors.centerIn: parent
                    visible: gameModel.gameMode === GameHistoryModel.PlayMode
                    spacing: 15

                    // EXCHANGE / CANCEL / PASS
                    Button {
                        text: leftColumn.exchangeMode ? "CANCEL" : (gameModel.bagCount >= 7 ? "EXCHANGE" : "PASS")
                        onClicked: {
                            if (leftColumn.exchangeMode) {
                                leftColumn.cancelExchangeMode();
                            } else if (gameModel.bagCount >= 7) {
                                leftColumn.enterExchangeMode();
                            } else {
                                gameModel.pass();
                                leftColumn.humanMoveCompleted();
                            }
                        }
                        contentItem: Text {
                            text: parent.text; color: "white"; font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle {
                            implicitWidth: 100; implicitHeight: 40; radius: 8
                            color: leftColumn.exchangeMode ? "#F38BA8" : (gameModel.bagCount >= 7 ? "#FAB387" : "#F38BA8")
                        }
                    }

                    // PLAY / CONFIRM
                    Button {
                        text: leftColumn.exchangeMode ? "CONFIRM" : "PLAY"
                        onClicked: {
                            if (leftColumn.exchangeMode) {
                                leftColumn.confirmExchange();
                            } else {
                                leftColumn.commitMove();
                            }
                        }
                        contentItem: Text {
                            text: parent.text
                            color: leftColumn.exchangeMode ? "white" : "#1E1E2E"
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                        background: Rectangle {
                            implicitWidth: 100; implicitHeight: 40; radius: 8
                            color: "#A6E3A1"
                        }
                    }
                }
            }

            // Tile Tracking (fills space between rack and controls) - Review Mode only
            Rectangle {
                id: tileTrackingArea
                visible: gameModel.gameMode === GameHistoryModel.ReviewMode
                anchors.top: rackView.bottom
                anchors.topMargin: 10
                anchors.bottom: controlsArea.top
                anchors.bottomMargin: 10
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: (parent.width - boardBackground.width) / 2
                anchors.rightMargin: (parent.width - boardBackground.width) / 2
                color: "#313244"
                radius: 10
                clip: true

                // Header (Bag/Unseen counts)
                Row {
                    id: bagUnseenHeader
                    height: 20
                    spacing: 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 8

                    Text {
                        text: gameModel.bagCount + " IN BAG"
                        color: "#CDD6F4"
                        font.pixelSize: 10
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        width: parent.width / 2
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        text: (gameModel.vowelCount + gameModel.consonantCount + gameModel.blankCount) + " UNSEEN"
                        color: "#CDD6F4"
                        font.pixelSize: 10
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        width: parent.width / 2
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                // Tiles Body (fills remaining space)
                Rectangle {
                    id: tilesBody
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: bagUnseenHeader.bottom
                    anchors.bottom: tileCountsFooter.top
                    anchors.leftMargin: 5
                    anchors.rightMargin: 5
                    anchors.topMargin: 4
                    anchors.bottomMargin: 4
                    color: "transparent"
                    clip: true

                    Text {
                        text: gameModel.unseenTiles
                        color: "#FFFFFF"
                        font.family: "Consolas"
                        // Dynamic size based on cell size, with sane limits
                        font.pixelSize: Math.max(14, Math.min(36, leftColumn.computedCellSize * 0.8))
                        font.bold: false
                        wrapMode: Text.Wrap
                        anchors.fill: parent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignTop
                    }
                }

                // Footer (Vowels/Consonants)
                Row {
                    id: tileCountsFooter
                    height: 20
                    spacing: 0
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 0

                    Text {
                        text: gameModel.vowelCount + " VOWELS"
                        color: "#CDD6F4"
                        font.pixelSize: 10
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        width: parent.width / 2
                        verticalAlignment: Text.AlignVCenter
                    }

                    Text {
                        text: gameModel.consonantCount + " CONSONANTS"
                        color: "#CDD6F4"
                        font.pixelSize: 10
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        width: parent.width / 2
                        verticalAlignment: Text.AlignVCenter
            }
            }
            }
            }


        // Middle Column: Status & History (Review Mode only)
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.34
            spacing: 20
            visible: gameModel.gameMode === GameHistoryModel.ReviewMode

            // Scoreboard
            Rectangle {
                Layout.fillWidth: true
                height: 120
                color: "#313244"
                radius: 10

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 10

                    // Player 1
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: gameModel.playerOnTurnIndex === 0 ? "#45475A" : "transparent"

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 0

                            Text {
                                text: gameModel.player1Name
                                color: gameModel.playerOnTurnIndex === 0 ? "#CDD6F4" : "#7F849C"
                                font.pixelSize: 20
                                fontSizeMode: Text.Fit
                                minimumPixelSize: 10
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: gameModel.playerOnTurnIndex === 0
                                leftPadding: 5
                                rightPadding: 5
                            }
                            Text {
                                text: gameModel.player1Score
                                color: gameModel.playerOnTurnIndex === 0 ? "#89B4FA" : "#7F849C"
                                font.pixelSize: 36
                                font.bold: true
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                    }

                    // Separator
                    Rectangle {
                        width: 1
                        height: 40
                        color: "#585B70"
                        Layout.alignment: Qt.AlignVCenter
                    }

                    // Player 2
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: gameModel.playerOnTurnIndex === 1 ? "#45475A" : "transparent"

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 0

                            Text {
                                text: gameModel.player2Name
                                color: gameModel.playerOnTurnIndex === 1 ? "#CDD6F4" : "#7F849C"
                                font.pixelSize: 20
                                fontSizeMode: Text.Fit
                                minimumPixelSize: 10
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: gameModel.playerOnTurnIndex === 1
                                leftPadding: 5
                                rightPadding: 5
                            }
                            Text {
                                text: gameModel.player2Score
                                color: gameModel.playerOnTurnIndex === 1 ? "#89B4FA" : "#7F849C"
                                font.pixelSize: 36
                                font.bold: true
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                    }
                }
            }

            // History (Review Mode)
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#313244"
                radius: 10
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    GridView {
                        id: historyList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: gameModel.history
                        clip: true

                        cellWidth: (width - 14) / 2
                        cellHeight: 80
                        flow: GridView.FlowLeftToRight

                        ScrollBar.vertical: ScrollBar {
                            id: vbar
                            active: true
                            width: 12
                            anchors.right: parent.right

                            contentItem: Rectangle {
                                implicitWidth: 8
                                implicitHeight: 100
                                radius: 4
                                color: vbar.pressed ? "#89B4FA" : (vbar.hovered ? "#6C7086" : "#585B70")
                            }

                            background: Rectangle {
                                color: "#1E1E2E"
                                opacity: 0.2
                                radius: 4
                            }
                        }

                        onCountChanged: if (gameModel.currentHistoryIndex >= 0) positionViewAtIndex(gameModel.currentHistoryIndex, GridView.Visible)

                        Connections {
                            target: gameModel
                            function onGameChanged() {
                                if (gameModel.currentHistoryIndex >= 0) {
                                    historyList.positionViewAtIndex(gameModel.currentHistoryIndex, GridView.Visible)
                                } else {
                                    historyList.positionViewAtBeginning()
                                }
                            }
                        }

                        delegate: Rectangle {
                            width: historyList.cellWidth
                            height: historyList.cellHeight
                            color: (index === gameModel.currentHistoryIndex) ? "#585B70" : (historyMouseArea.containsMouse ? "#45475A" : "transparent")

                            MouseArea {
                                id: historyMouseArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: gameModel.jumpToHistoryIndex(index)
                            }

                            Item {
                                anchors.fill: parent
                                anchors.margins: 8

                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Repeater {
                                        model: modelData.scoreLines
                                        delegate: RowLayout {
                                            spacing: 4

                                            Text {
                                                text: modelData.text
                                                color: "#CDD6F4"
                                                font.pixelSize: modelData.type === 1 ? 14 : 10
                                                font.bold: modelData.type === 1
                                                textFormat: Text.PlainText
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                            Text {
                                                text: modelData.scoreText
                                                color: {
                                                    if (modelData.type === 4 || modelData.type === 6) return "#A6E3A1"
                                                    if (modelData.type === 2 || modelData.type === 7 || modelData.type === 8) return "#F38BA8"
                                                    return "#89B4FA"
                                                }
                                                font.pixelSize: 10
                                                font.bold: false
                                                Layout.alignment: Qt.AlignVCenter
                                            }
                                        }
                                    }

                                    Text {
                                        text: modelData.rackString
                                        color: "#A6ADC8"
                                        font.family: "Consolas"
                                        font.pixelSize: 12
                                        textFormat: Text.StyledText
                                    }
                                }

                                Text {
                                    text: modelData.cumulativeScore
                                    color: "#89B4FA"
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.bottomMargin: 5
                                    font.bold: true
                                    font.pixelSize: 16
                                }
                            }

                            Rectangle {
                                visible: index % 2 === 0
                                width: 1
                                height: parent.height
                                color: "#585B70"
                                opacity: 0.3
                                anchors.right: parent.right
                            }

                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: "#585B70"
                                opacity: 0.3
                            }
                        }
                    }
                }
            }
        }

        // Right Column: Analysis (Review Mode only)
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.34
            spacing: 20
            visible: gameModel.gameMode === GameHistoryModel.ReviewMode

            AnalysisPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                analysisModel: gameModel.analysisModel
                lexiconName: gameModel.lexiconName
            }
        }

        // Right Column: Play Mode (Player Headers + History Grid + Tracking)
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 10
            visible: gameModel.gameMode === GameHistoryModel.PlayMode

            // Player Info Headers Row
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 90
                color: "#313244"
                radius: 10

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8

                    // Player 1 Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: gameModel.playerOnTurnIndex === 0 ? "#45475A" : "transparent"
                        border.color: gameModel.playerOnTurnIndex === 0 ? "#89B4FA" : "transparent"
                        border.width: 2

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8

                            // Avatar circle
                            Rectangle {
                                width: 48
                                height: 48
                                radius: 24
                                color: "#89B4FA"
                                Layout.alignment: Qt.AlignVCenter

                                Text {
                                    anchors.centerIn: parent
                                    text: gameModel.player1Name.charAt(0).toUpperCase()
                                    color: "#1E1E2E"
                                    font.pixelSize: 22
                                    font.bold: true
                                }
                            }

                            // Name, Score, Timer column
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 2

                                Text {
                                    text: gameModel.player1Name
                                    color: gameModel.playerOnTurnIndex === 0 ? "#CDD6F4" : "#7F849C"
                                    font.bold: true
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                RowLayout {
                                    spacing: 6
                                    Text {
                                        text: gameModel.player1Score
                                        color: "#89B4FA"
                                        font.pixelSize: 24
                                        font.bold: true
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: formatTime(gameModel.player1Clock)
                                        color: gameModel.player1Clock < 30 ? "#F38BA8" : "#A6E3A1"
                                        font.family: "Consolas"
                                        font.pixelSize: 16
                                    }
                                    // Pause button
                                    Rectangle {
                                        width: 28
                                        height: 28
                                        radius: 14
                                        color: "#4A90E2"

                                        Text {
                                            anchors.centerIn: parent
                                            text: gameModel.timerRunning ? "||" : ">"
                                            color: "white"
                                            font.pixelSize: 12
                                            font.bold: true
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            onClicked: gameModel.toggleTimer()
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Separator
                    Rectangle {
                        width: 1
                        Layout.fillHeight: true
                        Layout.topMargin: 10
                        Layout.bottomMargin: 10
                        color: "#585B70"
                    }

                    // Player 2 Header
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: gameModel.playerOnTurnIndex === 1 ? "#45475A" : "transparent"
                        border.color: gameModel.playerOnTurnIndex === 1 ? "#F38BA8" : "transparent"
                        border.width: 2

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 8

                            // Avatar circle
                            Rectangle {
                                width: 48
                                height: 48
                                radius: 24
                                color: "#F38BA8"
                                Layout.alignment: Qt.AlignVCenter

                                Text {
                                    anchors.centerIn: parent
                                    text: gameModel.player2Name.charAt(0).toUpperCase()
                                    color: "#1E1E2E"
                                    font.pixelSize: 22
                                    font.bold: true
                                }
                            }

                            // Name, Score, Timer column
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 2

                                Text {
                                    text: gameModel.player2Name
                                    color: gameModel.playerOnTurnIndex === 1 ? "#CDD6F4" : "#7F849C"
                                    font.bold: true
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                    Layout.fillWidth: true
                                }

                                RowLayout {
                                    spacing: 6
                                    Text {
                                        text: gameModel.player2Score
                                        color: "#89B4FA"
                                        font.pixelSize: 24
                                        font.bold: true
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: formatTime(gameModel.player2Clock)
                                        color: gameModel.player2Clock < 30 ? "#F38BA8" : "#A6E3A1"
                                        font.family: "Consolas"
                                        font.pixelSize: 16
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Two-Column History Grid (Play Mode)
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#313244"
                radius: 10
                clip: true

                GridView {
                    id: playModeHistoryGrid
                    anchors.fill: parent
                    anchors.margins: 7
                    model: gameModel.history
                    clip: true

                    cellWidth: (width - 14) / 2
                    cellHeight: 70
                    flow: GridView.FlowLeftToRight

                    ScrollBar.vertical: ScrollBar {
                        id: playModeVbar
                        active: true
                        width: 12
                        anchors.right: parent.right

                        contentItem: Rectangle {
                            implicitWidth: 8
                            implicitHeight: 100
                            radius: 4
                            color: playModeVbar.pressed ? "#89B4FA" : (playModeVbar.hovered ? "#6C7086" : "#585B70")
                        }

                        background: Rectangle {
                            color: "#1E1E2E"
                            opacity: 0.2
                            radius: 4
                        }
                    }

                    onCountChanged: if (count > 0) positionViewAtEnd()

                    Connections {
                        target: gameModel
                        function onGameChanged() {
                            if (playModeHistoryGrid.count > 0) {
                                playModeHistoryGrid.positionViewAtEnd()
                            }
                        }
                    }

                    delegate: Rectangle {
                        width: playModeHistoryGrid.cellWidth
                        height: playModeHistoryGrid.cellHeight
                        color: (index === gameModel.currentHistoryIndex) ? "#585B70" : (playHistoryMouseArea.containsMouse ? "#45475A" : "transparent")

                        MouseArea {
                            id: playHistoryMouseArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: gameModel.jumpToHistoryIndex(index)
                        }

                        Item {
                            anchors.fill: parent
                            anchors.margins: 6

                            // Notation + score line
                            ColumnLayout {
                                anchors.left: parent.left
                                anchors.right: cumulativeText.left
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 1

                                Repeater {
                                    model: modelData.scoreLines
                                    delegate: RowLayout {
                                        spacing: 4

                                        Text {
                                            text: modelData.text
                                            color: "#CDD6F4"
                                            font.pixelSize: modelData.type === 1 ? 13 : 10
                                            font.bold: modelData.type === 1
                                            textFormat: Text.PlainText
                                            Layout.alignment: Qt.AlignVCenter
                                        }
                                        Text {
                                            text: modelData.scoreText
                                            color: {
                                                if (modelData.type === 4 || modelData.type === 6) return "#A6E3A1"
                                                if (modelData.type === 2 || modelData.type === 7 || modelData.type === 8) return "#F38BA8"
                                                return "#89B4FA"
                                            }
                                            font.pixelSize: 11
                                            Layout.alignment: Qt.AlignVCenter
                                        }
                                    }
                                }

                                // Rack
                                Text {
                                    text: modelData.rackString
                                    color: "#A6ADC8"
                                    font.family: "Consolas"
                                    font.pixelSize: 11
                                    textFormat: Text.PlainText
                                }
                            }

                            // Cumulative score (bottom-right)
                            Text {
                                id: cumulativeText
                                text: modelData.cumulativeScore
                                color: "#89B4FA"
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.bottomMargin: 2
                                font.bold: true
                                font.pixelSize: 14
                            }
                        }

                        // Column separator
                        Rectangle {
                            visible: index % 2 === 0
                            width: 1
                            height: parent.height
                            color: "#585B70"
                            opacity: 0.3
                            anchors.right: parent.right
                        }

                        // Row separator
                        Rectangle {
                            anchors.bottom: parent.bottom
                            width: parent.width
                            height: 1
                            color: "#585B70"
                            opacity: 0.3
                        }
                    }
                }
            }

            // Tile Tracking (Play Mode - compact at bottom)
            Rectangle {
                id: playModeTracking
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(140, 50 + playModeTilesText.implicitHeight)
                color: "#313244"
                radius: 10
                clip: true

                Column {
                    anchors.fill: parent
                    anchors.margins: 5
                    spacing: 0

                    // Header
                    Row {
                        width: parent.width
                        height: 18

                        Text {
                            text: gameModel.bagCount + " IN BAG"
                            color: "#CDD6F4"
                            font.pixelSize: 10
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            width: parent.width / 2
                        }

                        Text {
                            text: (gameModel.vowelCount + gameModel.consonantCount + gameModel.blankCount) + " UNSEEN"
                            color: "#CDD6F4"
                            font.pixelSize: 10
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            width: parent.width / 2
                        }
                    }

                    // Tiles
                    Text {
                        id: playModeTilesText
                        width: parent.width
                        text: gameModel.unseenTiles
                        color: "#FFFFFF"
                        font.family: "Consolas"
                        font.pixelSize: 16
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        padding: 2
                    }

                    // Footer
                    Row {
                        width: parent.width
                        height: 18

                        Text {
                            text: gameModel.vowelCount + " VOWELS"
                            color: "#CDD6F4"
                            font.pixelSize: 10
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            width: parent.width / 2
                        }

                        Text {
                            text: gameModel.consonantCount + " CONSONANTS"
                            color: "#CDD6F4"
                            font.pixelSize: 10
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            width: parent.width / 2
                        }
                    }
                }
            }
        }
        }
    }

    StartupView {
        anchors.fill: parent
        visible: gameModel.gameMode === GameHistoryModel.SetupMode
        onStartRequested: (lexicon, minutes) => {
            gameModel.startNewGame(lexicon, minutes)
        }
    }
}
