// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
Basic.ComboBox {
    id: control
    font.pixelSize: 13
    padding: 6
    opacity: enabled ? 1 : 0.42
    delegate: ItemDelegate {
        required property var model
        required property int index
        width: ListView.view.width
        text: control.textRole ? model[control.textRole] : model.modelData
        highlighted: control.highlightedIndex === index
        font.weight: control.currentIndex === index ? Font.DemiBold : Font.Normal
    }
    popup: Basic.Popup {
        y: control.height + 6
        width: control.width
        padding: 6
        implicitHeight: Math.min(contentItem.implicitHeight + 12, 320)
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            Basic.ScrollBar.vertical: ScrollBar {}
        }
        background: Rectangle {
            radius: 12; color: control.palette.window
            border.width: 1; border.color: Qt.alpha(control.palette.text, 0.15)
        }
    }
    background: Rectangle {
        implicitWidth: 160; implicitHeight: 34; radius: 10
        color: control.down ? control.palette.midlight : control.palette.button
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.palette.highlight : Qt.alpha(control.palette.text, 0.13)
    }
}
