#pragma once

#include <shadcn/core.hpp>
#include <shadcn/rows.hpp>
#include <shadcn/widgets.hpp>

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyle>
#include <QStyleHints>
#include <QAccessibilityHints>

namespace melearner {

/// The desktop asks for its own high contrast palette, so the theme must not
/// paint over it.
[[nodiscard]] inline bool highContrast() {
    return QApplication::styleHints()->accessibility()->contrastPreference() ==
           Qt::ContrastPreference::HighContrast;
}

/// The desktop has turned animation off. Qt 6.11 has no reduced motion property,
/// so the two native signals are the widget animation duration and the cursor
/// flash time, both of which the platform reports as zero when it wants no
/// motion.
[[nodiscard]] inline bool reducedMotion() {
    return QApplication::style()->styleHint(QStyle::SH_Widget_Animation_Duration) == 0 ||
           QApplication::styleHints()->cursorFlashTime() == 0;
}



namespace detail {

[[nodiscard]] inline shadcn::Rgba rgba(const QColor& color) {
    return {static_cast<double>(color.redF()), static_cast<double>(color.greenF()),
            static_cast<double>(color.blueF()), static_cast<double>(color.alphaF())};
}

/// Assign one role, ignoring a value the theme rejects. The system palette is
/// always in range, so a rejected value means the role is out of date rather
/// than the palette being wrong, and keeping the neutral value is the safe
/// answer.
inline void assign(shadcn::Theme& theme, shadcn::Role role, const QColor& color) {
    if (color.isValid()) (void)theme.set(role, rgba(color));
}

/// Map the system palette onto the theme's semantic roles. Only the roles the
/// application can actually reach are mapped; the rest stay at their neutral
/// values because nothing paints them in this state.
inline void applySystemContrast(shadcn::Theme& theme) {
    const auto palette = QApplication::style()->standardPalette();
    const auto window = palette.color(QPalette::Window);
    const auto windowText = palette.color(QPalette::WindowText);
    const auto base = palette.color(QPalette::Base);
    const auto text = palette.color(QPalette::Text);
    const auto button = palette.color(QPalette::Button);
    const auto buttonText = palette.color(QPalette::ButtonText);
    const auto highlight = palette.color(QPalette::Highlight);
    const auto highlightedText = palette.color(QPalette::HighlightedText);
    const auto mid = palette.color(QPalette::Mid);
    const auto placeholder = palette.color(QPalette::PlaceholderText);
    const auto alternate = palette.color(QPalette::AlternateBase);

    assign(theme, shadcn::Role::Background, window);
    assign(theme, shadcn::Role::Foreground, windowText);
    assign(theme, shadcn::Role::Card, base);
    assign(theme, shadcn::Role::CardForeground, text);
    assign(theme, shadcn::Role::Popover, window);
    assign(theme, shadcn::Role::PopoverForeground, windowText);
    assign(theme, shadcn::Role::Primary, highlight);
    assign(theme, shadcn::Role::PrimaryForeground, highlightedText);
    assign(theme, shadcn::Role::Secondary, button);
    assign(theme, shadcn::Role::SecondaryForeground, buttonText);
    assign(theme, shadcn::Role::Muted, alternate);
    assign(theme, shadcn::Role::MutedForeground, placeholder);
    assign(theme, shadcn::Role::Accent, highlight);
    assign(theme, shadcn::Role::AccentForeground, highlightedText);
    assign(theme, shadcn::Role::Border, mid);
    assign(theme, shadcn::Role::Input, mid);
    assign(theme, shadcn::Role::Ring, highlight);
    assign(theme, shadcn::Role::Chart1, highlight);
    assign(theme, shadcn::Role::Chart2, windowText);
    assign(theme, shadcn::Role::Chart3, mid);
    assign(theme, shadcn::Role::Chart4, buttonText);
    assign(theme, shadcn::Role::Chart5, placeholder);
    assign(theme, shadcn::Role::Sidebar, window);
    assign(theme, shadcn::Role::SidebarForeground, windowText);
    assign(theme, shadcn::Role::SidebarPrimary, highlight);
    assign(theme, shadcn::Role::SidebarPrimaryForeground, highlightedText);
    assign(theme, shadcn::Role::SidebarAccent, highlight);
    assign(theme, shadcn::Role::SidebarAccentForeground, highlightedText);
    assign(theme, shadcn::Role::SidebarBorder, mid);
    assign(theme, shadcn::Role::SidebarRing, highlight);
}

}  // namespace detail

/// Install the shadcn neutral theme. A zero or negative `fontPixels` keeps the
/// current font, so a re-install never overrides the user's text size; only the
/// first call sets one.
///
/// `dark` is a parameter rather than fixed here because the harness renders both
/// modes to check that no component keeps a colour from the other one, and that
/// check is worth having. The application itself only ever passes true.
inline void installTheme(bool dark, int fontPixels = 0) {
    qApp->setProperty("melearnerClassicTooltips", !highContrast());
    auto theme = shadcn::Theme::neutral(dark ? shadcn::ColorMode::Dark : shadcn::ColorMode::Light);
    if (highContrast()) detail::applySystemContrast(theme);
    shadcn::install(*qApp, std::move(theme),
                    reducedMotion() ? shadcn::MotionPolicy::Reduced : shadcn::MotionPolicy::Full,
                    fontPixels);
    // shadcn::install writes the Qt palette from the theme. Under high contrast
    // the platform palette is the user's choice, so it goes back afterwards for
    // the views that keep Qt's own painting.
    if (highContrast()) {
        QApplication::setPalette(QApplication::style()->standardPalette());
    } else {
        auto palette = QApplication::palette();
        const QColor tooltipBase("#111111");
        const QColor tooltipText("#ffffff");
        for (const auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            palette.setColor(group, QPalette::ToolTipBase, tooltipBase);
            palette.setColor(group, QPalette::ToolTipText, tooltipText);
        }
        QApplication::setPalette(palette);
    }
}

/// The theme behind a widget's style, falling back to the application style and
/// then to the neutral theme when neither is a shadcn style.
[[nodiscard]] inline const shadcn::Theme& themeFor(const QWidget* widget) {
    if (widget != nullptr) {
        if (const auto* style = qobject_cast<shadcn::Style*>(widget->style())) return style->theme();
    }
    if (const auto* style = qobject_cast<shadcn::Style*>(QApplication::style())) return style->theme();
    static const shadcn::Theme fallback = shadcn::Theme::neutral();
    return fallback;
}

[[nodiscard]] inline QColor roleColor(const shadcn::Theme& theme, shadcn::Role role) {
    const auto value = theme.color(role);
    return QColor::fromRgbF(static_cast<float>(value.r), static_cast<float>(value.g),
                            static_cast<float>(value.b), static_cast<float>(value.a));
}

[[nodiscard]] inline QColor roleColor(shadcn::Role role) {
    return roleColor(themeFor(nullptr), role);
}

[[nodiscard]] inline QColor roleColor(const QWidget* widget, shadcn::Role role) {
    return roleColor(themeFor(widget), role);
}

/// The item data roles the themed row views read, named once here so a model and
/// the view that paints it cannot drift onto different numbers.
inline constexpr int shadcnRowDescription = static_cast<int>(shadcn::RowRole::Description);
inline constexpr int shadcnRowLeading = static_cast<int>(shadcn::RowRole::Leading);
inline constexpr int shadcnRowTrailing = static_cast<int>(shadcn::RowRole::Trailing);
inline constexpr int shadcnRowTrailingText = static_cast<int>(shadcn::RowRole::TrailingText);
inline constexpr int shadcnRowProgress = static_cast<int>(shadcn::RowRole::Progress);
inline constexpr int shadcnRowHeading = static_cast<int>(shadcn::RowRole::Heading);

}  // namespace melearner
