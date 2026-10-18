// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.ToolButton {
    id: control
    font.pixelSize: 13
    padding: 7
    opacity: enabled ? 1 : 0.42
    background: Rectangle {
        implicitWidth: 32; implicitHeight: 32; radius: 10
        color: control.checked ? Qt.alpha(control.palette.highlight, 0.16)
            : control.down ? control.palette.midlight : control.hovered ? control.palette.button : "transparent"
        border.width: control.visualFocus ? 2 : 0
        border.color: control.palette.highlight
    }
}
