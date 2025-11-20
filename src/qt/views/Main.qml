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
            Layout.fillHeight: true
            Layout.preferredWidth: parent.width * 0.4
            spacing: 20

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
                        // Size based on available space in the parent Item
                        property int availableSpace: Math.min(parent.parent.width, parent.parent.height)
                        width: availableSpace > 20 ? availableSpace - 20 : 0
                        height: width
                        anchors.centerIn: parent

                        property int cellSize: width / 15
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
                                        ctx.fillStyle = "#F9E2AF"; // Beige
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
                tileSize: gridContainer.cellSize
            }

            // Controls
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 20
                
                Button { 
                    text: "Previous" 
                    onClicked: gameModel.previous()
                    enabled: gameModel.currentEventIndex > 0
                }
                Text { 
                    text: gameModel.currentEventIndex + " / " + gameModel.totalEvents 
                    color: "#CDD6F4"
                    font.pixelSize: 16
                }
                Button { 
                    text: "Next" 
                    onClicked: gameModel.next()
                    enabled: gameModel.currentEventIndex < gameModel.totalEvents
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
                                font.pixelSize: 24
                                fontSizeMode: Text.Fit
                                minimumPixelSize: 10
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: gameModel.playerOnTurnIndex === 0
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
                                font.pixelSize: 24
                                fontSizeMode: Text.Fit
                                minimumPixelSize: 10
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                font.bold: gameModel.playerOnTurnIndex === 1
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
            
            // History Placeholder
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#313244"
                radius: 10
                
                Text {
                    text: "Game History"
                    color: "#585B70"
                    font.pixelSize: 18
                    anchors.centerIn: parent
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
