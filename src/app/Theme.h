#pragma once

class QWidget;

namespace Mosaic {

// Global dark theme (Fusion style + palette + stylesheet + native window chrome).
// Call once from main() before any widget is created.
namespace Theme {

// Design tokens. Use these instead of hard-coded hex values in widgets; prefer
// a dynamic property styled in Theme.cpp (e.g. role="hint") over setStyleSheet.
namespace Color {
inline constexpr char Bg[] = "#17181c";           // window background
inline constexpr char Panel[] = "#1e2026";        // inputs, lists, sidebars
inline constexpr char Raised[] = "#23262d";       // buttons, menus, popups
inline constexpr char Hover[] = "#2b2f38";        // hover fill
inline constexpr char Line[] = "#2f323b";         // hairline borders
inline constexpr char LineStrong[] = "#454a58";   // hovered borders
inline constexpr char Text[] = "#e8e9ec";
inline constexpr char TextDim[] = "#9aa0ab";
inline constexpr char Accent[] = "#4f8cff";
inline constexpr char AccentHover[] = "#6ea1ff";
inline constexpr char AccentPressed[] = "#3d6fd6";
inline constexpr char Danger[] = "#d9534f";
}  // namespace Color

void apply();

// Matches a top-level window's native frame to the theme: caption/border
// color on normal windows, small rounded corners on popups and tooltips.
// Windows 11 only; a no-op elsewhere. apply() does this automatically for
// every window as it is shown.
void styleNativeWindow(QWidget* window);

}  // namespace Theme

}  // namespace Mosaic
