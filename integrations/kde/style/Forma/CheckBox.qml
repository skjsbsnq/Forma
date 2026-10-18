// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.CheckBox {
    id: control
    font.pixelSize: 13
    padding: 5; spacing: 8
    opacity: enabled ? 1 : 0.42
    indicator: Rectangle {
        implicitWidth: 18; implicitHeight: 18; radius: 5
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        color: control.checkState !== Qt.Unchecked ? control.palette.highlight : control.palette.base
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus || control.checkState !== Qt.Unchecked ? control.palette.highlight : Qt.alpha(control.palette.text, 0.25)
        Text {
            anchors.centerIn: parent
            text: control.checkState === Qt.PartiallyChecked ? "−" : "✓"
            visible: control.checkState !== Qt.Unchecked
            font.pixelSize: 14; font.bold: true
            color: control.palette.highlightedText
        }
    }
}
