// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.ItemDelegate {
    id: control
    font.pixelSize: 13; padding: 10; spacing: 8
    opacity: enabled ? 1 : 0.42
    background: Rectangle {
        implicitHeight: 38; radius: 10
        color: control.highlighted ? Qt.alpha(control.palette.highlight, 0.16)
            : control.down ? control.palette.midlight : control.hovered ? Qt.alpha(control.palette.text, 0.06) : "transparent"
        border.width: control.visualFocus ? 2 : 0; border.color: control.palette.highlight
    }
}
