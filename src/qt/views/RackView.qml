import QtQuick
import QtQuick.Controls

Item {
    id: root

    property string rack: ""

    // Internal state for drag-and-drop
    property string internalRack: ""
    property int tileSize: 40
    property int dropIndex: -1
    property int caretDisplayIndex: 0 // Tracks position for visual continuity during fade-out
    property int draggingIndex: -1
    property bool exchangeMode: false
    property var selectedIndices: []

    // Signal emitted when a tile is dropped externally (e.g., on board)
    signal tileDroppedOnBoard(string letter, int rackIndex)

    // Constants
    readonly property int tileSpacing: Math.max(2, tileSize * 0.1)

    // Sync external rack to internal rack (padding to 7)
    onRackChanged: {
        var r = rack;
        if (r === undefined || r === null) r = "";
        while (r.length < 7) r += " ";
        internalRack = r;
        selectedIndices = [];
    }
    
    Component.onCompleted: {
        var r = rack;
        if (r === undefined || r === null) r = "";
        while (r.length < 7) r += " ";
        internalRack = r;
    }
    
    implicitWidth: (7 * tileSize) + (6 * tileSpacing) + (tileSize * 0.5)
    implicitHeight: tileSize * 1.4
    
    Rectangle {
        anchors.fill: parent
        color: "#232433" // Slightly lighter than main background
        radius: tileSize * 0.2
    }
    
    function getSlotX(index) {
        var totalWidth = 7 * tileSize + 6 * tileSpacing;
        var startX = (root.width - totalWidth) / 2;
        return startX + index * (tileSize + tileSpacing);
    }

    function getSelectedTiles() {
        var result = "";
        for (var i = 0; i < selectedIndices.length; i++) {
            result += internalRack.charAt(selectedIndices[i]);
        }
        return result;
    }

    function clearSelection() {
        selectedIndices = [];
    }

    // Drop Indicator (Caret)
    Item {
        id: caret
        visible: root.dropIndex !== -1
        
        width: Math.max(4, root.tileSize * 0.1) // Width of caps
        height: root.tileSize + (root.tileSize * 0.2) // Taller than tile
        z: 50
        
        y: (root.height - height) / 2
        x: {
            // Use dropIndex if valid, otherwise fallback to caretDisplayIndex (for fade-out)
            var idx = root.dropIndex !== -1 ? root.dropIndex : root.caretDisplayIndex;
            
            var centerX;
            if (idx === 7) {
                 centerX = getSlotX(6) + tileSize + tileSpacing / 2;
            } else {
                centerX = getSlotX(idx) - tileSpacing / 2;
            }
            return centerX - width / 2;
        }
        
        // Vertical Line
        Rectangle {
            anchors.centerIn: parent
            width: Math.max(2, parent.width * 0.2)
            height: parent.height
            color: "#6496FF"
            radius: width / 2
        }
        
        // Top Cap
        Rectangle {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            height: Math.max(2, parent.width * 0.2)
            color: "#6496FF"
            radius: height / 2
        }
        
        // Bottom Cap
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            height: Math.max(2, parent.width * 0.2)
            color: "#6496FF"
            radius: height / 2
        }

        // Animate appearance
        Behavior on visible { NumberAnimation { duration: 100 } }
    }

    Repeater {
        model: 7
        delegate: Item {
            id: tileDelegate
            width: root.tileSize
            height: root.tileSize

            property int tileIndex: index
            property string charStr: root.internalRack.charAt(index)
            property bool isHeld: false

            // A tile is a blank if it is '?' or a lowercase letter (designated blank)
            property bool isBlank: charStr === "?" || (charStr >= "a" && charStr <= "z")
            property bool isSelected: root.exchangeMode && root.selectedIndices.indexOf(index) >= 0

            // Drag properties for external drop targets (e.g., board)
            Drag.active: isHeld && charStr !== " "
            Drag.keys: ["rackTile"]
            Drag.source: tileDelegate
            Drag.hotSpot.x: width / 2
            Drag.hotSpot.y: height / 2

            // Centered vertically, shifted up when selected in exchange mode
            y: (root.height - height) / 2 - (isSelected ? root.tileSize * 0.3 : 0)
            Behavior on y { NumberAnimation { duration: 150; easing.type: Easing.OutQuad } }

            // X position
            x: getSlotX(index)

            onIsHeldChanged: {
                if (!isHeld) {
                    x = Qt.binding(function() { return root.getSlotX(index) });
                    y = Qt.binding(function() {
                        return (root.height - height) / 2 - (isSelected ? root.tileSize * 0.3 : 0);
                    });
                }
            }

            z: isHeld ? 100 : 1
            
            // Tile Visual
            Rectangle {
                anchors.fill: parent
                radius: root.tileSize * 0.1
                
                // Shadow for dragged tile
                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: root.tileSize * 0.1
                    anchors.leftMargin: root.tileSize * 0.1
                    color: "black"
                    opacity: 0.2
                    radius: parent.radius
                    z: -1
                    visible: tileDelegate.isHeld
                }

                color: {
                    if (charStr === " ") return "#313244"; // Empty
                    if (tileDelegate.isSelected) return "#89CFF0"; // Selected for exchange
                    return "#F9E2AF"; // Tile color
                }
                border.color: tileDelegate.isSelected ? "#4A90E2" : "transparent"
                border.width: tileDelegate.isSelected ? 2 : 0
                
                // Style changes during drag
                opacity: tileDelegate.isHeld ? 0.9 : 1.0
                scale: tileDelegate.isHeld ? 1.15 : 1.0
                Behavior on scale { NumberAnimation { duration: 100 } }

                Text {
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    // Slight vertical adjustment to visually center Caps
                    bottomPadding: root.tileSize * 0.1
                    text: {
                        if (charStr === "?" || charStr === " ") return "";
                        return charStr.toUpperCase();
                    }
                    font.family: "Clear Sans"
                    font.pixelSize: root.tileSize * 0.5
                    font.bold: true
                    color: "#1E1E2E"
                    visible: charStr !== " "
                }
                
                // Blank designation (small centered box to match board style)
                Rectangle {
                    width: parent.width * 0.5
                    height: parent.height * 0.5
                    anchors.centerIn: parent
                    anchors.verticalCenterOffset: -root.tileSize * 0.05
                    color: "transparent"
                    border.color: "black"
                    border.width: 1
                    // Fix: Use 'isBlank' from scope (tileDelegate), NOT 'parent.isBlank'
                    visible: isBlank && charStr !== "?" && charStr !== " "
                }

                // Undesignated blank
                Text {
                    anchors.fill: parent
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    bottomPadding: root.tileSize * 0.1
                    text: "?"
                    font.family: "Clear Sans"
                    font.pixelSize: root.tileSize * 0.5
                    font.bold: true
                    color: "#1E1E2E"
                    visible: charStr === "?"
                }
                
                // Score
                Text {
                    id: scoreText
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: text.length > 1 ? root.tileSize * 0.05 : root.tileSize * 0.12
                    anchors.bottomMargin: root.tileSize * 0.05
                    font.family: "Clear Sans"
                    font.pixelSize: scoreText.text.length > 1 ? root.tileSize * 0.22 : root.tileSize * 0.25
                    color: "#1E1E2E"
                    text: {
                        if (charStr === " ") return "";
                        // Fix: Use 'isBlank' from scope
                        if (isBlank) return "0";
                        var l = charStr.toUpperCase();
                        var scores = {
                            'A':1, 'B':3, 'C':3, 'D':2, 'E':1, 'F':4, 'G':2, 'H':4, 'I':1, 'J':8, 'K':5, 'L':1, 'M':3,
                            'N':1, 'O':1, 'P':3, 'Q':10, 'R':1, 'S':1, 'T':1, 'U':1, 'V':4, 'W':4, 'X':8, 'Y':4, 'Z':10
                        };
                        return scores[l] || "0";
                    }
                    visible: charStr !== " "
                }
            }

            MouseArea {
                anchors.fill: parent
                drag.target: root.exchangeMode ? null : tileDelegate
                drag.smoothed: false
                drag.threshold: 5

                enabled: charStr !== " "

                onPressed: {
                    if (root.exchangeMode) {
                        var newIndices = root.selectedIndices.slice();
                        var idx = newIndices.indexOf(index);
                        if (idx >= 0) {
                            newIndices.splice(idx, 1);
                        } else {
                            newIndices.push(index);
                        }
                        root.selectedIndices = newIndices;
                        return;
                    }
                    tileDelegate.isHeld = true;
                    root.draggingIndex = index;
                }

                onPositionChanged: {
                    if (root.exchangeMode) return;
                    if (!tileDelegate.isHeld) return;
                    
                    var centerX = tileDelegate.x + tileDelegate.width / 2;
                    var bestDist = 99999;
                    var bestIdx = -1;
                    
                    for (var i = 0; i <= 7; i++) {
                        var boundaryX;
                        if (i === 0) boundaryX = root.getSlotX(0);
                        else if (i === 7) boundaryX = root.getSlotX(6) + root.tileSize + root.tileSpacing;
                        else {
                            var x1 = root.getSlotX(i-1) + root.tileSize;
                            var x2 = root.getSlotX(i);
                            boundaryX = (x1 + x2) / 2;
                        }
                        
                        var dist = Math.abs(centerX - boundaryX);
                        if (dist < bestDist) {
                            bestDist = dist;
                            bestIdx = i;
                        }
                    }
                    
                    if (bestIdx === root.draggingIndex || bestIdx === root.draggingIndex + 1) {
                        root.dropIndex = -1;
                    } else {
                        root.dropIndex = bestIdx;
                        if (bestIdx !== -1) {
                            root.caretDisplayIndex = bestIdx;
                        }
                    }
                }
                
                onReleased: {
                    if (root.exchangeMode) return;
                    // Check if dropped on external target (e.g., board DropArea)
                    if (tileDelegate.Drag.target) {
                        tileDelegate.Drag.drop();
                        // Don't process internal rack reorder
                        tileDelegate.isHeld = false;
                        root.dropIndex = -1;
                        root.draggingIndex = -1;
                        return;
                    }

                    tileDelegate.isHeld = false;

                    if (root.dropIndex !== -1) {
                        var arr = root.internalRack.split('');
                        var charToMove = arr[root.draggingIndex];

                        arr.splice(root.draggingIndex, 1);

                        var insertAt = root.dropIndex;
                        if (root.draggingIndex < insertAt) insertAt--;

                        arr.splice(insertAt, 0, charToMove);

                        root.internalRack = arr.join('');
                    }

                    root.dropIndex = -1;
                    root.draggingIndex = -1;
                }
            }
        }
    }
    
    // Shuffle Button
    Button {
        anchors.right: parent.left
        anchors.verticalCenter: parent.verticalCenter
        anchors.rightMargin: root.tileSpacing * 2.5
        width: root.tileSize
        height: root.tileSize
        
        background: Rectangle {
            color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
            radius: width / 2
        }
        
        contentItem: Item {
            Canvas {
                anchors.centerIn: parent
                width: parent.parent.width * 0.6
                height: parent.parent.height * 0.6
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.strokeStyle = "white";
                    ctx.lineWidth = width * 0.08;
                    ctx.lineCap = "round";
                    
                    var s = width;
                    ctx.clearRect(0, 0, width, height);

                    ctx.beginPath();
                    ctx.moveTo(s*0.2, s*0.2);
                    ctx.lineTo(s*0.8, s*0.8);
                    ctx.moveTo(s*0.8, s*0.2);
                    ctx.lineTo(s*0.2, s*0.8);
                    ctx.stroke();
                    
                    // Arrowheads
                    // (Simplified shuffle icon)
                    ctx.beginPath();
                    ctx.moveTo(s*0.65, s*0.8);
                    ctx.lineTo(s*0.8, s*0.8);
                    ctx.lineTo(s*0.8, s*0.65);
                    ctx.stroke();
                    
                    ctx.beginPath();
                    ctx.moveTo(s*0.65, s*0.2);
                    ctx.lineTo(s*0.8, s*0.2);
                    ctx.lineTo(s*0.8, s*0.35);
                    ctx.stroke();
                }
                // Repaint when size changes
                onWidthChanged: requestPaint()
            }
        }
        onClicked: {
             var arr = root.internalRack.trim().split('');
             for (var i = arr.length - 1; i > 0; i--) {
                 var j = Math.floor(Math.random() * (i + 1));
                 var temp = arr[i];
                 arr[i] = arr[j];
                 arr[j] = temp;
             }
             var r = arr.join('');
             while (r.length < 7) r += " ";
             root.internalRack = r;
        }
    }

    // Sort Button
    Button {
        anchors.left: parent.right
        anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: root.tileSpacing * 2.5
        width: root.tileSize
        height: root.tileSize
        
        background: Rectangle {
            color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
            radius: width / 2
        }
        
        contentItem: Item {
            Canvas {
                anchors.centerIn: parent
                width: parent.parent.width * 0.6
                height: parent.parent.height * 0.6
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.fillStyle = "white";
                    ctx.clearRect(0, 0, width, height);
                    
                    var w = width;
                    var h = height;
                    
                    ctx.fillRect(w*0.2, h*0.65, w*0.15, h*0.15);
                    ctx.fillRect(w*0.45, h*0.5, w*0.15, h*0.30);
                    ctx.fillRect(w*0.7, h*0.35, w*0.15, h*0.45);
                }
                onWidthChanged: requestPaint()
            }
        }
        onClicked: {
            var arr = root.internalRack.trim().split('');
            arr.sort();
            var r = arr.join('');
            while (r.length < 7) r += " ";
            root.internalRack = r;
        }
    }
}