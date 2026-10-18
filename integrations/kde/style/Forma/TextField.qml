// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.TextField {
    id: control
    font.pixelSize: 13
    padding: 8
    leftPadding: 12; rightPadding: 12
    color: palette.text
    placeholderTextColor: palette.placeholderText
    selectByMouse: true
    background: Rectangle {
        implicitWidth: 180; implicitHeight: 34; radius: 10
        color: control.palette.base
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? control.palette.highlight : Qt.alpha(control.palette.text, 0.15)
    }
}
