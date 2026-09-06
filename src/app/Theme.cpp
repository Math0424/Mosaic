#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>

namespace Mosaic {
namespace Theme {

namespace {

constexpr char kAccent[] = "#4f8cff";
constexpr char kAccentHover[] = "#6ea1ff";
constexpr char kAccentPressed[] = "#3d6fd6";
constexpr char kBg[] = "#17181c";
constexpr char kSurface[] = "#1e2026";
constexpr char kSurfaceAlt[] = "#23262d";
constexpr char kBorder[] = "#33363f";
constexpr char kText[] = "#e8e9ec";
constexpr char kTextDim[] = "#9aa0ab";

}  // namespace

void apply() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette pal;
    const QColor bg(kBg);
    const QColor surface(kSurface);
    const QColor text(kText);
    const QColor dim(kTextDim);
    const QColor accent(kAccent);

    pal.setColor(QPalette::Window, bg);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, surface);
    pal.setColor(QPalette::AlternateBase, kSurfaceAlt);
    pal.setColor(QPalette::ToolTipBase, kSurfaceAlt);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::PlaceholderText, dim);
    pal.setColor(QPalette::Button, kSurfaceAlt);
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::BrightText, Qt::white);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::Link, kAccentHover);
    pal.setColor(QPalette::LinkVisited, dim);
    pal.setColor(QPalette::Mid, kBorder);
    pal.setColor(QPalette::Dark, QColor(10, 10, 12));
    pal.setColor(QPalette::Light, QColor(60, 64, 74));

    const QPalette::ColorGroup disabled = QPalette::Disabled;
    pal.setColor(disabled, QPalette::WindowText, dim);
    pal.setColor(disabled, QPalette::Text, dim);
    pal.setColor(disabled, QPalette::ButtonText, dim);
    pal.setColor(disabled, QPalette::Base, surface);
    pal.setColor(disabled, QPalette::Button, kSurfaceAlt);

    QApplication::setPalette(pal);

    QString sheet = QStringLiteral(R"(
        /* chrome */
        QMainWindow, QDialog {
            background: %1;
        }
        QWidget {
            color: %6;
            selection-background-color: %5;
            selection-color: white;
        }

        /* inputs */
        QLineEdit, QSpinBox, QComboBox {
            background: %2;
            border: 1px solid %4;
            border-radius: 6px;
            padding: 4px 8px;
            selection-background-color: %5;
        }
        QLineEdit:focus, QSpinBox:focus {
            border: 1px solid %5;
        }
        QLineEdit:disabled, QSpinBox:disabled {
            color: %7;
        }

        /* Search bar text takes the color of the tag group under the cursor. */
        QLineEdit[MosaicTagColor="artist"] { color: #78aaff; }
        QLineEdit[MosaicTagColor="character"] { color: #96dc96; }
        QLineEdit[MosaicTagColor="species"] { color: #f0be78; }
        QLineEdit[MosaicTagColor="meta"] { color: #c8a0dc; }
        QLineEdit[MosaicTagColor="usergroup"] { color: #ffc864; }
        QLineEdit[MosaicTagColor="model"] { color: #78d2d2; }
        QSpinBox::up-button, QSpinBox::down-button {
            background: %3;
            border: none;
            width: 14px;
        }
        QSpinBox::up-button:hover, QSpinBox::down-button:hover {
            background: %4;
        }

        /* buttons */
        QPushButton {
            background: %3;
            border: 1px solid %4;
            border-radius: 6px;
            padding: 5px 12px;
        }
        QPushButton:hover {
            background: #2a2e37;
            border-color: #454a58;
        }
        QPushButton:pressed {
            background: #16181d;
        }
        QPushButton:disabled {
            color: %7;
            background: %2;
        }
        QToolButton {
            background: transparent;
            border: 1px solid transparent;
            border-radius: 6px;
            padding: 4px;
        }
        QToolButton:hover {
            background: %3;
            border-color: %4;
        }
        QToolButton:pressed, QToolButton:checked {
            background: %5;
            border-color: %5;
        }
        QToolButton:disabled {
            color: %7;
        }

        /* tabs */
        QTabWidget::pane {
            border-top: 1px solid %4;
            background: %1;
        }
        QTabBar {
            background: transparent;
        }
        QTabBar::tab {
            background: %2;
            border: 1px solid %4;
            border-bottom: none;
            border-top-left-radius: 6px;
            border-top-right-radius: 6px;
            padding: 6px 14px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: %1;
            border-color: %5;
            color: white;
        }
        QTabBar::tab:hover:!selected {
            background: %3;
        }
        QTabBar::close-button {
            image: url(%8);
            border-radius: 4px;
            padding: 2px;
            margin: 2px 8px 2px 2px;
        }
        QTabBar::close-button:hover {
            image: url(%8);
            background: %3;
        }

        /* lists */
        QListWidget {
            background: %2;
            border: 1px solid %4;
            border-radius: 3px;
            outline: none;
        }
        QListWidget::item {
            border-radius: 2px;
        }
        QListWidget::item:hover {
            background: %3;
        }
        QListWidget::item:selected {
            background: %5;
            color: white;
        }

        /* scrollbars */
        QScrollBar:vertical {
            background: %1;
            width: 12px;
            margin: 0;
        }
        QScrollBar::handle:vertical {
            background: #3a3f4b;
            border-radius: 5px;
            min-height: 24px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #4a5060;
        }
        QScrollBar:horizontal {
            background: %1;
            height: 12px;
            margin: 0;
        }
        QScrollBar::handle:horizontal {
            background: #3a3f4b;
            border-radius: 5px;
            min-width: 24px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #4a5060;
        }
        QScrollBar::add-line, QScrollBar::sub-line {
            height: 0;
            width: 0;
        }
        QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
        }

        /* sliders */
        QSlider::groove:horizontal {
            height: 4px;
            background: %3;
            border-radius: 2px;
        }
        QSlider::sub-page:horizontal {
            background: %5;
            border-radius: 2px;
        }
        QSlider::handle:horizontal {
            background: %6;
            width: 12px;
            height: 12px;
            margin: -5px 0;
            border-radius: 6px;
        }
        QSlider::handle:horizontal:hover {
            background: white;
        }

        /* progress */
        QProgressBar {
            background: %2;
            border: 1px solid %4;
            border-radius: 6px;
            text-align: center;
        }
        QProgressBar::chunk {
            background: %5;
            border-radius: 5px;
        }

        /* menus */
        QMenu {
            background: %3;
            border: 1px solid %4;
            border-radius: 8px;
            padding: 4px;
        }
        QMenu::item {
            padding: 6px 24px 6px 12px;
            border-radius: 4px;
        }
        QMenu::item:selected {
            background: %5;
            color: white;
        }
        QMenu::separator {
            height: 1px;
            background: %4;
            margin: 4px 8px;
        }

        /* misc */
        QGroupBox {
            border: 1px solid %4;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            left: 10px;
            padding: 0 4px;
            color: %7;
        }
        QStatusBar {
            background: %2;
            border-top: 1px solid %4;
            color: %7;
        }
        QToolTip {
            background: %3;
            color: %6;
            border: 1px solid %4;
            border-radius: 4px;
            padding: 4px 6px;
        }
        QCheckBox {
            spacing: 8px;
        }
        QCheckBox::indicator {
            width: 16px;
            height: 16px;
            border: 1px solid %4;
            border-radius: 4px;
            background: %2;
        }
        QCheckBox::indicator:hover {
            border-color: %5;
        }
        QCheckBox::indicator:checked {
            background: %5;
            border-color: %5;
        }
        QCompleter, QAbstractItemView {
            background: %3;
            alternate-background-color: %2;
            border: 1px solid %4;
            border-radius: 6px;
            outline: none;
        }
        QAbstractItemView::item:selected {
            background: %5;
            color: white;
        }
    )");
    sheet.replace(QStringLiteral("%1"), QString::fromLatin1(kBg));
    sheet.replace(QStringLiteral("%2"), QString::fromLatin1(kSurface));
    sheet.replace(QStringLiteral("%3"), QString::fromLatin1(kSurfaceAlt));
    sheet.replace(QStringLiteral("%4"), QString::fromLatin1(kBorder));
    sheet.replace(QStringLiteral("%5"), QString::fromLatin1(kAccent));
    sheet.replace(QStringLiteral("%6"), QString::fromLatin1(kText));
    sheet.replace(QStringLiteral("%7"), QString::fromLatin1(kTextDim));
    // url() paths resolve against the working directory; use an absolute path.
    const QString closeIcon =
        QCoreApplication::applicationDirPath() + QStringLiteral("/assets/close.svg");
    sheet.replace(QStringLiteral("%8"), closeIcon);
    qApp->setStyleSheet(sheet);
}

}  // namespace Theme
}  // namespace Mosaic
