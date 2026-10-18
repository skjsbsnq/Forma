// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.RadioButton {
    id: control
    font.pixelSize: 13; padding: 5; spacing: 8
    opacity: enabled ? 1 : 0.42
    indicator: Rectangle {
        implicitWidth: 18; implicitHeight: 18; radius: 9
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        color: control.checked ? control.palette.highlight : control.palette.base
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus || control.checked ? control.palette.highlight : Qt.alpha(control.palette.text, 0.25)
        Rectangle {
            anchors.centerIn: parent
            width: 7; height: 7; radius: 4
            visible: control.checked; color: control.palette.highlightedText
        }
    }
}
