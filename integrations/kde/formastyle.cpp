// SPDX-License-Identifier: LGPL-2.0-or-later
#include "formastyle.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QJSEngine>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWindow>

namespace
{
class DialogChrome : public QWidget
{
public:
    explicit DialogChrome(QDialog *dialog)
        : QWidget(dialog)
        , m_dialog(dialog)
    {
        setFixedHeight(46);
        auto row = new QHBoxLayout(this);
        row->setContentsMargins(15, 0, 15, 0);
        row->setSpacing(8);
        auto makeDot = [this, row](const QString &name, const QString &symbol, const QString &accessible) {
            auto button = new QToolButton(this);
            button->setObjectName(name);
            button->setText(symbol);
            button->setAccessibleName(accessible);
            button->setFixedSize(13, 13);
            row->addWidget(button);
            return button;
        };
        auto close = makeDot(QStringLiteral("FormaClose"), QStringLiteral("×"), tr("Close"));
        auto minimize = makeDot(QStringLiteral("FormaMinimize"), QStringLiteral("−"), tr("Minimize"));
        auto zoom = makeDot(QStringLiteral("FormaZoom"), QStringLiteral("+"), tr("Zoom"));
        connect(close, &QToolButton::clicked, dialog, &QDialog::reject);
        connect(minimize, &QToolButton::clicked, dialog, &QWidget::showMinimized);
        connect(zoom, &QToolButton::clicked, dialog, [dialog] {
            dialog->isMaximized() ? dialog->showNormal() : dialog->showMaximized();
        });
        auto title = new QLabel(dialog->windowTitle(), this);
        title->setObjectName(QStringLiteral("FormaWindowTitle"));
        title->setAlignment(Qt::AlignCenter);
        title->setAttribute(Qt::WA_TransparentForMouseEvents);
        row->addWidget(title, 1);
        row->addSpacing(55);
        connect(dialog, &QWidget::windowTitleChanged, title, &QLabel::setText);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dialog->windowHandle()) {
            m_dialog->windowHandle()->startSystemMove();
            event->accept();
        }
    }

private:
    QDialog *m_dialog;
};

class DialogDecorator : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        auto dialog = qobject_cast<QDialog *>(object);
        if (!dialog || !dialog->isWindow())
            return false;
        if (dialog->property("formaDecorated").toBool()) {
            if (event->type() == QEvent::Resize) {
                if (auto chrome = dialog->findChild<QWidget *>(QStringLiteral("FormaWindowChrome"), Qt::FindDirectChildrenOnly))
                    chrome->setGeometry(1, 1, dialog->width() - 2, 46);
            } else if (event->type() == QEvent::Paint) {
                QPainter painter(dialog);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(qApp->palette().color(QPalette::Window));
                painter.setPen(qApp->palette().color(QPalette::Mid));
                painter.drawRoundedRect(QRectF(dialog->rect()).adjusted(0.5, 0.5, -0.5, -0.5), 20, 20);
            }
            return false;
        }
        if ((event->type() != QEvent::Polish && event->type() != QEvent::Show) || dialog->property("formaDecorationQueued").toBool())
            return false;
        dialog->setProperty("formaDecorationQueued", true);
        QTimer::singleShot(0, dialog, [dialog] {
            dialog->setProperty("formaDecorationQueued", false);
            if (dialog->property("formaDecorated").toBool() || !dialog->layout())
                return;
            const bool visible = dialog->isVisible();
            const auto size = dialog->size();
            dialog->setProperty("formaDecorated", true);
            // Keep the original layout and every widget parent in place. Some Qt print/file
            // dialogs manage their internal pages dynamically and cannot be reparented.
            auto margins = dialog->layout()->contentsMargins();
            dialog->layout()->setContentsMargins(qMax(16, margins.left()), margins.top() + 46, qMax(16, margins.right()), qMax(16, margins.bottom()));
            dialog->setObjectName(QStringLiteral("FormaPortalWindow"));
            dialog->setWindowFlag(Qt::FramelessWindowHint);
            dialog->setAttribute(Qt::WA_TranslucentBackground);
            dialog->setAttribute(Qt::WA_StyledBackground);
            auto chrome = new DialogChrome(dialog);
            chrome->setObjectName(QStringLiteral("FormaWindowChrome"));
            chrome->setGeometry(1, 1, dialog->width() - 2, 46);
            chrome->show();
            dialog->style()->unpolish(dialog);
            dialog->style()->polish(dialog);
            dialog->resize(size.width(), size.height() + 46);
            if (visible)
                dialog->show();
        });
        return false;
    }
};

class FormaPalette : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantMap colors READ colors NOTIFY changed)
public:
    FormaPalette()
    {
        qApp->installEventFilter(this);
    }
    QVariantMap colors() const
    {
        const auto p = qApp->palette();
        return {{QStringLiteral("window"), p.color(QPalette::Window)},
                {QStringLiteral("base"), p.color(QPalette::Base)},
                {QStringLiteral("alternateBase"), p.color(QPalette::AlternateBase)},
                {QStringLiteral("text"), p.color(QPalette::Text)},
                {QStringLiteral("button"), p.color(QPalette::Button)},
                {QStringLiteral("light"), p.color(QPalette::Light)},
                {QStringLiteral("midlight"), p.color(QPalette::Midlight)},
                {QStringLiteral("mid"), p.color(QPalette::Mid)},
                {QStringLiteral("dark"), p.color(QPalette::Dark)},
                {QStringLiteral("highlight"), p.color(QPalette::Highlight)},
                {QStringLiteral("highlightedText"), p.color(QPalette::HighlightedText)},
                {QStringLiteral("placeholderText"), p.color(QPalette::PlaceholderText)}};
    }
    Q_SIGNAL void changed();

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ApplicationPaletteChange)
            Q_EMIT changed();
        return false;
    }
};

void updatePalette(bool initialDark)
{
    auto appearance = qgetenv("FORMA_APPEARANCE");
    if (appearance.isEmpty())
        appearance = qgetenv("MAYO_APPEARANCE");
    const auto scheme = qApp->styleHints()->colorScheme();
    const bool dark =
        appearance == "dark" || (appearance != "light" && (scheme == Qt::ColorScheme::Dark || (scheme == Qt::ColorScheme::Unknown && initialDark)));
    QPalette p;
    p.setColor(QPalette::Window, QColor(dark ? "#232326" : "#f5f5f7"));
    p.setColor(QPalette::Base, QColor(dark ? "#1c1c1e" : "#ffffff"));
    p.setColor(QPalette::AlternateBase, QColor(dark ? "#29292c" : "#f0f0f3"));
    p.setColor(QPalette::Text, QColor(dark ? "#e9e9ec" : "#252528"));
    p.setColor(QPalette::WindowText, p.color(QPalette::Text));
    p.setColor(QPalette::ButtonText, p.color(QPalette::Text));
    p.setColor(QPalette::Button, QColor(dark ? "#3b3b3e" : "#ffffff"));
    p.setColor(QPalette::Light, QColor(dark ? "#4c4c50" : "#ffffff"));
    p.setColor(QPalette::Midlight, QColor(dark ? "#454549" : "#e7e7eb"));
    p.setColor(QPalette::Mid, QColor(dark ? "#626268" : "#c5c5ca"));
    p.setColor(QPalette::Dark, QColor(dark ? "#a0a0a8" : "#72727a"));
    p.setColor(QPalette::Shadow, QColor(dark ? "#111113" : "#d6d6dc"));
    p.setColor(QPalette::Highlight, QColor(dark ? "#0a84ff" : "#0088ff"));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, p.color(QPalette::Highlight));
    p.setColor(QPalette::PlaceholderText, QColor(dark ? "#8e8e96" : "#85858d"));
    p.setColor(QPalette::ToolTipBase, p.color(QPalette::Window));
    p.setColor(QPalette::ToolTipText, p.color(QPalette::Text));
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(dark ? "#74747b" : "#aaaab2"));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, p.color(QPalette::Disabled, QPalette::Text));
    p.setColor(QPalette::Disabled, QPalette::WindowText, p.color(QPalette::Disabled, QPalette::Text));
    qApp->setPalette(p);
    // Qt Widgets is used for printing, background permission and the file-chooser fallback.
    const QString sheet = QStringLiteral(R"(
QDialog#FormaPortalWindow { background: %1; border: 1px solid %5; border-radius: 20px; }
QWidget#FormaPortalBody { background: transparent; }
QLabel#FormaWindowTitle { font-weight: 600; font-size: 13px; }
QPushButton { min-height: 22px; min-width: 66px; padding: 4px 15px; border-radius: 15px; background: %2; border: 1px solid %5; }
QPushButton:hover { background: %3; }
QPushButton:pressed { background: %4; }
QPushButton:default { color: white; background: %6; border-color: %6; }
QPushButton:focus, QToolButton:focus, QComboBox:focus, QLineEdit:focus { border: 2px solid %6; }
QPushButton:disabled { color: %7; background: %1; }
QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox { min-height: 24px; padding: 3px 8px; border: 1px solid %5; border-radius: 8px; background: %8; }
QAbstractItemView { background: %8; alternate-background-color: %1; border: 1px solid %5; border-radius: 10px; selection-background-color: %6; selection-color: white; }
QAbstractItemView::item { padding: 4px; }
QCheckBox, QRadioButton { spacing: 8px; }
QCheckBox::indicator { width: 16px; height: 16px; border: 1px solid %5; border-radius: 5px; background: %8; }
QCheckBox::indicator:checked { background: %6; border-color: %6; image: url(:/qt/qml/check.svg); }
QCheckBox::indicator:indeterminate { background: %6; border-color: %6; }
QRadioButton::indicator { width: 16px; height: 16px; border: 1px solid %5; border-radius: 8px; background: %8; }
QRadioButton::indicator:checked { border-color: %6; background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, stop:0 white, stop:0.4 white, stop:0.45 %6, stop:1 %6); }
QProgressBar { border: none; border-radius: 3px; background: %4; text-align: center; }
QProgressBar::chunk { background: %6; border-radius: 3px; }
QHeaderView::section { background: %1; border: none; padding: 7px; }
QTabWidget::pane { border: 1px solid %5; border-radius: 10px; }
QTabBar::tab { padding: 7px 15px; border-radius: 8px; margin: 3px; }
QTabBar::tab:selected { background: %2; }
QGroupBox { border: 1px solid %5; border-radius: 10px; margin-top: 12px; padding-top: 12px; }
QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 12px; padding: 0 5px; }
QScrollBar:vertical { width: 10px; background: transparent; margin: 2px; }
QScrollBar::handle:vertical { background: %7; border-radius: 4px; min-height: 28px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QToolButton { padding: 5px; border: 1px solid transparent; border-radius: 8px; }
QToolButton:hover { background: %4; }
QToolButton#FormaClose, QToolButton#FormaMinimize, QToolButton#FormaZoom { min-width: 0; min-height: 0; padding: 0; border: none; border-radius: 6px; color: transparent; }
QToolButton#FormaClose { background: #ff5f57; }
QToolButton#FormaMinimize { background: #febc2e; }
QToolButton#FormaZoom { background: #28c840; }
QToolButton#FormaClose:hover, QToolButton#FormaMinimize:hover, QToolButton#FormaZoom:hover { color: #48231d; }
QMenu { background: %1; border: 1px solid %5; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 6px 20px; border-radius: 6px; }
QMenu::item:selected { color: white; background: %6; }
)")
                              .arg(p.color(QPalette::Window).name(),
                                   p.color(QPalette::Button).name(),
                                   p.color(QPalette::Light).name(),
                                   p.color(QPalette::Midlight).name(),
                                   dark ? QStringLiteral("#515156") : QStringLiteral("#d4d4da"),
                                   p.color(QPalette::Highlight).name(),
                                   p.color(QPalette::PlaceholderText).name(),
                                   p.color(QPalette::Base).name());
    qApp->setStyleSheet(sheet);
}
} // namespace

void installFormaPortalStyle()
{
    Q_INIT_RESOURCE(forma_style);
    const bool initialDark = qApp->palette().color(QPalette::Window).lightness() < 128;
    QApplication::setStyle(QStringLiteral("Fusion"));
    QQuickStyle::setStyle(QStringLiteral("Forma"));
    QQuickStyle::setFallbackStyle(QStringLiteral("Basic"));
    const int fontId = QFontDatabase::addApplicationFont(QStringLiteral(":/qt/qml/fonts/InterVariable.ttf"));
    if (fontId >= 0) {
        QFont font(QFontDatabase::applicationFontFamilies(fontId).value(0));
        font.setPixelSize(13);
        qApp->setFont(font);
    }
    qmlRegisterSingletonType<FormaPalette>("FormaPortal", 1, 0, "Theme", [](QQmlEngine *, QJSEngine *) -> QObject * {
        return new FormaPalette;
    });
    updatePalette(initialDark);
    QObject::connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, qApp, [initialDark] {
        updatePalette(initialDark);
    });
    qApp->installEventFilter(new DialogDecorator(qApp));
}

#include "formastyle.moc"
