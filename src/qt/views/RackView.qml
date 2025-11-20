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
    
    // Constants
    readonly property int tileSpacing: 4

    // Sync external rack to internal rack (padding to 7)
    onRackChanged: {
        var r = rack;
        if (r === undefined || r === null) r = "";
        while (r.length < 7) r += " ";
        internalRack = r;
    }
    
    Component.onCompleted: {
        var r = rack;
        if (r === undefined || r === null) r = "";
        while (r.length < 7) r += " ";
        internalRack = r;
    }
    
    width: 400
    height: 60
    
    Rectangle {
        anchors.fill: parent
        color: "#232433" // Slightly lighter than main background
        radius: 8
    }
    
    function getSlotX(index) {
        var totalWidth = 7 * tileSize + 6 * tileSpacing;
        var startX = (root.width - totalWidth) / 2;
        return startX + index * (tileSize + tileSpacing);
    }

    // Drop Indicator (Caret)
    Item {
        id: caret
        visible: root.dropIndex !== -1
        
        width: 14 // Width of caps
        height: root.tileSize + 8 // Taller than tile
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
            width: 3
            height: parent.height
            color: "#6496FF"
            radius: 1.5
        }
        
        // Top Cap
        Rectangle {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            height: 3
            color: "#6496FF"
            radius: 1.5
        }
        
        // Bottom Cap
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            width: parent.width
            height: 3
            color: "#6496FF"
            radius: 1.5
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
            
            // Centered vertically
            y: (root.height - height) / 2
            
            // X position
            x: getSlotX(index)
            
            onIsHeldChanged: {
                if (!isHeld) {
                    x = Qt.binding(function() { return root.getSlotX(index) });
                    y = (root.height - height) / 2;
                }
            }

            z: isHeld ? 100 : 1
            
            // Tile Visual
            Rectangle {
                anchors.fill: parent
                radius: 4
                
                // Shadow for dragged tile
                Rectangle {
                    anchors.fill: parent
                    anchors.topMargin: 4
                    anchors.leftMargin: 4
                    color: "black"
                    opacity: 0.2
                    radius: 4
                    z: -1
                    visible: tileDelegate.isHeld
                }

                color: {
                    if (charStr === " ") return "#313244"; // Empty
                    return "#F9E2AF"; // Tile color
                }
                
                // Style changes during drag
                opacity: tileDelegate.isHeld ? 0.9 : 1.0
                scale: tileDelegate.isHeld ? 1.15 : 1.0
                Behavior on scale { NumberAnimation { duration: 100 } }

                Text {
                    anchors.centerIn: parent
                    text: {
                        if (charStr === "?" || charStr === " ") return "";
                        return charStr.toUpperCase();
                    }
                    font.family: "Clear Sans"
                    font.pixelSize: 24
                    font.bold: true
                    color: "#1E1E2E"
                    visible: charStr !== " "
                }
                
                // Blank designation (small centered box to match board style)
                Rectangle {
                    width: parent.width * 0.5
                    height: parent.height * 0.5
                    anchors.centerIn: parent
                    color: "transparent"
                    border.color: "black"
                    border.width: 1
                    // Fix: Use 'isBlank' from scope (tileDelegate), NOT 'parent.isBlank'
                    visible: isBlank && charStr !== "?" && charStr !== " "
                }

                // Undesignated blank
                Text {
                    anchors.centerIn: parent
                    text: "?"
                    font.family: "Clear Sans"
                    font.pixelSize: 24
                    font.bold: true
                    color: "#1E1E2E"
                    visible: charStr === "?"
                }
                
                // Score
                Text {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: 3
                    anchors.bottomMargin: 2
                    font.family: "Clear Sans"
                    font.pixelSize: 10
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
                drag.target: tileDelegate
                drag.smoothed: false
                drag.threshold: 5
                
                enabled: charStr !== " "
                
                onPressed: {
                    tileDelegate.isHeld = true;
                    root.draggingIndex = index;
                }
                
                onPositionChanged: {
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
        anchors.rightMargin: 10
        width: 40
        height: 40
        
        background: Rectangle {
            color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
            radius: 20
        }
        
        contentItem: Item {
            Canvas {
                anchors.centerIn: parent
                width: 24
                height: 24
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.strokeStyle = "white";
                    ctx.lineWidth = 2;
                    ctx.lineCap = "round";
                    
                    ctx.beginPath();
                    ctx.moveTo(4, 4);
                    ctx.lineTo(20, 20);
                    ctx.moveTo(20, 4);
                    ctx.lineTo(4, 20);
                    ctx.stroke();
                    
                    ctx.beginPath();
                    ctx.moveTo(16, 20);
                    ctx.lineTo(20, 20);
                    ctx.lineTo(20, 16);
                    ctx.stroke();
                    
                    ctx.beginPath();
                    ctx.moveTo(16, 4);
                    ctx.lineTo(20, 4);
                    ctx.lineTo(20, 8);
                    ctx.stroke();
                }
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
        anchors.leftMargin: 10
        width: 40
        height: 40
        
        background: Rectangle {
            color: parent.down ? "#4070A0" : (parent.hovered ? "#5080B0" : "#6496C8")
            radius: 20
        }
        
        contentItem: Item {
            Canvas {
                anchors.centerIn: parent
                width: 24
                height: 24
                onPaint: {
                    var ctx = getContext("2d");
                    ctx.fillStyle = "white";
                    
                    ctx.fillRect(4, 16, 4, 4);
                    ctx.fillRect(10, 12, 4, 8);
                    ctx.fillRect(16, 8, 4, 12);
                }
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