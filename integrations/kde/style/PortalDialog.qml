/*
 * SPDX-License-Identifier: LGPL-2.0-or-later
 * Forma window shell. Keeps the KDE portal's public properties and signals intact.
 */
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import QtQuick.Templates as T
import FormaPortal 1.0
import Forma 1.0 as FormaStyle
import org.kde.kirigami as Kirigami

pragma ComponentBehavior: Bound

Kirigami.AbstractApplicationWindow {
    id: root
    title: mainText
    property string mainText: title
    property string subtitle: ""
    property string iconName: ""
    property list<T.Action> actions
    default property Item mainItem
    property bool scrollable: false
    property Item headerItem
    property Item footerItem
    readonly property QQC2.DialogButtonBox dialogButtonBox: buttons
    property var standardButtons: QQC2.DialogButtonBox.Ok | QQC2.DialogButtonBox.Cancel
    property Item dialogButtonBoxLeftItem
    property bool acceptable: true
    property string acceptButtonText: ""
    property string rejectButtonText: ""
    property int layout: actions.length > 3 ? Qt.Vertical : Qt.Horizontal
    property bool contentPadding: true
    readonly property real edgeSpacing: 24
    readonly property real contentWidth: Math.max(480, header.implicitWidth + 48,
        (root.mainItem?.implicitWidth ?? 0) + (root.contentPadding ? 48 : 0),
        footer.implicitWidth + 48)
    readonly property real contentHeight: chrome.height + column.implicitHeight + 16
    property bool accepted: false
    property bool formaFinished: false
    property bool formaPresented: false

    flags: Qt.Dialog | Qt.FramelessWindowHint
    color: "transparent"
    visible: false
    minimumWidth: 380
    minimumHeight: 180
    font.pixelSize: 13
    palette.window: Theme.colors.window
    palette.base: Theme.colors.base
    palette.alternateBase: Theme.colors.alternateBase
    palette.windowText: Theme.colors.text
    palette.text: Theme.colors.text
    palette.buttonText: Theme.colors.text
    palette.button: Theme.colors.button
    palette.light: Theme.colors.light
    palette.midlight: Theme.colors.midlight
    palette.mid: Theme.colors.mid
    palette.dark: Theme.colors.dark
    palette.highlight: Theme.colors.highlight
    palette.highlightedText: Theme.colors.highlightedText
    palette.placeholderText: Theme.colors.placeholderText
    Kirigami.Theme.inherit: false
    Kirigami.Theme.backgroundColor: Theme.colors.window
    Kirigami.Theme.textColor: Theme.colors.text
    Kirigami.Theme.highlightColor: Theme.colors.highlight
    Kirigami.Theme.highlightedTextColor: Theme.colors.highlightedText
    Kirigami.Theme.alternateBackgroundColor: Theme.colors.alternateBase

    function present(): void { formaPresented = true; show(); }
    signal accept()
    signal reject()
    onAccept: { accepted = true; formaFinished = true; close(); }
    onReject: { formaFinished = true; close(); }
    onClosing: close => { if (!formaFinished) root.reject(); }
    onVisibleChanged: { if (!visible && formaPresented && !formaFinished) root.reject(); }

    background: Rectangle {
        radius: root.visibility === Window.Maximized ? 0 : 20
        color: root.palette.window
        border.width: 1
        border.color: Qt.alpha(root.palette.windowText, 0.15)
        Rectangle {
            x: 1; y: 1; width: parent.width - 2; height: Math.min(110, parent.height - 2)
            radius: parent.radius
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.alpha("white", 0.09) }
                GradientStop { position: 1; color: "transparent" }
            }
        }
    }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onEnterPressed: { if (root.acceptable) root.accept(); }
        Keys.onReturnPressed: { if (root.acceptable) root.accept(); }
        Keys.onEscapePressed: root.reject()

        Item {
            id: chrome
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            height: 46
            MouseArea {
                anchors.fill: parent
                onPressed: root.startSystemMove()
            }
            Row {
                anchors.left: parent.left; anchors.leftMargin: 17
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                Repeater {
                    model: ["#ff5f57", "#febc2e", "#28c840"]
                    delegate: Rectangle {
                        id: dot
                        required property string modelData
                        required property int index
                        width: 13; height: 13; radius: 7
                        color: modelData
                        border.width: 1; border.color: Qt.alpha("black", 0.08)
                        Accessible.role: Accessible.Button
                        Accessible.name: index === 0 ? qsTr("Close") : index === 1 ? qsTr("Minimize") : qsTr("Zoom")
                        Accessible.onPressAction: clickArea.clicked(null)
                        QQC2.ToolTip.text: Accessible.name
                        QQC2.ToolTip.visible: clickArea.containsMouse
                        Text {
                            anchors.centerIn: parent
                            text: dot.index === 0 ? "×" : dot.index === 1 ? "−" : "+"
                            font.pixelSize: 12; color: "#47251c"
                            visible: clickArea.containsMouse
                        }
                        MouseArea {
                            id: clickArea
                            anchors.fill: parent; hoverEnabled: true
                            onClicked: {
                                if (dot.index === 0) root.reject();
                                else if (dot.index === 1) root.showMinimized();
                                else root.visibility === Window.Maximized ? root.showNormal() : root.showMaximized();
                            }
                        }
                    }
                }
            }
            QQC2.Label {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - 190)
                text: root.title
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                font.pixelSize: 13; font.weight: Font.DemiBold
                textFormat: Text.PlainText
            }
        }

        ColumnLayout {
            id: column
            anchors.top: chrome.bottom; anchors.bottom: parent.bottom
            anchors.left: parent.left; anchors.right: parent.right
            anchors.margins: 1
            spacing: 0

            QQC2.Control {
                id: header
                Layout.fillWidth: true
                padding: root.edgeSpacing
                topPadding: 12
                visible: root.mainText.length > 0 || root.subtitle.length > 0 || root.headerItem !== null
                contentItem: ColumnLayout {
                    spacing: 16
                    RowLayout {
                        spacing: 16
                        Layout.fillWidth: true
                        Kirigami.Icon {
                            visible: root.iconName.length > 0
                            source: root.iconName
                            implicitWidth: 40; implicitHeight: 40
                        }
                        ColumnLayout {
                            spacing: 8
                            Layout.fillWidth: true
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: root.mainText
                                visible: text.length > 0
                                font.pixelSize: 20; font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                                textFormat: Text.RichText
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                text: root.subtitle
                                visible: text.length > 0
                                color: Qt.alpha(root.palette.windowText, 0.65)
                                wrapMode: Text.WordWrap
                                textFormat: Text.RichText
                            }
                        }
                    }
                    QQC2.Control {
                        Layout.fillWidth: true
                        padding: 0
                        visible: root.headerItem?.visible ?? false
                        contentItem: root.headerItem
                    }
                }
            }

            Loader {
                id: body
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: root.mainItem !== null
                sourceComponent: root.scrollable ? scrollComponent : contentComponent
            }
            Component {
                id: contentComponent
                QQC2.Control {
                    padding: root.contentPadding ? root.edgeSpacing : 0
                    topPadding: root.contentPadding ? 12 : 0
                    bottomPadding: root.contentPadding ? 12 : 0
                    contentItem: root.mainItem
                    background: Rectangle { color: Qt.alpha(root.palette.base, 0.5) }
                }
            }
            Component {
                id: scrollComponent
                QQC2.ScrollView {
                    implicitHeight: Math.min(420, scrollBody.implicitHeight)
                    contentWidth: availableWidth
                    clip: true
                    QQC2.Control {
                        id: scrollBody
                        width: parent.width
                        padding: root.contentPadding ? root.edgeSpacing : 0
                        contentItem: root.mainItem
                    }
                    background: Rectangle { color: Qt.alpha(root.palette.base, 0.5) }
                }
            }

            QQC2.Control {
                id: footer
                Layout.fillWidth: true
                padding: root.edgeSpacing
                topPadding: 16; bottomPadding: 16
                contentItem: ColumnLayout {
                    spacing: 16
                    QQC2.Control {
                        Layout.fillWidth: true
                        padding: 0
                        visible: root.footerItem?.visible ?? false
                        contentItem: root.footerItem
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16
                        QQC2.Control {
                            visible: root.dialogButtonBoxLeftItem?.visible ?? false
                            padding: 0
                            contentItem: root.dialogButtonBoxLeftItem
                        }
                        Item { Layout.fillWidth: true }
                        FormaStyle.DialogButtonBox {
                            id: buttons
                            formaAcceptLabel: root.acceptButtonText
                            formaRejectLabel: root.rejectButtonText
                            standardButtons: root.standardButtons
                            onAccepted: { if (root.acceptable) root.accept(); }
                            onRejected: root.reject()
                            Repeater {
                                model: root.actions
                                delegate: QQC2.Button {
                                    required property var modelData
                                    action: modelData
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    Binding {
        target: root.dialogButtonBox?.standardButton(QQC2.DialogButtonBox.Ok) ?? null
        property: "enabled"
        when: (root.standardButtons & QQC2.DialogButtonBox.Ok) !== 0
        value: root.acceptable
    }
}
