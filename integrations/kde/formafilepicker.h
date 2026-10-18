// SPDX-License-Identifier: LGPL-2.0-or-later
#pragma once
#include <QDBusObjectPath>
#include <QVariantMap>

// False means unsupported request or helper failure: keep the original KDE dialog.
bool runFormaFilePicker(const QDBusObjectPath &handle,
                        const QString &appId,
                        const QString &parentWindow,
                        const QString &title,
                        const QVariantMap &options,
                        bool save,
                        uint &response,
                        QVariantMap &results);
