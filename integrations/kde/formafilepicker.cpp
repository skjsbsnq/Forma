// SPDX-License-Identifier: LGPL-2.0-or-later
#include "formafilepicker.h"
#include "filechooser.h"
#include "request.h"
#include <QDBusMetaType>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

Q_DECLARE_METATYPE(FileChooserPortal::Filter)
Q_DECLARE_METATYPE(FileChooserPortal::Filters)
Q_DECLARE_METATYPE(FileChooserPortal::FilterList)
Q_DECLARE_METATYPE(FileChooserPortal::FilterListList)

const QDBusArgument &operator>>(const QDBusArgument &, FileChooserPortal::Filter &);
const QDBusArgument &operator>>(const QDBusArgument &, FileChooserPortal::FilterList &);

bool runFormaFilePicker(const QDBusObjectPath &handle,
                        const QString &appId,
                        const QString &parentWindow,
                        const QString &title,
                        const QVariantMap &options,
                        bool save,
                        uint &response,
                        QVariantMap &results)
{
    // Opt-in only; an unmodified KDE installation remains the default.
    if (!qEnvironmentVariableIsSet("FORMA_PORTAL_PICKER"))
        return false;
    // Extra application controls remain with KDE until Forma can represent them.
    if (options.contains(QStringLiteral("choices")))
        return false;
    QString executable = qEnvironmentVariable("FORMA_PORTAL_PICKER");
    if (executable.isEmpty())
        executable = QStandardPaths::findExecutable(QStringLiteral("forma-file-picker"));
    if (executable.isEmpty() || !QFileInfo(executable).isExecutable())
        return false;

    const auto filters = qdbus_cast<FileChooserPortal::FilterListList>(options.value(QStringLiteral("filters")));
    const auto current = qdbus_cast<FileChooserPortal::FilterList>(options.value(QStringLiteral("current_filter")));
    QJsonArray jsonFilters;
    int filterIndex = 0;
    for (int i = 0; i < filters.size(); ++i) {
        const auto &filter = filters[i];
        QJsonArray terms;
        for (const auto &term : filter.filters)
            terms.append(QJsonObject{{QStringLiteral("type"), int(term.type)}, {QStringLiteral("value"), term.filterString}});
        jsonFilters.append(QJsonObject{{QStringLiteral("name"), filter.userVisibleName}, {QStringLiteral("terms"), terms}});
        if (filter.userVisibleName == current.userVisibleName)
            filterIndex = i;
    }
    auto decode = [&](const char *key) {
        QByteArray value = options.value(QLatin1String(key)).toByteArray();
        if (value.endsWith('\0'))
            value.chop(1);
        return QString::fromLocal8Bit(value);
    };
    QString folder = decode("current_folder");
    QString name = options.value(QStringLiteral("current_name")).toString();
    const QString currentFile = decode("current_file");
    if (!currentFile.isEmpty()) {
        folder = QFileInfo(currentFile).absolutePath();
        name = QFileInfo(currentFile).fileName();
    }
    QJsonObject input{{QStringLiteral("version"), 1},
                      {QStringLiteral("save"), save},
                      {QStringLiteral("title"), title},
                      {QStringLiteral("appId"), appId},
                      {QStringLiteral("parentWindow"), parentWindow},
                      {QStringLiteral("folder"), folder},
                      {QStringLiteral("name"), name},
                      {QStringLiteral("multiple"), options.value(QStringLiteral("multiple")).toBool()},
                      {QStringLiteral("directory"), options.value(QStringLiteral("directory")).toBool()},
                      {QStringLiteral("filters"), jsonFilters},
                      {QStringLiteral("filterIndex"), filterIndex}};
    if (options.contains(QStringLiteral("accept_label")))
        input.insert(QStringLiteral("acceptLabel"), options.value(QStringLiteral("accept_label")).toString().remove(QLatin1Char('_')));

    QProcess process;
    QEventLoop loop;
    QObject requestOwner;
    auto request = new Request(handle, &requestOwner);
    bool cancelled = false;
    QObject::connect(request, &Request::closeRequested, &loop, [&] {
        cancelled = true;
        process.kill();
        loop.quit();
    });
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&process, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    // Keep diagnostics away from the result pipe and never invoke a shell.
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    auto environment = QProcessEnvironment::systemEnvironment();
    if (environment.contains(QStringLiteral("FORMA_APPEARANCE")))
        environment.insert(QStringLiteral("MAYO_APPEARANCE"), environment.value(QStringLiteral("FORMA_APPEARANCE")));
    process.setProcessEnvironment(environment);
    process.start(executable, QStringList{});
    if (!process.waitForStarted(3000))
        return false;
    process.write(QJsonDocument(input).toJson(QJsonDocument::Compact) + '\n');
    process.closeWriteChannel();
    if (process.state() != QProcess::NotRunning)
        loop.exec();
    if (cancelled) {
        process.waitForFinished(1000);
        response = 1;
        results.clear();
        return true;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return false;
    const QByteArray output = process.readAllStandardOutput().trimmed();
    if (output.size() > 1024 * 1024)
        return false;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(output, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const auto result = document.object();
    const int code = result.value(QStringLiteral("response")).toInt(-1);
    if (result.value(QStringLiteral("version")).toInt() != 1 || (code != 0 && code != 1))
        return false;
    QVariantMap validated;
    if (code == 0) {
        const auto array = result.value(QStringLiteral("uris")).toArray();
        if (array.isEmpty() || ((save || !options.value(QStringLiteral("multiple")).toBool()) && array.size() != 1))
            return false;
        QStringList uris;
        for (const auto &value : array) {
            const QUrl url(value.toString(), QUrl::StrictMode);
            if (!url.isValid() || !url.isLocalFile() || !QFileInfo(url.toLocalFile()).isAbsolute())
                return false;
            uris.append(url.toString(QUrl::FullyEncoded));
        }
        validated.insert(QStringLiteral("uris"), uris);
        const int index = result.value(QStringLiteral("filterIndex")).toInt(-1);
        if (!filters.isEmpty()) {
            if (index < 0 || index >= filters.size())
                return false;
            validated.insert(QStringLiteral("current_filter"), QVariant::fromValue(filters[index]));
        }
    }
    response = uint(code);
    results = validated;
    return true;
}
