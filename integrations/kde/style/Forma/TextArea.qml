// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.TextArea {
    id: control
    font.pixelSize: 13; padding: 12
    color: palette.text
    selectByMouse: true
    background: Rectangle {
        radius: 12; color: control.palette.base
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? control.palette.highlight : Qt.alpha(control.palette.text, 0.13)
    }
}
