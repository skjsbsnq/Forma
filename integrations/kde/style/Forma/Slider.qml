// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.Slider {
    id: control
    background: Rectangle {
        x: control.leftPadding; y: control.topPadding + (control.availableHeight - height) / 2
        width: control.availableWidth; height: 5; radius: 3; color: control.palette.midlight
        Rectangle { width: parent.width * control.visualPosition; height: parent.height; radius: 3; color: control.palette.highlight }
    }
    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        implicitWidth: 20; implicitHeight: 20; radius: 10; color: control.palette.light
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.palette.highlight : Qt.alpha(control.palette.text, 0.18)
    }
}
