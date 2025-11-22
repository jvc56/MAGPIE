import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    color: "#313244"
    radius: 10
    clip: true

    property var analysisModel
    property string lexiconName: "CSW24"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 10

        // Header: Lexicon + Status
        RowLayout {
            Layout.fillWidth: true
            
            Text {
                text: lexiconName
                color: "#A6ADC8"
                font.pixelSize: 14
                font.bold: true
            }
            
            Item { Layout.fillWidth: true }
            
            RowLayout {
                spacing: 5
                visible: analysisModel && analysisModel.isRunning
                
                // Braille spinner
                property int frame: 0
                property var brailleChars: ["⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"]
                Timer {
                    interval: 80
                    running: parent.visible
                    repeat: true
                    onTriggered: parent.frame = (parent.frame + 1) % parent.brailleChars.length
                }
                
                Text {
                    text: parent.brailleChars[parent.frame] + " Simulating"
                    color: "#A6ADC8"
                    font.pixelSize: 12
                    font.family: "Consolas"
                }
            }
            
            Text {
                text: analysisModel ? (!analysisModel.isRunning ? "Ready" : "") : ""
                color: "#A6ADC8"
                font.pixelSize: 12
                font.family: "Consolas"
                visible: analysisModel && !analysisModel.isRunning
            }
        }

        // Header: Plies + Confidence
        RowLayout {
            Layout.fillWidth: true
            
            Text {
                text: analysisModel ? analysisModel.plies + " ply" : ""
                color: "#A6ADC8"
                font.pixelSize: 12
                font.family: "Consolas"
                visible: analysisModel
            }
            
            Item { Layout.fillWidth: true }

            Text {
                text: (analysisModel && analysisModel.confidence >= 0.8) ? "conf: " + (analysisModel.confidence * 100).toFixed(1) + "%" : ""
                color: "#A6ADC8"
                font.pixelSize: 12
                font.family: "Consolas"
                visible: analysisModel && analysisModel.confidence >= 0.8
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: "#585B70"
        }

        // Table Header
        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            
            Text {
                text: "#"
                color: "#7F849C"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 12
                Layout.preferredWidth: 35
                horizontalAlignment: Text.AlignRight
            }
            Text {
                text: "Play"
                color: "#7F849C"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 12
                Layout.fillWidth: true
                Layout.minimumWidth: 80
                leftPadding: 10
            }
            Text {
                text: "Win%"
                color: "#7F849C"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 12
                Layout.preferredWidth: 70
                horizontalAlignment: Text.AlignRight
            }
            Text {
                text: "Spread"
                color: "#7F849C"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 12
                Layout.preferredWidth: 70
                horizontalAlignment: Text.AlignRight
            }
            Text {
                text: "Iters"
                color: "#7F849C"
                font.bold: true
                font.family: "Consolas"
                font.pixelSize: 12
                Layout.preferredWidth: 50
                horizontalAlignment: Text.AlignRight
                rightPadding: 10
            }
        }

        // List
        ListView {
            id: analysisList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            
            model: analysisModel
            
            delegate: Rectangle {
                width: analysisList.width
                height: 24
                color: index % 2 === 0 ? "transparent" : "#383B4E"
                
                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    
                    Text {
                        text: model.rank
                        color: "#CDD6F4"
                        font.pixelSize: 12
                        font.family: "Consolas"
                        Layout.preferredWidth: 35
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        text: model.notation
                        color: "#CDD6F4"
                        font.pixelSize: 13
                        font.family: "Consolas"
                        Layout.fillWidth: true
                        Layout.minimumWidth: 80
                        elide: Text.ElideRight
                        leftPadding: 10
                    }
                    Text {
                        text: model.winPct.toFixed(2)
                        color: "#CDD6F4"
                        font.pixelSize: 12
                        font.family: "Consolas"
                        Layout.preferredWidth: 70
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        text: model.spread.toFixed(2)
                        color: "#CDD6F4"
                        font.pixelSize: 12
                        font.family: "Consolas"
                        Layout.preferredWidth: 70
                        horizontalAlignment: Text.AlignRight
                    }
                    Text {
                        text: model.iterations
                        color: "#CDD6F4"
                        font.pixelSize: 12
                        font.family: "Consolas"
                        Layout.preferredWidth: 50
                        horizontalAlignment: Text.AlignRight
                        rightPadding: 10
                    }
                }
            }

            ScrollBar.vertical: ScrollBar {
                active: true
                width: 8
                background: Rectangle { color: "transparent" }
                contentItem: Rectangle { color: "#585B70"; radius: 4; opacity: 0.5 }
            }
        }
    }
}
