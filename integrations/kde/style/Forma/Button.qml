// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Controls.impl
Basic.Button {
    id: control
    property string formaDisplayText: text
    Accessible.name: formaDisplayText
    property bool formaProminent: highlighted || checked
    font.pixelSize: 13
    font.weight: Font.Medium
    padding: 7
    horizontalPadding: 18
    spacing: 7
    contentItem: IconLabel {
        spacing: control.spacing
        mirrored: control.mirrored
        display: control.display
        icon: control.icon
        text: control.formaDisplayText
        font: control.font
        color: control.formaProminent ? control.palette.highlightedText : control.palette.buttonText
    }
    opacity: enabled ? 1 : 0.42
    background: Rectangle {
        implicitWidth: 78; implicitHeight: 32
        radius: height / 2
        color: control.formaProminent ? (control.down ? Qt.darker(control.palette.highlight, 1.16) : control.palette.highlight)
            : control.down ? control.palette.midlight : control.hovered ? control.palette.light : control.palette.button
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.palette.highlight : Qt.alpha(control.palette.text, 0.13)
        Behavior on color { ColorAnimation { duration: 100 } }
    }
}
