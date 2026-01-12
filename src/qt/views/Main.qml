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
            anchors.margins: 20
            spacing: 20

            // Left Column: Board, Tracking, Rack, Controls
            Item {
                id: leftColumn
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.32

            // Cell size based on width only (board is square)
            property double availableW: width - 20
            property int computedCellSize: Math.max(10, Math.min(Math.floor((width - 20) / 15), Math.floor((height - 240) / 18)))

            Item {
                id: keyboardEntryController
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
                        addTile(String.fromCharCode(event.key));
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Backspace) {
                        removeLastTile();
                        event.accepted = true;
                    } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                        commitMove();
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
                // Simple letter add logic
                uncommittedTiles.append({
                    row: keyboardCursor.row,
                    col: keyboardCursor.col,
                    letter: letter,
                    isBlank: false
                });
                advanceCursor();
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

            // Player Headers (Clocks)
            RowLayout {
                id: gameHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 40
                visible: gameModel.gameMode === GameHistoryModel.PlayMode
                spacing: 12

                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    color: gameModel.playerOnTurnIndex === 0 ? "#45475A" : "#313244"
                    radius: 8
                    border.color: gameModel.playerOnTurnIndex === 0 ? "#89B4FA" : "transparent"
                    border.width: 2

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        Text {
                            text: gameModel.player1Name
                            color: "#CDD6F4"
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: formatTime(gameModel.player1Clock)
                            color: gameModel.player1Clock < 30 ? "#F38BA8" : "#A6E3A1"
                            font.family: "Consolas"
                            font.pixelSize: 18
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 40
                    color: gameModel.playerOnTurnIndex === 1 ? "#45475A" : "#313244"
                    radius: 8
                    border.color: gameModel.playerOnTurnIndex === 1 ? "#F38BA8" : "transparent"
                    border.width: 2

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 10
                        Text {
                            text: gameModel.player2Name
                            color: "#CDD6F4"
                            font.bold: true
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: formatTime(gameModel.player2Clock)
                            color: gameModel.player2Clock < 30 ? "#F38BA8" : "#A6E3A1"
                            font.family: "Consolas"
                            font.pixelSize: 18
                        }
                    }
                }
            }

            // Board Area (anchored to header or top)
            Item {
                id: boardArea
                anchors.top: gameHeader.visible ? gameHeader.bottom : parent.top
                anchors.topMargin: gameHeader.visible ? 10 : 0
                anchors.left: parent.left
                anchors.right: parent.right
                height: boardBackground.height
                
                // Background for Board (tight fit)
                Rectangle {
                    id: boardBackground
                    color: "#313244"
                    radius: 10
                    anchors.centerIn: parent
                    width: gridContainer.width + 20
                    height: gridContainer.height + 20
                    
                    Item {
                        id: gridContainer
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

                        // Keyboard Cursor
                        Item {
                            id: keyboardCursor
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
                                width: gridContainer.cellSize - 4
                                height: gridContainer.cellSize - 4
                                x: model.col * gridContainer.cellSize + 2
                                y: model.row * gridContainer.cellSize + 2
                                color: "#F9E2AF"
                                radius: 4
                                z: 5
                                
                                Text {
                                    anchors.centerIn: parent
                                    text: model.letter
                                    color: "#1E1E2E"
                                    font.bold: true
                                    font.pixelSize: parent.height * 0.7
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
                anchors.topMargin: 15
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

            // Tile Tracking (fills space between rack and controls)
            Rectangle {
                id: tileTrackingArea
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


        // Middle Column: Status & History
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.34
            spacing: 20
            
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
                                font.pixelSize: 20 // Reduced max font size
                                fontSizeMode: Text.Fit
                                minimumPixelSize: 10
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: gameModel.playerOnTurnIndex === 0
                                leftPadding: 5 // Added padding
                                rightPadding: 5 // Added padding
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
                                font.pixelSize: 20 // Reduced max font size
                                fontSizeMode: Text.Fit
                                minimumPixelSize: 10
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: gameModel.playerOnTurnIndex === 1
                                leftPadding: 5 // Added padding
                                rightPadding: 5 // Added padding
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
            
            // History
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#313244"
                radius: 10
                clip: true
                
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0
                    
                    // List
                    GridView {
                        id: historyList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        model: gameModel.history
                        clip: true
                        
                        // Adjust layout for scrollbar
                        cellWidth: (width - 14) / 2
                        cellHeight: 80 // Increased height for more clearance
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
                        
                        // Auto-scroll to selection
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

                            // Content Container
                            Item {
                                anchors.fill: parent
                                anchors.margins: 8
                                
                                ColumnLayout {
                                    anchors.left: parent.left
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2 // Spacing between move lines
                                    
                                    Repeater {
                                        model: modelData.scoreLines // Iterate over scoreLines
                                        delegate: RowLayout {
                                            spacing: 4 // Space between move part and score part
                                            
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
                                                font.pixelSize: 10 // Smaller font size
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
                                    anchors.bottom: parent.bottom // Move to bottom
                                    anchors.bottomMargin: 5 // Add margin
                                    font.bold: true
                                    font.pixelSize: 16
                                }
                            }
                            
                            // Vertical Separator (only for left column / even index)
                            Rectangle {
                                visible: index % 2 === 0
                                width: 1
                                height: parent.height
                                color: "#585B70"
                                opacity: 0.3
                                anchors.right: parent.right
                            }
                            
                            // Horizontal Separator line
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

        // Right Column: Analysis
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.34
            spacing: 20
            
            AnalysisPanel {
                Layout.fillWidth: true
                Layout.fillHeight: true
                analysisModel: gameModel.analysisModel
                lexiconName: gameModel.lexiconName
            }
        }        }
    }

    StartupView {
        anchors.fill: parent
        visible: gameModel.gameMode === GameHistoryModel.SetupMode
        onStartRequested: (lexicon, minutes) => {
            gameModel.startNewGame(lexicon, minutes)
        }
    }
}
