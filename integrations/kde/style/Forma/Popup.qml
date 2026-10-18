// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.Popup {
    id: control
    padding: 8
    background: Rectangle { radius: 14; color: control.palette.window; border.width: 1; border.color: Qt.alpha(control.palette.text, 0.15) }
}
