import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

Rectangle {
    id: root
    color: "#1E1E2E"
    
    signal startRequested(string lexicon, int timeMinutes)
    
    ColumnLayout {
        anchors.centerIn: parent
        spacing: 30
        
        Text {
            text: "MAGPIE"
            color: "#89B4FA"
            font.pixelSize: 48
            font.bold: true
            Layout.alignment: Qt.AlignHCenter
        }
        
        Text {
            text: "Select Game Configuration"
            color: "#CDD6F4"
            font.pixelSize: 18
            Layout.alignment: Qt.AlignHCenter
        }
        
        // Lexicon Selection
        ColumnLayout {
            spacing: 10
            Layout.alignment: Qt.AlignHCenter
            
            Text {
                text: "Lexicon"
                color: "#A6ADC8"
                font.pixelSize: 14
            }
            
            ComboBox {
                id: lexiconCombo
                model: ["CSW24", "CSW21", "NWL23"]
                currentIndex: 0
                implicitWidth: 200
                
                contentItem: Text {
                    text: lexiconCombo.displayText
                    color: "#CDD6F4"
                    font.pixelSize: 14
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 10
                }
                
                background: Rectangle {
                    color: lexiconCombo.hovered || lexiconCombo.activeFocus ? "#45475A" : "#313244"
                    border.color: lexiconCombo.activeFocus ? "#89B4FA" : "transparent"
                    radius: 8
                    Behavior on color { ColorAnimation { duration: 100 } }
                }
            }
        }
        
        // Time Control Selection
        ColumnLayout {
            spacing: 10
            Layout.alignment: Qt.AlignHCenter
            
            Text {
                text: "Time Control"
                color: "#A6ADC8"
                font.pixelSize: 14
            }
            
            RowLayout {
                spacing: 15
                
                Repeater {
                    model: [5, 10, 15, 25]
                    delegate: Button {
                        id: timeBtn
                        text: modelData + "m"
                        property bool isSelected: timeModel.value === modelData
                        
                        onClicked: timeModel.value = modelData
                        
                        contentItem: Text {
                            text: timeBtn.text
                            color: timeBtn.isSelected ? "#1E1E2E" : "#CDD6F4"
                            font.pixelSize: 14
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        
                        background: Rectangle {
                            implicitWidth: 60
                            implicitHeight: 40
                            color: timeBtn.isSelected ? "#89B4FA" : (timeBtn.hovered ? "#45475A" : "#313244")
                            radius: 8
                            border.color: timeBtn.hovered && !timeBtn.isSelected ? "#89B4FA" : "transparent"
                            border.width: 1
                            Behavior on color { ColorAnimation { duration: 100 } }
                        }
                    }
                }
            }
            
            QtObject {
                id: timeModel
                property int value: 25
            }
        }
        
        Button {
            text: "Start Game"
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 20
            
            onClicked: root.startRequested(lexiconCombo.currentText, timeModel.value)
            
            contentItem: Text {
                text: parent.text
                color: "#1E1E2E"
                font.pixelSize: 18
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                padding: 10
            }
            
            background: Rectangle {
                implicitWidth: 200
                implicitHeight: 50
                color: parent.down ? "#98D395" : (parent.hovered ? "#C1FFBC" : "#A6E3A1")
                radius: 12
                scale: parent.pressed ? 0.95 : (parent.hovered ? 1.05 : 1.0)
                border.color: parent.hovered ? "#FFFFFF" : "transparent"
                border.width: parent.hovered ? 2 : 0
                
                layer.enabled: true
                Behavior on color { ColorAnimation { duration: 100 } }
                Behavior on scale { NumberAnimation { duration: 100 } }
                Behavior on border.width { NumberAnimation { duration: 100 } }
            }
        }

        Text {
            text: "Or open a GCG file to review"
            font.pixelSize: 12
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 20
            
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: gameModel.openGameDialog()
            }
            color: parent.hovered ? "#89B4FA" : "#6C7086"
            Behavior on color { ColorAnimation { duration: 100 } }
        }
    }
}
