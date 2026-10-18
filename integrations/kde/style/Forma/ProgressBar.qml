// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.ProgressBar {
    id: control
    background: Rectangle { implicitWidth: 180; implicitHeight: 5; radius: 3; color: control.palette.midlight }
    // Keep Basic's indeterminate animation and accessibility behavior.
}
