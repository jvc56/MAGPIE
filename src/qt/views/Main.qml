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
            Layout.preferredWidth: gameModel.gameMode === GameHistoryModel.PlayMode ? parent.width * 0.4 : parent.width * 0.32

            // Cell size: fit the largest square board in available space
            // In Play mode: header(80) + margin(8) + board + margin(10) + rack(~50) + controls(50) + padding
            // In Review mode: board + margin(10) + rack(~50) + tracking + controls(50) + padding
            property int playModeVerticalSpace: height - 220
            property int reviewModeVerticalSpace: height - 200
            property int availableVertical: gameModel.gameMode === GameHistoryModel.PlayMode ? playModeVerticalSpace : reviewModeVerticalSpace
            // Also account for board background padding (12px)
            property int computedCellSize: Math.max(10, Math.min(Math.floor((width - 12) / 15), Math.floor((availableVertical - 12) / 15)))

            Item {
                id: keyboardEntryController
                objectName: "keyboardEntryController"
                anchors.fill: parent
                focus: gameModel.gameMode === GameHistoryModel.PlayMode
                
                Keys.onPressed: (event) => {
                    if (gameModel.gameMode !== GameHistoryModel.PlayMode) return;

                    if (event.key === Qt.Key_Right) {
                        keyboardCursor.col = (keyboardCursor.col + 1) % 15;
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Left) {
                        keyboardCursor.col = (keyboardCursor.col + 14) % 15;
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Down) {
                        keyboardCursor.row = (keyboardCursor.row + 1) % 15;
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Up) {
                        keyboardCursor.row = (keyboardCursor.row + 14) % 15;
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Space) {
                        keyboardCursor.dir = 1 - keyboardCursor.dir;
                        event.accepted = true;
                    } else if (event.key >= Qt.Key_A && event.key <= Qt.Key_Z) {
                        leftColumn.addTile(String.fromCharCode(event.key));
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Backspace) {
                        leftColumn.removeLastTile();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        leftColumn.commitMove();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Escape) {
                        uncommittedTiles.clear();
                        event.accepted = true;
                    }
                }
            }

            ListModel {
                id: uncommittedTiles
            }

            function addTile(letter) {
                // Verify letter is in rack
                let rackStr = gameModel.currentRack.toUpperCase();
                let upperLetter = letter.toUpperCase();

                // Track how many of this letter are already in uncommittedTiles
                let usedCount = 0;
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    if (uncommittedTiles.get(i).letter.toUpperCase() === upperLetter) {
                        usedCount++;
                    }
                }

                // Count available in rack
                let availableCount = 0;
                for (let i = 0; i < rackStr.length; i++) {
                    if (rackStr[i] === upperLetter) {
                        availableCount++;
                    }
                }

                // For now, if not in rack, check for blank?
                // Let's just do a strict check for now.
                if (usedCount >= availableCount) {
                    console.log("Letter " + upperLetter + " not available in rack (available: " + availableCount + ", used: " + usedCount + ")");
                    return;
                }

                uncommittedTiles.append({
                    row: keyboardCursor.row,
                    col: keyboardCursor.col,
                    letter: upperLetter,
                    isBlank: false
                });
                advanceCursor();
            }

            // Add a tile at a specific board position (for drag-drop)
            function addTileAtPosition(letter, row, col) {
                let rackStr = gameModel.currentRack.toUpperCase();
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

                // Track how many of this letter are already in uncommittedTiles
                let usedCount = 0;
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    if (uncommittedTiles.get(i).letter.toUpperCase() === upperLetter) {
                        usedCount++;
                    }
                }

                // Count available in rack
                let availableCount = 0;
                for (let i = 0; i < rackStr.length; i++) {
                    if (rackStr[i] === upperLetter) {
                        availableCount++;
                    }
                }

                if (usedCount >= availableCount) {
                    console.log("Letter " + upperLetter + " not available in rack");
                    return false;
                }

                uncommittedTiles.append({
                    row: row,
                    col: col,
                    letter: upperLetter,
                    isBlank: letter >= 'a' && letter <= 'z'
                });

                // Move cursor to dropped position
                keyboardCursor.row = row;
                keyboardCursor.col = col;

                return true;
            }

            function removeLastTile() {
                if (uncommittedTiles.count > 0) {
                    uncommittedTiles.remove(uncommittedTiles.count - 1);
                    // Reverse cursor? (Optional, maybe just stay)
                }
            }

            function advanceCursor() {
                if (keyboardCursor.dir === 0) {
                    keyboardCursor.col = (keyboardCursor.col + 1) % 15;
                } else {
                    keyboardCursor.row = (keyboardCursor.row + 1) % 15;
                }
            }

            function commitMove() {
                if (uncommittedTiles.count === 0) return;
                
                let first = uncommittedTiles.get(0);
                let colChar = String.fromCharCode(65 + first.col);
                let rowStr = (first.row + 1).toString();
                let word = "";
                for (let i = 0; i < uncommittedTiles.count; i++) {
                    word += uncommittedTiles.get(i).letter;
                }
                
                let pos = (keyboardCursor.dir === 0) ? (rowStr + colChar) : (colChar + rowStr);
                let notation = pos + " " + word;
                
                gameModel.submitMove(notation);
                uncommittedTiles.clear();
            }

            // Player Info Header (Play Mode)
            Rectangle {
                id: gameHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 80
                visible: gameModel.gameMode === GameHistoryModel.PlayMode
                color: "#313244"
                radius: 10

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 6
                    spacing: 8

                    // Player 1 Info
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: gameModel.playerOnTurnIndex === 0 ? "#45475A" : "transparent"
                        border.color: gameModel.playerOnTurnIndex === 0 ? "#89B4FA" : "transparent"
                        border.width: 2

                        Column {
                            anchors.fill: parent
                            anchors.margins: 5
                            spacing: 0

                            // Name and Time row
                            Row {
                                width: parent.width
                                height: 16
                                Text {
                                    text: gameModel.player1Name
                                    color: gameModel.playerOnTurnIndex === 0 ? "#CDD6F4" : "#7F849C"
                                    font.bold: true
                                    font.pixelSize: 12
                                }
                                Item { width: parent.width - parent.children[0].width - parent.children[2].width; height: 1 }
                                Text {
                                    text: formatTime(gameModel.player1Clock)
                                    color: gameModel.player1Clock < 30 ? "#F38BA8" : "#A6E3A1"
                                    font.family: "Consolas"
                                    font.pixelSize: 12
                                }
                            }

                            // Score
                            Text {
                                width: parent.width
                                height: 28
                                text: gameModel.player1Score
                                color: gameModel.playerOnTurnIndex === 0 ? "#89B4FA" : "#7F849C"
                                font.pixelSize: 24
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            // Rack (only show for player on turn)
                            Text {
                                visible: gameModel.playerOnTurnIndex === 0
                                width: parent.width
                                height: 18
                                text: gameModel.currentRack
                                color: "#F9E2AF"
                                font.family: "Consolas"
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }

                    // Separator
                    Rectangle {
                        width: 1
                        Layout.fillHeight: true
                        Layout.topMargin: 8
                        Layout.bottomMargin: 8
                        color: "#585B70"
                    }

                    // Player 2 Info
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 8
                        color: gameModel.playerOnTurnIndex === 1 ? "#45475A" : "transparent"
                        border.color: gameModel.playerOnTurnIndex === 1 ? "#F38BA8" : "transparent"
                        border.width: 2

                        Column {
                            anchors.fill: parent
                            anchors.margins: 5
                            spacing: 0

                            // Name and Time row
                            Row {
                                width: parent.width
                                height: 16
                                Text {
                                    text: gameModel.player2Name
                                    color: gameModel.playerOnTurnIndex === 1 ? "#CDD6F4" : "#7F849C"
                                    font.bold: true
                                    font.pixelSize: 12
                                }
                                Item { width: parent.width - parent.children[0].width - parent.children[2].width; height: 1 }
                                Text {
                                    text: formatTime(gameModel.player2Clock)
                                    color: gameModel.player2Clock < 30 ? "#F38BA8" : "#A6E3A1"
                                    font.family: "Consolas"
                                    font.pixelSize: 12
                                }
                            }

                            // Score
                            Text {
                                width: parent.width
                                height: 28
                                text: gameModel.player2Score
                                color: gameModel.playerOnTurnIndex === 1 ? "#89B4FA" : "#7F849C"
                                font.pixelSize: 24
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }

                            // Rack (only show for player on turn)
                            Text {
                                visible: gameModel.playerOnTurnIndex === 1
                                width: parent.width
                                height: 18
                                text: gameModel.currentRack
                                color: "#F9E2AF"
                                font.family: "Consolas"
                                font.pixelSize: 13
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                    }
                }
            }

            // Board Area (anchored to header or top)
            Item {
                id: boardArea
                anchors.top: gameHeader.visible ? gameHeader.bottom : parent.top
                anchors.topMargin: gameHeader.visible ? 8 : 0
                anchors.left: parent.left
                anchors.right: parent.right
                height: boardBackground.height

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
                                    // Get letter from the dragged tile
                                    let letter = drop.source ? drop.source.charStr : "";
                                    if (letter && letter !== " ") {
                                        leftColumn.addTileAtPosition(letter, row, col);
                                        keyboardEntryController.forceActiveFocus();
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
                                        keyboardCursor.dir = 1 - keyboardCursor.dir;
                                    } else {
                                        keyboardCursor.row = r;
                                        keyboardCursor.col = c;
                                    }
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
                                color: "#89B4FA"
                                opacity: 0.4
                                radius: 4
                                
                                Text {
                                    anchors.centerIn: parent
                                    text: keyboardCursor.dir === 0 ? "▶" : "▼"
                                    color: "white"
                                    font.bold: true
                                    font.pixelSize: parent.height * 0.5
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
                                    anchors.fill: parent
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    bottomPadding: gridContainer.cellSize * 0.1
                                    text: uncommittedTile.letter
                                    font.family: "Clear Sans"
                                    font.pixelSize: uncommittedTile.isBlank ? gridContainer.cellSize * 0.4 : gridContainer.cellSize * 0.5
                                    font.bold: true
                                    color: "#1E1E2E"
                                }

                                // Blank designation box
                                Rectangle {
                                    width: parent.width * 0.5
                                    height: parent.height * 0.5
                                    anchors.centerIn: parent
                                    anchors.verticalCenterOffset: -gridContainer.cellSize * 0.05
                                    color: "transparent"
                                    border.color: "black"
                                    border.width: 1
                                    visible: uncommittedTile.isBlank
                                }

                                // Score subscript
                                Text {
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.rightMargin: text.length > 1 ? gridContainer.cellSize * 0.05 : gridContainer.cellSize * 0.12
                                    anchors.bottomMargin: gridContainer.cellSize * 0.05
                                    font.family: "Clear Sans"
                                    font.pixelSize: text.length > 1 ? gridContainer.cellSize * 0.22 : gridContainer.cellSize * 0.25
                                    color: "#1E1E2E"
                                    text: {
                                        if (uncommittedTile.isBlank) return "0";
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
                                    
                                    if (hasLetter) {
                                        // Draw tile background
                                        ctx.fillStyle = modelData.isLastMove ? "#FF9933" : "#F9E2AF"; // Orange for last move, Beige normal
                                        // Rounded rectangle drawing
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

                                        // Draw letter
                                        ctx.font = "900 " + (modelData.isBlank ? gridContainer.cellSize * 0.4 : gridContainer.cellSize * 0.5) + "px 'Clear Sans'";
                                        ctx.fillStyle = "#1E1E2E";
                                        ctx.textAlign = "center";
                                        ctx.textBaseline = "middle";
                                        var letterText = modelData.isBlank ? modelData.letter.toUpperCase() : modelData.letter;
                                        ctx.fillText(letterText, x + gridContainer.cellSize / 2, y + gridContainer.cellSize * 0.52);

                                        // Draw score
                                        if (modelData.score > 0) {
                                            var scoreStr = modelData.score.toString();
                                            var scoreFontSize = scoreStr.length > 1 ? (gridContainer.cellSize * 0.22) : (gridContainer.cellSize * 0.25);
                                            ctx.font = scoreFontSize + "px 'Clear Sans'";
                                            ctx.textAlign = "right";
                                            ctx.textBaseline = "bottom";
                                            
                                            var rightOffset = scoreStr.length > 1 ? 0.95 : 0.88;
                                            
                                            // Move up to 0.90 to match RackView appearance
                                            ctx.fillText(modelData.score, x + gridContainer.cellSize * rightOffset, y + gridContainer.cellSize * 0.90);
                                        }

                                        // Draw blank border
                                        if (modelData.isBlank) {
                                            ctx.strokeStyle = "black";
                                            ctx.lineWidth = 1;
                                            // Center around the new letter vertical position (0.52)
                                            var blankSize = gridContainer.cellSize * 0.5;
                                            var blankX = x + (gridContainer.cellSize - blankSize) / 2;
                                            var blankY = y + gridContainer.cellSize * 0.52 - blankSize / 2;
                                            ctx.strokeRect(blankX, blankY, blankSize, blankSize);
                                        }                                    } else {
                                        // Draw bonus square (rounded rectangle)
                                        var style = gridContainer.bonusStyles[modelData.letterMultiplier + "," + modelData.wordMultiplier] || gridContainer.bonusStyles["1,1"];
                                        ctx.fillStyle = style.color;
                                        
                                        var tileX = x + 1;
                                        var tileY = y + 1;
                                        var tileW = gridContainer.cellSize - 2;
                                        var tileH = gridContainer.cellSize - 2;
                                        var radius = 6; // Same radius as tiles
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

                    Button {
                        text: "PASS"
                        onClicked: gameModel.pass()
                        contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { implicitWidth: 80; implicitHeight: 40; color: "#F38BA8"; radius: 8 }
                    }

                    Button {
                        text: "EXCHANGE"
                        onClicked: { /* TODO: exchangeDialog */ }
                        contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { implicitWidth: 100; implicitHeight: 40; color: "#FAB387"; radius: 8 }
                    }

                    Button {
                        text: "AI MOVE"
                        onClicked: {
                            let notation = gameModel.getComputerMove();
                            if (notation) {
                                gameModel.submitMove(notation);
                            }
                        }
                        contentItem: Text { text: parent.text; color: "white"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { implicitWidth: 100; implicitHeight: 40; color: "#89B4FA"; radius: 8 }
                    }

                    Button {
                        text: "PLAY"
                        onClicked: commitMove()
                        contentItem: Text { text: parent.text; color: "#1E1E2E"; font.bold: true; horizontalAlignment: Text.AlignHCenter }
                        background: Rectangle { implicitWidth: 80; implicitHeight: 40; color: "#A6E3A1"; radius: 8 }
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

        // Right Column: Play Mode (Tracking + Scores + History)
        ColumnLayout {
            Layout.fillHeight: true
            Layout.fillWidth: true
            spacing: 15
            visible: gameModel.gameMode === GameHistoryModel.PlayMode

            // Tile Tracking (Play Mode - in right column)
            Rectangle {
                id: playModeTracking
                Layout.fillWidth: true
                // Dynamic height: header(28) + tiles text + footer(20) + padding(10)
                Layout.preferredHeight: Math.min(200, 58 + playModeTilesText.implicitHeight)
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
                        height: 22

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
                        font.pixelSize: 18
                        wrapMode: Text.Wrap
                        horizontalAlignment: Text.AlignHCenter
                        padding: 4
                    }

                    // Footer
                    Row {
                        width: parent.width
                        height: 22

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

            // History (Play Mode)
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#313244"
                radius: 10
                clip: true

                ListView {
                    id: playModeHistoryList
                    anchors.fill: parent
                    anchors.margins: 5
                    model: gameModel.history
                    clip: true
                    spacing: 2

                    ScrollBar.vertical: ScrollBar {
                        id: playModeVbar
                        active: true
                        width: 10
                    }

                    onCountChanged: if (count > 0) positionViewAtEnd()

                    delegate: Rectangle {
                        width: playModeHistoryList.width - 12
                        height: 50
                        color: (index === gameModel.currentHistoryIndex) ? "#585B70" : "transparent"
                        radius: 5

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 8
                            spacing: 10

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2

                                Repeater {
                                    model: modelData.scoreLines
                                    delegate: Text {
                                        text: modelData.text + " " + modelData.scoreText
                                        color: "#CDD6F4"
                                        font.pixelSize: 12
                                        font.bold: modelData.type === 1
                                    }
                                }
                            }

                            Text {
                                text: modelData.cumulativeScore
                                color: "#89B4FA"
                                font.bold: true
                                font.pixelSize: 16
                            }
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
