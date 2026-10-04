#pragma once

#include "theme.hpp"

#include <QChildEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>

#include <utility>

namespace melearner {

[[nodiscard]] inline qreal canvasCornerRadius(const QWidget* widget) {
  return themeFor(widget).radius() * 1.4;
}

// A composited cutout keeps native GL and WebEngine surfaces rounded without
// masking their backing stores or reading pixels back from the GPU.
class CornerCover final : public QWidget {
public:
  explicit CornerCover(QWidget* canvas) : QWidget(canvas) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setEnabled(false);
    setAccessibleName(QString());
    setAccessibleDescription(QString());
    canvas->installEventFilter(this);
    setGeometry(canvas->rect());
  }

  void setRadii(qreal topLeft, qreal topRight, qreal bottomRight,
                qreal bottomLeft, QColor background) {
    const QList<qreal> radii{qMax<qreal>(0, topLeft), qMax<qreal>(0, topRight),
                             qMax<qreal>(0, bottomRight), qMax<qreal>(0, bottomLeft)};
    if (radii_ == radii && background_ == background) return;
    radii_ = radii;
    background_ = std::move(background);
    setVisible(topLeft > 0 || topRight > 0 || bottomRight > 0 || bottomLeft > 0);
    raise();
    update();
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override {
    if (watched == parentWidget()) {
      if (event->type() == QEvent::Resize || event->type() == QEvent::Show) {
        syncGeometryAndStack();
      } else if (event->type() == QEvent::ChildAdded) {
        QTimer::singleShot(0, this, [this] { syncGeometryAndStack(); });
      }
    }
    return QWidget::eventFilter(watched, event);
  }

  void paintEvent(QPaintEvent*) override {
    const QRectF bounds(rect());
    const qreal maxRadius = qMin(bounds.width(), bounds.height()) / 2.0;
    const qreal topLeft = qMin(radii_[0], maxRadius);
    const qreal topRight = qMin(radii_[1], maxRadius);
    const qreal bottomRight = qMin(radii_[2], maxRadius);
    const qreal bottomLeft = qMin(radii_[3], maxRadius);
    QPainterPath cover;
    cover.setFillRule(Qt::OddEvenFill);
    cover.addRect(bounds);
    QPainterPath rounded;
    rounded.moveTo(bounds.left() + topLeft, bounds.top());
    rounded.lineTo(bounds.right() - topRight, bounds.top());
    if (topRight > 0)
      rounded.arcTo(QRectF(bounds.right() - 2 * topRight, bounds.top(), 2 * topRight, 2 * topRight), 90, -90);
    rounded.lineTo(bounds.right(), bounds.bottom() - bottomRight);
    if (bottomRight > 0)
      rounded.arcTo(QRectF(bounds.right() - 2 * bottomRight, bounds.bottom() - 2 * bottomRight, 2 * bottomRight, 2 * bottomRight), 0, -90);
    rounded.lineTo(bounds.left() + bottomLeft, bounds.bottom());
    if (bottomLeft > 0)
      rounded.arcTo(QRectF(bounds.left(), bounds.bottom() - 2 * bottomLeft, 2 * bottomLeft, 2 * bottomLeft), 270, -90);
    rounded.lineTo(bounds.left(), bounds.top() + topLeft);
    if (topLeft > 0)
      rounded.arcTo(QRectF(bounds.left(), bounds.top(), 2 * topLeft, 2 * topLeft), 180, -90);
    rounded.closeSubpath();
    cover.addPath(rounded);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background_);
    painter.drawPath(cover);
  }

private:
  QList<qreal> radii_{0, 0, 0, 0};
  QColor background_ = Qt::black;

  void syncGeometryAndStack() {
    if (!parentWidget()) return;
    setGeometry(parentWidget()->rect());
    if (isVisible()) raise();
  }
};

} // namespace melearner
