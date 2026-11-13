#include "Theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QWidget>

#include <algorithm>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dwmapi.h>
#endif

namespace Mosaic {
namespace Theme {

namespace {

// Applies native chrome to windows as they appear and tags item-view popups
// (completers, combo lists) so the stylesheet can give them popup styling.
class ChromeFilter : public QObject {
   public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override {
        const QEvent::Type type = event->type();
        if (type != QEvent::Show && type != QEvent::Polish)
            return false;
        auto* widget = qobject_cast<QWidget*>(watched);
        if (!widget || !widget->isWindow())
            return false;

        if (type == QEvent::Polish) {
            if (widget->windowType() == Qt::Popup &&
                qobject_cast<QAbstractItemView*>(widget) &&
                !widget->property("popup").isValid()) {
                widget->setProperty("popup", true);
                widget->style()->unpolish(widget);
                widget->style()->polish(widget);
            }
            return false;
        }

        styleNativeWindow(widget);
        return false;
    }
};

QFont uiFont() {
    QFont font = QApplication::font();
    const QStringList families = QFontDatabase::families();
    for (const char* name : {"Segoe UI Variable Text", "Segoe UI Variable", "Segoe UI"}) {
        const QString family = QString::fromLatin1(name);
        if (families.contains(family)) {
            font.setFamily(family);
            break;
        }
    }
    font.setPointSizeF(10.0);
    return font;
}

QPalette darkPalette() {
    using namespace Color;
    QPalette pal;
    const QColor dim(TextDim);

    pal.setColor(QPalette::Window, QColor(Bg));
    pal.setColor(QPalette::WindowText, QColor(Text));
    pal.setColor(QPalette::Base, QColor(Panel));
    pal.setColor(QPalette::AlternateBase, QColor(Raised));
    pal.setColor(QPalette::ToolTipBase, QColor(Raised));
    pal.setColor(QPalette::ToolTipText, QColor(Text));
    pal.setColor(QPalette::Text, QColor(Text));
    pal.setColor(QPalette::PlaceholderText, dim);
    pal.setColor(QPalette::Button, QColor(Raised));
    pal.setColor(QPalette::ButtonText, QColor(Text));
    pal.setColor(QPalette::BrightText, Qt::white);
    pal.setColor(QPalette::Highlight, QColor(Accent));
    pal.setColor(QPalette::HighlightedText, Qt::white);
    pal.setColor(QPalette::Link, QColor(AccentHover));
    pal.setColor(QPalette::LinkVisited, dim);
    pal.setColor(QPalette::Mid, QColor(Line));
    pal.setColor(QPalette::Dark, QColor(10, 10, 12));
    pal.setColor(QPalette::Light, QColor(60, 64, 74));

    const QPalette::ColorGroup disabled = QPalette::Disabled;
    pal.setColor(disabled, QPalette::WindowText, dim);
    pal.setColor(disabled, QPalette::Text, dim);
    pal.setColor(disabled, QPalette::ButtonText, dim);
    pal.setColor(disabled, QPalette::Base, QColor(Panel));
    pal.setColor(disabled, QPalette::Button, QColor(Raised));
    return pal;
}

// Tokens are written as @name in the sheet. Longer names first so that
// "@accentHover" is not eaten by "@accent".
QString expandTokens(QString sheet) {
    using namespace Color;
    const std::pair<const char*, const char*> tokens[] = {
        {"@accentPressed", AccentPressed},
        {"@accentHover", AccentHover},
        {"@accentSoftHover", "rgba(79, 140, 255, 80)"},
        {"@accentSoft", "rgba(79, 140, 255, 56)"},
        {"@lineStrong", LineStrong},
        {"@accent", Accent},
        {"@danger", Danger},
        {"@raised", Raised},
        {"@hover", Hover},
        {"@panel", Panel},
        {"@line", Line},
        {"@text", Text},
        {"@dim", TextDim},
        {"@bg", Bg},
    };
    std::vector<std::pair<QString, QString>> sorted;
    for (const auto& [key, value] : tokens)
        sorted.emplace_back(QString::fromLatin1(key), QString::fromLatin1(value));
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    for (const auto& [key, value] : sorted) sheet.replace(key, value);
    return sheet;
}

const char kStyleSheet[] = R"(
/* ---------- base ---------- */
QMainWindow, QDialog {
    background: @bg;
}
QWidget {
    color: @text;
    selection-background-color: @accent;
    selection-color: white;
}

/* ---------- text inputs ---------- */
QLineEdit, QSpinBox, QPlainTextEdit, QTextBrowser {
    background: @panel;
    border: 1px solid @line;
    border-radius: 6px;
    padding: 5px 8px;
}
QPlainTextEdit, QTextBrowser {
    padding: 4px;
}
QLineEdit:hover, QSpinBox:hover, QPlainTextEdit:hover {
    border-color: @lineStrong;
}
QLineEdit:focus, QSpinBox:focus, QPlainTextEdit:focus {
    border-color: @accent;
}
QLineEdit:disabled, QSpinBox:disabled {
    color: @dim;
    border-color: @line;
}

/* Search bar text takes the color of the tag group under the cursor. */
QLineEdit[MosaicTagColor="artist"] { color: #78aaff; }
QLineEdit[MosaicTagColor="character"] { color: #96dc96; }
QLineEdit[MosaicTagColor="species"] { color: #f0be78; }
QLineEdit[MosaicTagColor="meta"] { color: #c8a0dc; }
QLineEdit[MosaicTagColor="usergroup"] { color: #ffc864; }
QLineEdit[MosaicTagColor="model"] { color: #78d2d2; }

QSpinBox {
    padding-right: 20px;
}
QSpinBox::up-button, QSpinBox::down-button {
    subcontrol-origin: border;
    width: 18px;
    border: none;
    background: transparent;
}
QSpinBox::up-button {
    subcontrol-position: top right;
    border-top-right-radius: 6px;
}
QSpinBox::down-button {
    subcontrol-position: bottom right;
    border-bottom-right-radius: 6px;
}
QSpinBox::up-button:hover, QSpinBox::down-button:hover {
    background: @hover;
}
QSpinBox::up-arrow {
    image: url(:/icons/chevron-up.svg);
    width: 10px;
    height: 10px;
}
QSpinBox::down-arrow {
    image: url(:/icons/chevron-down.svg);
    width: 10px;
    height: 10px;
}

/* ---------- buttons ---------- */
QPushButton {
    background: @raised;
    border: 1px solid @line;
    border-radius: 6px;
    padding: 5px 14px;
    min-height: 18px;
}
QPushButton:hover {
    background: @hover;
    border-color: @lineStrong;
}
QPushButton:pressed {
    background: @panel;
}
QPushButton:disabled {
    color: @dim;
    background: @panel;
    border-color: @line;
}
QPushButton:flat {
    background: transparent;
    border-color: transparent;
}
QPushButton:flat:hover {
    background: @hover;
}

/* Primary action: the default button of a form, or accent="true". */
QPushButton:default, QPushButton[accent="true"] {
    background: @accent;
    border-color: @accent;
    color: white;
    font-weight: 600;
}
QPushButton:default:hover, QPushButton[accent="true"]:hover {
    background: @accentHover;
    border-color: @accentHover;
}
QPushButton:default:pressed, QPushButton[accent="true"]:pressed {
    background: @accentPressed;
    border-color: @accentPressed;
}
QPushButton:default:disabled, QPushButton[accent="true"]:disabled {
    background: @panel;
    border-color: @line;
    color: @dim;
}

/* Destructive action: neutral until hovered. */
QPushButton[danger="true"]:hover {
    background: @danger;
    border-color: @danger;
    color: white;
}

/* Joined controls (pager): [‹][page][›] */
QPushButton[segment="left"] {
    border-top-right-radius: 0;
    border-bottom-right-radius: 0;
    border-right: none;
    padding: 5px 8px;
}
QPushButton[segment="right"] {
    border-top-left-radius: 0;
    border-bottom-left-radius: 0;
    border-left: none;
    padding: 5px 8px;
}
QLineEdit[segment="middle"] {
    border-radius: 0;
}

QToolButton {
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    padding: 5px;
}
QToolButton:hover {
    background: @hover;
}
QToolButton:pressed {
    background: @panel;
}
QToolButton:checked {
    background: @accentSoft;
}
QToolButton:checked:hover {
    background: @accentSoftHover;
}
QToolButton:disabled {
    color: @dim;
}
QToolButton::menu-indicator {
    image: none;
    width: 0;
}

/* ---------- tabs ---------- */
QTabWidget::pane {
    border: none;
    border-top: 1px solid @line;
    background: @bg;
    top: 0px;
}
QTabWidget::tab-bar {
    left: 0;
}
QTabBar {
    background: transparent;
}
QTabBar::tab {
    background: transparent;
    color: @dim;
    border: none;
    border-bottom: 2px solid transparent;
    padding: 9px 8px 9px 14px;
    margin: 0 1px;
    min-width: 48px;
}
QTabBar::tab:hover:!selected {
    color: @text;
    background: @panel;
    border-top-left-radius: 6px;
    border-top-right-radius: 6px;
}
QTabBar::tab:selected {
    color: @text;
    border-bottom: 2px solid @accent;
}
QTabBar::close-button {
    image: url(:/icons/close-dim.svg);
    subcontrol-position: right;
    border-radius: 4px;
    padding: 2px;
    margin-left: 4px;
}
QTabBar::close-button:selected {
    image: url(:/icons/close.svg);
}
QTabBar::close-button:hover {
    image: url(:/icons/close.svg);
    background: @hover;
}
QTabBar QToolButton {
    background: @bg;
    border-radius: 0;
}
QLineEdit#tabNameEditor {
    background: @raised;
    border: 1px solid @accent;
    border-radius: 4px;
    padding: 0 4px;
}

/* ---------- item views ---------- */
QListWidget, QListView {
    background: @panel;
    border: 1px solid @line;
    border-radius: 8px;
    padding: 4px;
    outline: none;
}
QListWidget::item, QListView::item {
    border-radius: 4px;
    padding: 3px 4px;
}
QListWidget::item:hover, QListView::item:hover {
    background: @raised;
}
QListWidget::item:selected, QListView::item:selected {
    background: @accentSoft;
    color: @text;
}

/* Popup lists (completers): the native window supplies the rounded corners. */
QAbstractItemView[popup="true"] {
    background: @raised;
    border: 1px solid @line;
    border-radius: 0;
    padding: 4px;
    outline: none;
}
QAbstractItemView[popup="true"]::item {
    border-radius: 4px;
    padding: 4px 8px;
}
QAbstractItemView[popup="true"]::item:hover {
    background: @hover;
}
QAbstractItemView[popup="true"]::item:selected {
    background: @accentSoft;
    color: @text;
}

/* ---------- scrollbars ---------- */
QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: rgba(255, 255, 255, 38);
    border-radius: 3px;
    min-height: 32px;
    margin: 2px;
}
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: rgba(255, 255, 255, 38);
    border-radius: 3px;
    min-width: 32px;
    margin: 2px;
}
QScrollBar::handle:hover {
    background: rgba(255, 255, 255, 80);
}
QScrollBar::handle:pressed {
    background: @accent;
}
QScrollBar::add-line, QScrollBar::sub-line {
    height: 0;
    width: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
    background: transparent;
}

/* ---------- sliders ---------- */
QSlider::groove:horizontal {
    height: 4px;
    background: @hover;
    border-radius: 2px;
}
QSlider::sub-page:horizontal {
    background: @accent;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: @text;
    width: 12px;
    height: 12px;
    margin: -4px 0;
    border-radius: 6px;
}
QSlider::handle:horizontal:hover {
    background: white;
}

/* ---------- progress ---------- */
QProgressBar {
    background: @panel;
    border: 1px solid @line;
    border-radius: 6px;
    text-align: center;
    min-height: 16px;
}
QProgressBar::chunk {
    background: @accent;
    border-radius: 5px;
}

/* ---------- menus & tooltips (native window rounds the corners) ---------- */
QMenu {
    background: @raised;
    border: 1px solid @line;
    padding: 4px;
}
QMenu::item {
    padding: 6px 28px 6px 12px;
    border-radius: 4px;
}
QMenu::item:selected {
    background: @hover;
}
QMenu::item:disabled {
    color: @dim;
}
QMenu::separator {
    height: 1px;
    background: @line;
    margin: 4px 6px;
}
QToolTip {
    background: @raised;
    color: @text;
    border: 1px solid @line;
    padding: 5px 8px;
}

/* ---------- forms ---------- */
QGroupBox {
    border: 1px solid @line;
    border-radius: 8px;
    margin-top: 14px;
    padding: 10px 8px 8px 8px;
    font-weight: 600;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    left: 10px;
    padding: 0 4px;
    color: @text;
}
QCheckBox {
    spacing: 8px;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
    border: 1px solid @lineStrong;
    border-radius: 4px;
    background: @panel;
}
QCheckBox::indicator:hover {
    border-color: @accent;
}
QCheckBox::indicator:checked {
    background: @accent;
    border-color: @accent;
    image: url(:/icons/check.svg);
}
QCheckBox::indicator:disabled {
    background: @bg;
    border-color: @line;
}

/* ---------- status bar ---------- */
QStatusBar {
    background: @bg;
    border-top: 1px solid @line;
    color: @dim;
}
QStatusBar::item {
    border: none;
}
QStatusBar QLabel {
    color: @dim;
    padding: 2px 6px;
}

/* ---------- roles (setProperty("role", ...)) ---------- */
QLabel[role="hint"] {
    color: @dim;
}
QLabel[role="empty"] {
    color: @dim;
    font-size: 13pt;
}
QLabel[role="sectionHeader"] {
    color: @dim;
    font-size: 8pt;
    font-weight: 700;
}
QLabel#overlayBadge {
    background: rgba(15, 16, 19, 200);
    color: @text;
    border-radius: 6px;
    padding: 3px 8px;
    font-weight: 600;
}

/* ---------- viewer tag sidebar ---------- */
QWidget#tagSidebar {
    background: @panel;
    border-right: 1px solid @line;
}
QPushButton#sidebarHeader {
    text-align: left;
    font-weight: 600;
    padding: 8px 10px;
    border: none;
    border-radius: 0;
    background: transparent;
}
QPushButton#sidebarHeader:hover {
    background: @hover;
}
QListWidget#tagList {
    background: transparent;
    border: none;
    padding: 0 4px;
}
QListWidget#tagList::item {
    padding: 0;
}
QListWidget#tagList::item:selected {
    background: transparent;
}
)";

}  // namespace

void styleNativeWindow(QWidget* window) {
#ifdef Q_OS_WIN
    if (!window || !window->isWindow())
        return;
    // Attribute ids from dwmapi.h (Windows 11 SDK); spelled out so older SDKs build.
    constexpr DWORD kDarkMode = 20;           // DWMWA_USE_IMMERSIVE_DARK_MODE
    constexpr DWORD kCornerPreference = 33;   // DWMWA_WINDOW_CORNER_PREFERENCE
    constexpr DWORD kBorderColor = 34;        // DWMWA_BORDER_COLOR
    constexpr DWORD kCaptionColor = 35;       // DWMWA_CAPTION_COLOR
    constexpr DWORD kTextColor = 36;          // DWMWA_TEXT_COLOR
    constexpr int kRoundSmall = 3;            // DWMWCP_ROUNDSMALL

    auto colorRef = [](const char* hex) {
        const QColor c(hex);
        return static_cast<COLORREF>(RGB(c.red(), c.green(), c.blue()));
    };

    const Qt::WindowType type = window->windowType();
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;

    if (type == Qt::Popup || type == Qt::ToolTip) {
        // Native rounded corners + shadow for menus, completers and tooltips.
        DwmSetWindowAttribute(hwnd, kCornerPreference, &kRoundSmall, sizeof kRoundSmall);
        const COLORREF border = colorRef(Color::Line);
        DwmSetWindowAttribute(hwnd, kBorderColor, &border, sizeof border);
        return;
    }
    if (type != Qt::Window && type != Qt::Dialog)
        return;

    // Errors are ignored: on Windows 10 the color attributes don't exist and
    // the system frame stays as it was. The border is left to Windows so the
    // user's "accent color on window borders" setting still applies.
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, kDarkMode, &dark, sizeof dark);
    const COLORREF caption = colorRef(Color::Bg);
    DwmSetWindowAttribute(hwnd, kCaptionColor, &caption, sizeof caption);
    const COLORREF text = colorRef(Color::Text);
    DwmSetWindowAttribute(hwnd, kTextColor, &text, sizeof text);
#else
    Q_UNUSED(window);
#endif
}

void apply() {
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QApplication::setFont(uiFont());
    QApplication::setPalette(darkPalette());
    qApp->setStyleSheet(expandTokens(QString::fromUtf8(kStyleSheet)));
    qApp->installEventFilter(new ChromeFilter(qApp));
}

}  // namespace Theme
}  // namespace Mosaic
