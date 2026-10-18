// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.Switch {
    id: control
    font.pixelSize: 13; padding: 5; spacing: 8
    opacity: enabled ? 1 : 0.42
    indicator: Rectangle {
        implicitWidth: 36; implicitHeight: 22; radius: 11
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        color: control.checked ? control.palette.highlight : control.palette.mid
        border.width: control.visualFocus ? 2 : 0; border.color: control.palette.highlight
        Rectangle {
            x: 2 + (parent.width - width - 4) * control.visualPosition
            y: 2; width: 18; height: 18; radius: 9; color: "white"
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
    }
}
