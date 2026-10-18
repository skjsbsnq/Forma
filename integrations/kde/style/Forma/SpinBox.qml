// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.SpinBox {
    id: control
    font.pixelSize: 13; padding: 6
    background: Rectangle {
        implicitHeight: 34; radius: 10; color: control.palette.base
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.palette.highlight : Qt.alpha(control.palette.text, 0.15)
    }
}
