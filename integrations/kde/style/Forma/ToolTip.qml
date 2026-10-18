// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.ToolTip {
    id: control
    font.pixelSize: 11; padding: 8
    background: Rectangle {
        radius: 8; color: control.palette.window
        border.width: 1; border.color: Qt.alpha(control.palette.text, 0.15)
    }
}
