// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.Menu {
    id: control
    padding: 6
    background: Rectangle { implicitWidth: 180; radius: 12; color: control.palette.window; border.width: 1; border.color: Qt.alpha(control.palette.text, 0.15) }
}
