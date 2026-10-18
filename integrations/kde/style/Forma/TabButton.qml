// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.TabButton {
    id: control
    font.pixelSize: 13
    padding: 8
    background: Rectangle {
        implicitHeight: 34; radius: 9
        color: control.checked ? control.palette.button : control.hovered ? Qt.alpha(control.palette.text, 0.05) : "transparent"
        border.width: control.visualFocus ? 2 : 0; border.color: control.palette.highlight
    }
}
