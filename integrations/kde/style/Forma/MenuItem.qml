// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.MenuItem {
    id: control
    font.pixelSize: 13; padding: 8
    background: Rectangle { implicitHeight: 32; radius: 7; color: control.highlighted ? control.palette.highlight : "transparent" }
}
