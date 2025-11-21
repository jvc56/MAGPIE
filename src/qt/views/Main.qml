import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtCore
import QtPie 1.0

ApplicationWindow {
    width: 1280
    height: 800
    minimumWidth: 800
    minimumHeight: 600
    visible: true
    title: "QtPie - MAGPIE Frontend"
    color: "#1E1E2E"
    font.family: "Clear Sans"

    Settings {
        id: appSettings
        property string recentFilesJson: "[]"
    }

    function addToRecentFiles(fileUrl) {
        var urlStr = fileUrl.toString();
        var files = JSON.parse(appSettings.recentFilesJson);
        var index = files.indexOf(urlStr);
        if (index !== -1) files.splice(index, 1);
        files.unshift(urlStr);
        if (files.length > 10) files.pop();
        appSettings.recentFilesJson = JSON.stringify(files);
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
            // Sample GCG
            loadGame("#lexicon CSW24\n#player1 olaugh olaugh\n#player2 magpie magpie\n>olaugh: AEIOUQI 8H QI +11 11\n>magpie: AB CDE 9G BE +4 4");
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
                enabled: JSON.parse(appSettings.recentFilesJson).length > 0
                
                Repeater {
                    model: JSON.parse(appSettings.recentFilesJson)
                    MenuItem {
                        text: decodeURIComponent(modelData.toString().split('/').pop())
                        onTriggered: gameModel.loadGameFromFile(modelData)
                    }
                }
                
                MenuSeparator {
                    visible: JSON.parse(appSettings.recentFilesJson).length > 0
                }
                
                MenuItem {
                    text: "Clear Recent Games"
                    enabled: JSON.parse(appSettings.recentFilesJson).length > 0
                    onTriggered: appSettings.recentFilesJson = "[]"
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

    RowLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 20
        
        // Left Column: Board, Rack, Controls
        ColumnLayout {
            id: leftColumn
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.4
            spacing: 20
            
            // Calculate cell size to fit both width and height constraints to avoid binding loops
            // Height breakdown:
            // Board: 15*S + 20 (margin)
            // Rack: 1.4*S
            // Controls: 1.0*S
            // Spacing: 2 * 20 = 40
            // Total Height = 17.4*S + 60
            // Total Width = 15*S + 20
            property double availableH: height - 60
            property double availableW: width - 20
            property int computedCellSize: Math.max(10, Math.floor(Math.min(availableW / 15, availableH / 17.4)))

            // Board Area
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                
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
                            "1,2": { color: "#FA8072", text: "2W", textColor: "#1E1E2E" }, // DWS - Salmon
                            "1,3": { color: "#FF0000", text: "3W", textColor: "#FFFFFF" }, // TWS - Red
                            "2,1": { color: "#ADD8E6", text: "2L", textColor: "#1E1E2E" }, // DLS - Light Blue
                            "3,1": { color: "#00008B", text: "3L", textColor: "#FFFFFF" }, // TLS - Dark Blue
                            "1,1": { color: "#45475A", text: "", textColor: "#CDD6F4" }     // Normal
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
                                        ctx.fillStyle = modelData.isLastMove ? "#FAB387" : "#F9E2AF"; // Peach for last move, Beige normal
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
                                        var style = gridContainer.bonusStyles[modelData.letterMultiplier + "," + modelData.wordMultiplier];
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

            // Rack
            RackView {
                Layout.alignment: Qt.AlignHCenter
                rack: gameModel.currentRack
                tileSize: leftColumn.computedCellSize
            }

            // Controls
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 15
                
                // Go to First Button
                Button {
                    id: goToFirstButton
                    Layout.preferredWidth: leftColumn.computedCellSize
                    Layout.preferredHeight: leftColumn.computedCellSize
                    enabled: gameModel.currentEventIndex > 0
                    onClicked: gameModel.jumpTo(0)
                    background: Rectangle {
                        width: parent.width
                        height: parent.height
                        color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
                        radius: width / 2
                        opacity: goToFirstButton.enabled ? 1.0 : 0.5
                    }
                    contentItem: Item {
                        anchors.fill: parent
                        Canvas {
                            anchors.centerIn: parent
                            width: parent.width * 0.5
                            height: parent.height * 0.5
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.clearRect(0, 0, width, height);
                                ctx.fillStyle = "white";
                                
                                var s = Math.min(width, height);
                                // Vertical bar
                                ctx.fillRect(0, s * 0.2, s * 0.15, s * 0.6);
                                // Left arrow
                                ctx.beginPath();
                                ctx.moveTo(s * 0.9, s * 0.2);
                                ctx.lineTo(s * 0.3, s * 0.5);
                                ctx.lineTo(s * 0.9, s * 0.8);
                                ctx.closePath();
                                ctx.fill();
                            }
                            onWidthChanged: requestPaint()
                        }
                    }
                }

                // Previous Button
                Button {
                    id: previousButton
                    Layout.preferredWidth: leftColumn.computedCellSize
                    Layout.preferredHeight: leftColumn.computedCellSize
                    enabled: gameModel.currentEventIndex > 0
                    onClicked: gameModel.previous()
                    background: Rectangle {
                        width: parent.width
                        height: parent.height
                        color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
                        radius: width / 2
                        opacity: previousButton.enabled ? 1.0 : 0.5
                    }
                    contentItem: Item {
                        anchors.fill: parent
                        Canvas {
                            anchors.centerIn: parent
                            width: parent.width * 0.5
                            height: parent.height * 0.5
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.clearRect(0, 0, width, height);
                                ctx.fillStyle = "white";
                                
                                var s = Math.min(width, height);
                                // Left arrow
                                ctx.beginPath();
                                ctx.moveTo(s * 0.8, s * 0.2);
                                ctx.lineTo(s * 0.2, s * 0.5);
                                ctx.lineTo(s * 0.8, s * 0.8);
                                ctx.closePath();
                                ctx.fill();
                            }
                            onWidthChanged: requestPaint()
                        }
                    }
                }

                Text {
                    text: "Turn " + (gameModel.currentEventIndex + 1) + " of " + gameModel.totalEvents
                    color: "#CDD6F4"
                    font.pixelSize: 18
                    font.bold: true
                    Layout.alignment: Qt.AlignVCenter
                    Layout.margins: 10
                }

                // Next Button
                Button {
                    id: nextButton
                    Layout.preferredWidth: leftColumn.computedCellSize
                    Layout.preferredHeight: leftColumn.computedCellSize
                    enabled: gameModel.currentEventIndex < gameModel.totalEvents - 1
                    onClicked: gameModel.next()
                    background: Rectangle {
                        width: parent.width
                        height: parent.height
                        color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
                        radius: width / 2
                        opacity: nextButton.enabled ? 1.0 : 0.5
                    }
                    contentItem: Item {
                        anchors.fill: parent
                        Canvas {
                            anchors.centerIn: parent
                            width: parent.width * 0.5
                            height: parent.height * 0.5
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.clearRect(0, 0, width, height);
                                ctx.fillStyle = "white";
                                
                                var s = Math.min(width, height);
                                // Right arrow
                                ctx.beginPath();
                                ctx.moveTo(s * 0.2, s * 0.2);
                                ctx.lineTo(s * 0.8, s * 0.5);
                                ctx.lineTo(s * 0.2, s * 0.8);
                                ctx.closePath();
                                ctx.fill();
                            }
                            onWidthChanged: requestPaint()
                        }
                    }
                }

                // Go to Last Button
                Button {
                    id: goToLastButton
                    Layout.preferredWidth: leftColumn.computedCellSize
                    Layout.preferredHeight: leftColumn.computedCellSize
                    enabled: gameModel.currentEventIndex < gameModel.totalEvents - 1
                    onClicked: gameModel.jumpTo(gameModel.totalEvents - 1)
                    background: Rectangle {
                        width: parent.width
                        height: parent.height
                        color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
                        radius: width / 2
                        opacity: goToLastButton.enabled ? 1.0 : 0.5
                    }
                    contentItem: Item {
                        anchors.fill: parent
                        Canvas {
                            anchors.centerIn: parent
                            width: parent.width * 0.5
                            height: parent.height * 0.5
                            onPaint: {
                                var ctx = getContext("2d");
                                ctx.clearRect(0, 0, width, height);
                                ctx.fillStyle = "white";
                                
                                var s = Math.min(width, height);
                                // Vertical bar
                                ctx.fillRect(s * 0.85, s * 0.2, s * 0.15, s * 0.6);
                                // Right arrow
                                ctx.beginPath();
                                ctx.moveTo(s * 0.1, s * 0.2);
                                ctx.lineTo(s * 0.7, s * 0.5);
                                ctx.lineTo(s * 0.1, s * 0.8);
                                ctx.closePath();
                                ctx.fill();
                            }
                            onWidthChanged: requestPaint()
                        }
                    }
                }
            }
        }

        // Middle Column: Status & History
        ColumnLayout {
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.3
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
                        cellWidth: width / 2
                        cellHeight: 80 // Increased height for more clearance
                        flow: GridView.FlowLeftToRight
                        
                        // Auto-scroll to selection
                        onCountChanged: if (gameModel.currentEventIndex > 0) positionViewAtIndex(gameModel.currentEventIndex - 1, GridView.Visible)
                        
                        Connections {
                            target: gameModel
                            function onCurrentEventIndexChanged() {
                                if (gameModel.currentEventIndex > 0) {
                                    historyList.positionViewAtIndex(gameModel.currentEventIndex - 1, GridView.Visible)
                                } else {
                                    historyList.positionViewAtBeginning()
                                }
                            }
                        }

                        delegate: Rectangle {
                            width: historyList.cellWidth
                            height: historyList.cellHeight
                            color: (index === gameModel.currentEventIndex - 1) ? "#585B70" : "transparent"
                            
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
                                                color: "#CDD6F4"
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
            Layout.preferredWidth: parent.width * 0.3
            spacing: 20
            
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#313244"
                radius: 10
                
                Text {
                    text: "Analysis"
                    color: "#585B70"
                    font.pixelSize: 18
                    anchors.centerIn: parent
                }
            }
        }
    }
}