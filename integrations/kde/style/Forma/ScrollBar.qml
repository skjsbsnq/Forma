// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.ScrollBar {
    id: control
    padding: 2
    contentItem: Rectangle {
        implicitWidth: 6; implicitHeight: 6; radius: 3
        color: control.palette.text
        opacity: control.pressed ? 0.55 : control.active ? 0.3 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }
    background: Item {}
}
