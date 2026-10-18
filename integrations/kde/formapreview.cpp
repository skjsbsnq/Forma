// SPDX-License-Identifier: LGPL-2.0-or-later
// Read-only previews and interaction checks: no portal service is registered and no request is granted.
#include "appchooserdialog.h"
#include "dbushelpers.h"
#include "formastyle.h"
#include "outputsmodel.h"
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPrintDialog>
#include <QPrinter>
#include <QQmlApplicationEngine>
#include <QQmlExpression>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QTest>

using namespace Qt::StringLiterals;

class FixtureModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(bool hasGlobalConflict MEMBER conflict CONSTANT)
    Q_PROPERTY(bool hasInternalConflict MEMBER conflict CONSTANT)
    Q_PROPERTY(bool hasSelection MEMBER conflict CONSTANT)
    Q_PROPERTY(int syntheticCount MEMBER count CONSTANT)
public:
    bool conflict = false;
    int count = 0;
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex & = {}) const override
    {
        return 0;
    }
    QVariant data(const QModelIndex &, int) const override
    {
        return {};
    }
    Q_INVOKABLE void clearSelection()
    {
    }
    Q_INVOKABLE bool geometryIntersects(const QModelIndex &, const QRect &) const
    {
        return false;
    }
};

class FixtureLauncher : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name MEMBER name NOTIFY nameChanged)
    Q_PROPERTY(QVariant icon MEMBER icon NOTIFY iconChanged)
public:
    QString name = QStringLiteral("Forma 文本编辑器");
    QVariant icon = QIcon::fromTheme(QStringLiteral("accessories-text-editor"));
    Q_SIGNAL void nameChanged();
    Q_SIGNAL void iconChanged();
};

int main(int argc, char **argv)
{
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    KLocalizedString::setApplicationDomain("xdg-desktop-portal-kde");
    if (QIcon::themeName().isEmpty())
        QIcon::setThemeName(QStringLiteral("breeze"));
    installFormaPortalStyle();
    const QString requested = argc > 1 ? QString::fromLocal8Bit(argv[1]) : u"AccessDialog"_s;
    const QString output = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString();
    const bool test = application.arguments().contains(u"--test"_s);
    if (!output.isEmpty())
        QDir().mkpath(QFileInfo(output).absolutePath());

    if (requested == u"print" || requested == u"background" || requested == u"file-fallback") {
        QWindow transientParent;
        transientParent.create();
        QPrinter printer;
        std::unique_ptr<QDialog> dialog;
        if (requested == u"print")
            dialog = std::make_unique<QPrintDialog>(&printer);
        else if (requested == u"file-fallback")
            dialog = std::make_unique<FileDialog>();
        else {
            auto message = std::make_unique<QMessageBox>(QMessageBox::Question, u"后台运行"_s, u"允许 Forma 邮件在没有打开窗口时继续接收新邮件吗？"_s);
            message->addButton(u"不允许"_s, QMessageBox::RejectRole);
            message->addButton(u"允许"_s, QMessageBox::AcceptRole);
            dialog = std::move(message);
        }
        dialog->winId();
        dialog->windowHandle()->setTransientParent(&transientParent);
        dialog->setWindowModality(Qt::WindowModal);
        dialog->show();
        QTest::qWait(350);
        if (!dialog->isModal() || dialog->windowHandle()->transientParent() != &transientParent)
            return 7;
        if (!dialog->property("formaDecorated").toBool())
            return 4;
        if (!output.isEmpty() && !dialog->grab().save(output))
            return 3;
        if (!test)
            return application.exec();
        QSignalSpy rejected(dialog.get(), &QDialog::rejected);
        dialog->reject();
        if (rejected.count() != 1)
            return 5;
        qInfo() << "PASS" << requested << "Forma chrome, rendering and rejection";
        return 0;
    }

    QQmlApplicationEngine engine;
    KLocalization::setupLocalizedContext(&engine);
    bool warnings = false;
    QObject::connect(&engine, &QQmlEngine::warnings, &application, [&](const QList<QQmlError> &list) {
        warnings = true;
        for (const auto &warning : list)
            qWarning().noquote() << warning.toString();
    });
    FixtureModel empty;
    FixtureLauncher launcher;
    AppModel apps;
    AppFilterModel appFilter;
    appFilter.setSourceModel(&apps);
    appFilter.setShowOnlyPreferredApps(false);
    AppChooserData appData;
    appData.setFileName(u"项目说明.txt"_s);
    appData.setMimeName(u"text/plain"_s);
    std::unique_ptr<OutputsModel> outputs;
    if (requested == u"ScreenChooserDialog")
        outputs = std::make_unique<OutputsModel>(OutputsModel::None, &application);
    QStandardItemModel screenshotTypes;
    screenshotTypes.appendRow(new QStandardItem(u"整个屏幕"_s));
    screenshotTypes.appendRow(new QStandardItem(u"当前窗口"_s));
    QVariantMap props;
    if (requested == u"AccessDialog") {
        OptionList choices;
        choices.append({u"remember"_s, u"记住本次选择"_s, {}, u"false"_s});
        props = {{u"title"_s, u"访问请求"_s},
                 {u"mainText"_s, u"允许 Forma 访问此设备？"_s},
                 {u"subtitle"_s, u"此请求来自 Forma 文本编辑器。"_s},
                 {u"body"_s, u"应用希望访问你选择的设备。你可以随时取消。"_s},
                 {u"acceptLabel"_s, u"允许"_s},
                 {u"rejectLabel"_s, u"不允许"_s},
                 {u"choices"_s, QVariant::fromValue(choices)}};
    } else if (requested == u"AppChooserDialog") {
        props = {{u"title"_s, u"打开方式"_s},
                 {u"mainText"_s, u"选择打开文件的应用"_s},
                 {u"appModel"_s, QVariant::fromValue(&appFilter)},
                 {u"appChooserData"_s, QVariant::fromValue(&appData)}};
    } else if (requested == u"UserInfoDialog") {
        props = {{u"realname"_s, u"Forma 用户"_s},
                 {u"username"_s, u"forma"_s},
                 {u"mainText"_s, u"允许共享账户信息？"_s},
                 {u"subtitle"_s, u"应用将获得你的显示名称与头像。"_s}};
    } else if (requested == u"RemoteDesktopDialog") {
        props = {{u"mainText"_s, u"允许 Forma 远程协助控制此设备？"_s}, {u"description"_s, u"应用请求使用键盘和鼠标。"_s}, {u"persistenceRequested"_s, true}};
    } else if (requested == u"InputCaptureDialog") {
        props = {{u"app"_s, u"Forma 共享"_s}, {u"persistenceRequested"_s, true}};
    } else if (requested == u"UsbDialog") {
        QVariantMap device{{u"properties"_s, QVariantMap{{u"ID_MODEL_FROM_DATABASE"_s, u"USB 相机"_s}, {u"ID_VENDOR_FROM_DATABASE"_s, u"Forma 示例设备"_s}}}};
        props = {{u"app"_s, u"Forma 照片"_s}, {u"devices"_s, QVariantList{device}}};
    } else if (requested == u"WallpaperDialog") {
        QImage wallpaper(640, 360, QImage::Format_RGB32);
        wallpaper.fill(QColor(u"#77a9da"_s));
        const QString image = QDir::tempPath() + u"/forma-portal-preview-wallpaper.png"_s;
        wallpaper.save(image);
        props = {{u"app"_s, u"Forma 设置"_s}, {u"location"_s, 1}, {u"image"_s, QUrl::fromLocalFile(image)}};
    } else if (requested == u"DynamicLauncherDialog") {
        props = {{u"title"_s, u"添加启动器"_s}, {u"mainText"_s, u"将应用添加到启动器？"_s}, {u"dialog"_s, QVariant::fromValue(&launcher)}};
    } else if (requested == u"GlobalShortcutsDialog") {
        props = {{u"app"_s, u"Forma 终端"_s},
                 {u"component"_s, u"forma-preview"_s},
                 {u"newShortcuts"_s, QVariant::fromValue(&empty)},
                 {u"returningShortcuts"_s, QVariantList{}}};
    } else if (requested == u"ScreenChooserDialog") {
        props = {{u"title"_s, u"共享屏幕"_s},
                 {u"mainText"_s, u"选择要共享的屏幕或窗口"_s},
                 {u"outputsModel"_s, QVariant::fromValue(outputs.get())},
                 {u"windowsModel"_s, QVariant::fromValue(&empty)}};
    } else if (requested == u"ScreenshotDialog") {
        props = {{u"appName"_s, u"Forma 截图"_s}, {u"screenshotTypesModel"_s, QVariant::fromValue(&screenshotTypes)}};
    } else
        return 2;
    engine.setInitialProperties(props);
    engine.loadFromModule(u"org.kde.xdgdesktopportal"_s, requested);
    if (engine.rootObjects().isEmpty())
        return 4;
    auto window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    if (!window)
        return 4;
    QMetaObject::invokeMethod(window, "present");
    QTest::qWait(500);
    QQmlExpression colorExpression(QQmlEngine::contextForObject(window), window, u"palette.window"_s);
    const QColor actualBackground = colorExpression.evaluate().value<QColor>();
    if (actualBackground != application.palette().color(QPalette::Window)) {
        qWarning() << "Incorrect appearance" << actualBackground << application.palette().color(QPalette::Window);
        return 8;
    }
    if (requested == u"AccessDialog") {
        QQmlExpression labelExpression(QQmlEngine::contextForObject(window), window, u"dialogButtonBox.standardButton(1024).formaDisplayText"_s);
        const QString actualLabel = labelExpression.evaluate().toString();
        if (actualLabel != u"允许") {
            qWarning() << "Incorrect confirmation label" << actualLabel << window->property("acceptLabel") << window->property("acceptButtonText");
            return 9;
        }
        QSignalSpy accepted(window, SIGNAL(accept()));
        window->setProperty("acceptable", false);
        QTest::qWait(5);
        QTest::keyClick(window, Qt::Key_Return);
        if (accepted.count() != 0 || !window->isVisible())
            return 10;
        window->setProperty("acceptable", true);
    }
    if (!output.isEmpty() && !window->grabWindow().save(output))
        return 3;
    if (!test)
        return application.exec();
    if (!window->flags().testFlag(Qt::FramelessWindowHint))
        return 5;
    QSignalSpy rejected(window, SIGNAL(reject()));
    // Exercise the same close path as the title-bar close button / window manager.
    window->close();
    QTest::qWait(10);
    if (rejected.count() != 1)
        return 5;
    if (warnings)
        return 6;
    qInfo() << "PASS" << requested << "Forma shell, QML bindings and close rejection";
    return 0;
}
#include "formapreview.moc"
