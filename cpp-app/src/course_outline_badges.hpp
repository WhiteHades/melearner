#pragma once

#include "course_outline_model.hpp"
#include "theme.hpp"

#include <shadcn/rows.hpp>

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPointer>
#include <QPixmapCache>
#include <QStyleOptionViewItem>

#include <algorithm>

namespace melearner {

/// Adds a shadcn Badge to lesson descriptions while leaving section rows in the
/// native row delegate. The original description remains available to assistive
/// technology and tooltips.
class CourseOutlineBadgeDelegate final : public QStyledItemDelegate {
public:
    CourseOutlineBadgeDelegate(QAbstractItemDelegate* native, QObject* parent = nullptr)
        : QStyledItemDelegate(parent), native_(native) {}

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        return native_->sizeHint(option, index);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        if (!index.data(courseOutlineBadgeRole).isValid()) {
            native_->paint(painter, option, index);
            return;
        }

        native_->paint(painter, option, index);
        const auto* widget = option.widget;
        if (!widget) return;

        const auto compact = [&] {
            const auto* rows = qobject_cast<const shadcn::RowDelegate*>(native_.data());
            return rows && rows->compact();
        }();
        const auto inset = compact ? 10 : 14;
        const auto side = compact ? 16 : 20;
        auto x = option.rect.left() + inset;
        if (const auto* tree = qobject_cast<const shadcn::TreeView*>(widget)) {
            const auto disclosure = tree->disclosureRect(index);
            if (disclosure.isValid()) x = disclosure.right() + 8;
        }
        if (!index.data(shadcnRowLeading).value<QPixmap>().isNull()) x += side + 10;
        const auto right = option.rect.right() - inset;

        auto bodyFont = option.font;
        QFontMetrics bodyMetrics(bodyFont);
        auto titleFont = bodyFont;
        titleFont.setWeight(QFont::Medium);
        QFontMetrics titleMetrics(titleFont);
        const auto blockHeight = titleMetrics.lineSpacing() + bodyMetrics.lineSpacing();
        const auto descriptionY = option.rect.top()
            + std::max(0, (option.rect.height() - blockHeight) / 2) + titleMetrics.lineSpacing();

        auto background = roleColor(widget, shadcn::Role::Background);
        const auto selected = option.state.testFlag(QStyle::State_Selected);
        const auto hovered = option.state.testFlag(QStyle::State_MouseOver);
        if (selected) {
            background = roleColor(widget, shadcn::Role::Accent);
        } else if (hovered) {
            const auto accent = roleColor(widget, shadcn::Role::Accent);
            background = QColor::fromRgbF(
                background.redF() * .6 + accent.redF() * .4,
                background.greenF() * .6 + accent.greenF() * .4,
                background.blueF() * .6 + accent.blueF() * .4,
                background.alphaF() * .6 + accent.alphaF() * .4);
        }

        const auto label = index.data(courseOutlineBadgeRole).toString();
        auto description = index.data(shadcnRowDescription).toString();
        if (description.startsWith(label)) description.remove(0, label.size());
        if (description.startsWith(QStringLiteral(" · "))) description.remove(0, 3);
        const auto badge = badgePixmap(label, widget->devicePixelRatioF());

        painter->save();
        painter->fillRect(QRect(x, descriptionY, std::max(0, right - x), bodyMetrics.lineSpacing()),
                          background);
        const auto badgeSize = badge.deviceIndependentSize();
        const auto badgeY = descriptionY + (bodyMetrics.lineSpacing() - badgeSize.height()) / 2;
        painter->drawPixmap(QPointF(x, badgeY), badge);
        x += badgeSize.width();
        if (!description.isEmpty()) {
            const auto separator = QStringLiteral(" · ");
            painter->setFont(bodyFont);
            painter->setPen(roleColor(widget, selected ? shadcn::Role::AccentForeground
                                                       : shadcn::Role::MutedForeground));
            const auto separatorWidth = bodyMetrics.horizontalAdvance(separator);
            painter->drawText(QRect(x, descriptionY, separatorWidth, bodyMetrics.lineSpacing()),
                              Qt::AlignLeft | Qt::AlignVCenter, separator);
            x += separatorWidth;
            painter->drawText(QRect(x, descriptionY, std::max(0, right - x), bodyMetrics.lineSpacing()),
                              Qt::AlignLeft | Qt::AlignVCenter,
                              bodyMetrics.elidedText(description, Qt::ElideRight, std::max(0, right - x)));
        }
        painter->restore();
    }

private:
    QPointer<QAbstractItemDelegate> native_;

    [[nodiscard]] static QPixmap badgePixmap(const QString& label, qreal devicePixelRatio) {
        const auto highContrastMode = highContrast();
        auto badgeTheme = themeFor(nullptr);
        if (!highContrastMode) {
            const auto dark = badgeTheme.mode() == shadcn::ColorMode::Dark;
            auto fill = shadcn::Rgba{0.85, 0.90, 0.97, 1};
            auto text = shadcn::Rgba{0.25, 0.35, 0.51, 1};
            if (dark) {
                fill = {0.22, 0.29, 0.37, 1};
                text = {0.65, 0.80, 0.96, 1};
            }
            if (label == QObject::tr("Video")) {
                fill = dark ? shadcn::Rgba{0.28, 0.24, 0.36, 1} : shadcn::Rgba{0.91, 0.85, 0.97, 1};
                text = dark ? shadcn::Rgba{0.82, 0.75, 0.95, 1} : shadcn::Rgba{0.36, 0.29, 0.48, 1};
            } else if (label == QObject::tr("Audio")) {
                fill = dark ? shadcn::Rgba{0.22, 0.31, 0.29, 1} : shadcn::Rgba{0.85, 0.94, 0.87, 1};
                text = dark ? shadcn::Rgba{0.67, 0.88, 0.75, 1} : shadcn::Rgba{0.25, 0.42, 0.31, 1};
            }
            (void)badgeTheme.set(shadcn::Role::Secondary, fill);
            (void)badgeTheme.set(shadcn::Role::SecondaryForeground, text);
        }

        const auto fill = roleColor(badgeTheme, shadcn::Role::Secondary);
        const auto foreground = roleColor(badgeTheme, shadcn::Role::SecondaryForeground);
        const auto dpr = std::max<qreal>(1, devicePixelRatio);
        const auto key = QStringLiteral("course-outline-badge-%1-%2-%3-%4-%5")
            .arg(label).arg(fill.rgba()).arg(foreground.rgba()).arg(highContrastMode).arg(dpr);
        QPixmap cached;
        if (QPixmapCache::find(key, &cached)) return cached;

        shadcn::Style style(badgeTheme);
        shadcn::Badge badge(label);
        badge.setVariant(shadcn::Variant::Secondary);
        badge.setStyle(&style);
        badge.ensurePolished();
        badge.resize(badge.sizeHint());
        QPixmap pixmap(qRound(badge.width() * dpr), qRound(badge.height() * dpr));
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        badge.render(&pixmap, QPoint(), QRegion(), QWidget::DrawChildren);
        QPixmapCache::insert(key, pixmap);
        return pixmap;
    }
};

}  // namespace melearner
