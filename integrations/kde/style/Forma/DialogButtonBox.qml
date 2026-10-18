// SPDX-License-Identifier: LGPL-2.0-or-later
import QtQuick
import QtQuick.Controls.Basic as Basic
import QtQuick.Templates as T
Basic.DialogButtonBox {
    id: box
    property string formaAcceptLabel: ""
    property string formaRejectLabel: ""
    spacing: 10
    padding: 0
    alignment: Qt.AlignRight
    background: Item {}
    delegate: Button {
        // Button roles remain authoritative; never turn Deny/Cancel into the default action.
        formaProminent: T.DialogButtonBox.buttonRole === T.DialogButtonBox.AcceptRole
            || T.DialogButtonBox.buttonRole === T.DialogButtonBox.YesRole
        formaDisplayText: {
            const role = T.DialogButtonBox.buttonRole;
            if ((role === T.DialogButtonBox.AcceptRole || role === T.DialogButtonBox.YesRole) && box.formaAcceptLabel.length > 0)
                return box.formaAcceptLabel;
            if ((role === T.DialogButtonBox.RejectRole || role === T.DialogButtonBox.NoRole) && box.formaRejectLabel.length > 0)
                return box.formaRejectLabel;
            return text;
        }
    }
}
