#include "course_rows.hpp"

#include "theme.hpp"

#include <QFontMetrics>
#include <QIdentityProxyModel>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace melearner {
namespace {

class PaintRolesModel final : public QIdentityProxyModel {
public:
    using QIdentityProxyModel::QIdentityProxyModel;

    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override {
        if (role == Qt::DisplayRole || role == shadcnRowDescription || role == shadcnRowProgress
            || role == shadcnRowLeading || role == shadcnRowTrailing) {
            return {};
        }
        return QIdentityProxyModel::data(index, role);
    }
};

class CourseRowDelegate final : public QStyledItemDelegate {
public:
    explicit CourseRowDelegate(CourseListView* view)
        : QStyledItemDelegate(view), view_(view), native_(this) {}

    void setPresentation(shadcn::ListPresentation presentation) {
        native_.setPresentation(presentation);
        view_->viewport()->update();
    }

    void setCompact(bool compact) { native_.setCompact(compact); }
    void setRowFont(const QFont& font) { native_.setRowFont(font); }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override {
        if (view_->presentation() == shadcn::ListPresentation::List) {
            return {0, std::max({76, native_.rowHeight(), QFontMetrics(option.font).lineSpacing() * 2 + 18})};
        }
        return view_->gridSize();
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        auto* source = const_cast<QAbstractItemModel*>(index.model());
        if (masked_.sourceModel() != source) masked_.setSourceModel(source);
        const auto maskedIndex = masked_.mapFromSource(index);
        if (!maskedIndex.isValid()) return;

        // RowDelegate retains all native backgrounds, selection, focus rings,
        // and card gutters. Apply reveal state against the real view index; the
        // temporary role-masking proxy is only used to keep its generic text out.
        painter->save();
        const auto progress = view_->revealProgress(index);
        bool reduced = false;
        auto* style = qobject_cast<const shadcn::Style*>(view_->style());
        if (!style) style = qobject_cast<const shadcn::Style*>(QApplication::style());
        reduced = style && style->motion() == shadcn::MotionPolicy::Reduced;
        painter->setOpacity(painter->opacity() * progress);
        if (!reduced) painter->translate(0, 8.0 * (1.0 - progress));
        native_.paint(painter, option, maskedIndex);

        painter->setRenderHint(QPainter::Antialiasing);
        if (view_->presentation() == shadcn::ListPresentation::Cards) {
            paintCard(painter, option, index);
        } else {
            paintList(painter, option, index);
        }
        painter->restore();
    }

private:
    static QVector<QString> wrappedLines(const QString& text, const QFont& font, int width, int limit) {
        QVector<QString> result;
        if (text.isEmpty() || width <= 0 || limit <= 0) return result;

        QTextLayout layout(text, font);
        QTextOption textOption;
        textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(textOption);
        layout.beginLayout();
        int consumed = 0;
        int lastStart = 0;
        while (result.size() < limit) {
            auto line = layout.createLine();
            if (!line.isValid()) break;
            line.setLineWidth(width);
            const auto start = line.textStart();
            const auto length = line.textLength();
            lastStart = start;
            consumed = start + length;
            result.append(text.mid(start, length).trimmed());
        }
        layout.endLayout();
        if (result.size() == limit && consumed < text.size()) {
            result.last() = QFontMetrics(font).elidedText(text.mid(lastStart), Qt::ElideRight, width);
        }
        return result;
    }

    void paintThumbnail(QPainter* painter, const QRectF& target, const QPixmap& pixmap,
                        qreal radius) const {
        QPainterPath clip;
        clip.addRoundedRect(target, radius, radius);
        painter->save();
        painter->setClipPath(clip);
        if (!pixmap.isNull()) {
            // Source coordinates are physical pixels; the target is logical.
            const auto pixels = pixmap.size();
            const auto aspect = target.width() / std::max<qreal>(1, target.height());
            int cropWidth = pixels.width();
            int cropHeight = pixels.height();
            if (static_cast<qreal>(pixels.width()) / std::max(1, pixels.height()) > aspect) {
                cropWidth = std::max(1, qRound(pixels.height() * aspect));
            } else {
                cropHeight = std::max(1, qRound(pixels.width() / aspect));
            }
            const QRect sourceRect((pixels.width() - cropWidth) / 2,
                                   (pixels.height() - cropHeight) / 2, cropWidth, cropHeight);
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawPixmap(target, pixmap, QRectF(sourceRect));
        } else {
            painter->fillPath(clip, roleColor(view_, shadcn::Role::Muted));
            const auto center = target.center();
            const auto size = std::min(target.width() * .19, target.height() * .42);
            const QRectF book(center.x() - size, center.y() - size * .62, size * 2, size * 1.24);
            QPen pen(roleColor(view_, shadcn::Role::MutedForeground));
            pen.setWidthF(1.7);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            QPainterPath glyph;
            glyph.moveTo(book.center().x(), book.top() + 2);
            glyph.cubicTo(book.left() + size * .55, book.top() - 1,
                          book.left() + 1, book.top() + 2, book.left() + 1, book.top() + 5);
            glyph.lineTo(book.left() + 1, book.bottom() - 4);
            glyph.cubicTo(book.left() + size * .55, book.bottom() - 7,
                          book.center().x() - 2, book.bottom() - 1, book.center().x(), book.bottom());
            glyph.cubicTo(book.center().x() + 2, book.bottom() - 1,
                          book.right() - size * .55, book.bottom() - 7, book.right() - 1, book.bottom() - 4);
            glyph.lineTo(book.right() - 1, book.top() + 5);
            glyph.cubicTo(book.right() - 1, book.top() + 2,
                          book.right() - size * .55, book.top() - 1, book.center().x(), book.top() + 2);
            painter->drawPath(glyph);
            painter->drawLine(QPointF(book.center().x(), book.top() + 2),
                              QPointF(book.center().x(), book.bottom() - 1));
        }
        painter->restore();
    }

    void paintCard(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const {
        const auto card = QRectF(option.rect).adjusted(6, 6, -6, -6);
        const auto pad = 12.0;
        const auto image = QRectF(card.left() + pad, card.top() + pad,
                                  card.width() - pad * 2, (card.width() - pad * 2) * 9.0 / 16.0);
        paintThumbnail(painter, image, index.data(CourseThumbnailRole).value<QPixmap>(),
                       themeFor(view_).radius());

        const auto titleFont = option.font;
        auto emphasized = titleFont;
        emphasized.setWeight(QFont::DemiBold);
        const auto bodyFont = option.font;
        const auto textX = card.left() + pad;
        const auto textWidth = std::max(0, qFloor(card.width() - pad * 2));
        const auto titleLine = QFontMetrics(emphasized).lineSpacing();
        const auto bodyLine = QFontMetrics(bodyFont).lineSpacing();
        auto y = image.bottom() + 8;
        painter->setFont(emphasized);
        painter->setPen(option.state.testFlag(QStyle::State_Selected)
                            ? roleColor(view_, shadcn::Role::AccentForeground)
                            : roleColor(view_, shadcn::Role::CardForeground));
        const auto titles = wrappedLines(index.data(Qt::DisplayRole).toString(), emphasized, textWidth, 2);
        for (const auto& line : titles) {
            painter->drawText(QRect(qRound(textX), qRound(y), textWidth, titleLine),
                              Qt::AlignLeft | Qt::AlignVCenter, line);
            y += titleLine;
        }
        const auto description = index.data(shadcnRowDescription).toString();
        if (!description.isEmpty()) {
            painter->setFont(bodyFont);
            painter->setPen(roleColor(view_, shadcn::Role::MutedForeground));
            const auto line = QFontMetrics(bodyFont).elidedText(description, Qt::ElideRight, textWidth);
            painter->drawText(QRect(qRound(textX), qRound(y + 2), textWidth, bodyLine),
                              Qt::AlignLeft | Qt::AlignVCenter, line);
        }
    }

    void paintList(QPainter* painter, const QStyleOptionViewItem& option,
                   const QModelIndex& index) const {
        const auto rect = option.rect.adjusted(18, 7, -18, -7);
        constexpr int thumbnailWidth = 76;
        constexpr int thumbnailHeight = 43;
        const QRectF thumb(rect.left(), rect.center().y() - thumbnailHeight / 2.0,
                           thumbnailWidth, thumbnailHeight);
        paintThumbnail(painter, thumb, index.data(CourseThumbnailRole).value<QPixmap>(),
                       themeFor(view_).radius());

        const auto gap = 14;
        const auto right = rect.right() - 24;
        const auto x = qRound(thumb.right()) + gap;
        const auto width = std::max(0, right - x);
        auto emphasized = option.font;
        emphasized.setWeight(QFont::Medium);
        const auto metrics = QFontMetrics(emphasized);
        const auto body = QFontMetrics(option.font);
        const auto title = metrics.elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, width);
        const auto description = body.elidedText(index.data(shadcnRowDescription).toString(), Qt::ElideRight, width);
        const auto blockHeight = metrics.lineSpacing() + body.lineSpacing();
        const auto y = rect.top() + (rect.height() - blockHeight) / 2;
        painter->setFont(emphasized);
        painter->setPen(option.state.testFlag(QStyle::State_Selected)
                            ? roleColor(view_, shadcn::Role::AccentForeground)
                            : roleColor(view_, shadcn::Role::Foreground));
        painter->drawText(QRect(x, y, width, metrics.lineSpacing()), Qt::AlignLeft | Qt::AlignVCenter, title);
        painter->setFont(option.font);
        painter->setPen(roleColor(view_, shadcn::Role::MutedForeground));
        painter->drawText(QRect(x, y + metrics.lineSpacing(), width, body.lineSpacing()),
                          Qt::AlignLeft | Qt::AlignVCenter, description);

        QPen chevron(roleColor(view_, shadcn::Role::MutedForeground));
        chevron.setWidthF(1.7);
        chevron.setCapStyle(Qt::RoundCap);
        chevron.setJoinStyle(Qt::RoundJoin);
        painter->setPen(chevron);
        const auto cx = option.rect.right() - 21.0;
        const auto cy = option.rect.center().y();
        painter->drawLine(QPointF(cx - 3, cy - 5), QPointF(cx + 2, cy));
        painter->drawLine(QPointF(cx + 2, cy), QPointF(cx - 3, cy + 5));
    }

    CourseListView* view_;
    mutable PaintRolesModel masked_;
    shadcn::RowDelegate native_;
};

}  // namespace

CourseListView::CourseListView(QWidget* parent) : shadcn::ListView(parent) {
    auto* courseDelegate = new CourseRowDelegate(this);
    QListView::setItemDelegate(courseDelegate);
    courseDelegate->setPresentation(presentation());
    courseDelegate->setCompact(compactRows());
    courseDelegate->setRowFont(font());
}

void CourseListView::setPresentation(shadcn::ListPresentation value) {
    shadcn::ListView::setPresentation(value);
    if (auto* delegate = dynamic_cast<CourseRowDelegate*>(itemDelegate())) {
        delegate->setPresentation(value);
        delegate->setCompact(compactRows());
        delegate->setRowFont(font());
    }
    updateCourseGrid();
    doItemsLayout();
}

void CourseListView::resizeEvent(QResizeEvent* event) {
    shadcn::ListView::resizeEvent(event);
    updateCourseGrid();
}

void CourseListView::changeEvent(QEvent* event) {
    shadcn::ListView::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange
        || event->type() == QEvent::ApplicationFontChange) {
        if (auto* delegate = dynamic_cast<CourseRowDelegate*>(itemDelegate())) {
            delegate->setCompact(compactRows());
            delegate->setRowFont(font());
        }
        updateCourseGrid();
    }
}

void CourseListView::updateCourseGrid() {
    if (presentation() != shadcn::ListPresentation::Cards) return;
    constexpr int gap = 12;
    const auto width = std::max(1, viewport()->width());
    const auto minCardWidth = std::max(260, QFontMetrics(font()).horizontalAdvance(QStringLiteral("MMMMMMMMMMMM")) + 36);
    const auto columns = width < 560 ? 1 : std::clamp((width + gap) / (minCardWidth + gap), 1, 4);
    const auto cardWidth = std::max(1, (width - gap * (columns - 1)) / columns);
    const auto innerWidth = std::max(1, cardWidth - 36);
    const auto imageHeight = qCeil(innerWidth * 9.0 / 16.0);
    QFont emphasized = font();
    emphasized.setWeight(QFont::DemiBold);
    const auto line = std::max(QFontMetrics(emphasized).lineSpacing(), QFontMetrics(font()).lineSpacing());
    const auto descriptionLine = QFontMetrics(font()).lineSpacing();
    const auto height = 44 + imageHeight + 2 * line + descriptionLine + 10;
    setGridSize(QSize(cardWidth, height));
}

}  // namespace melearner
