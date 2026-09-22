#include "study_icons.hpp"

#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>

#include <algorithm>
#include <cmath>
#include <utility>

namespace melearner {
namespace {

constexpr qreal kCanvas = 24.0;
constexpr qreal kStroke = 1.7;
constexpr qreal kPi = 3.14159265358979323846;

QColor modeColor(QColor color, QIcon::Mode mode) {
  if (!color.isValid()) color = QColor(Qt::black);
  if (mode == QIcon::Disabled) color.setAlphaF(color.alphaF() * 0.45);
  return color;
}

void drawCourses(QPainter& painter) {
  QPainterPath path;
  path.moveTo(12, 5);
  path.cubicTo(9.7, 4.0, 6.6, 4.0, 4, 5.2);
  path.lineTo(4, 18.7);
  path.cubicTo(6.6, 17.7, 9.6, 17.7, 12, 19);
  path.cubicTo(14.4, 17.7, 17.4, 17.7, 20, 18.7);
  path.lineTo(20, 5.2);
  path.cubicTo(17.4, 4.0, 14.3, 4.0, 12, 5);
  path.closeSubpath();
  painter.drawPath(path);
  painter.drawLine(QPointF(12, 5), QPointF(12, 19));
}

void drawActivity(QPainter& painter) {
  QPainterPath path;
  path.moveTo(3, 14);
  path.lineTo(7, 14);
  path.lineTo(9.2, 6);
  path.lineTo(12, 18);
  path.lineTo(15, 10);
  path.lineTo(17, 14);
  path.lineTo(21, 14);
  painter.drawPath(path);
}

void drawSearch(QPainter& painter) {
  painter.drawEllipse(QRectF(3.5, 3.5, 13, 13));
  painter.drawLine(QPointF(15.5, 15.5), QPointF(20.5, 20.5));
}

void drawSettings(QPainter& painter) {
  QPainterPath gear;
  constexpr int teeth = 8;
  constexpr qreal outerRadius = 8.4;
  constexpr qreal innerRadius = 6.6;
  for (int point = 0; point < teeth * 2; ++point) {
    const qreal angle = -kPi / 2 + point * kPi / teeth;
    const qreal radius = point % 2 == 0 ? outerRadius : innerRadius;
    const QPointF position(12 + radius * std::cos(angle), 12 + radius * std::sin(angle));
    if (point == 0) gear.moveTo(position);
    else gear.lineTo(position);
  }
  gear.closeSubpath();
  painter.drawPath(gear);
  painter.drawEllipse(QRectF(9, 9, 6, 6));
}

void drawKeyboard(QPainter& painter) {
  painter.drawRoundedRect(QRectF(2.5, 5, 19, 14), 2, 2);
  for (const QRectF& key : {QRectF(5, 8, 2, 2), QRectF(8.5, 8, 2, 2),
                             QRectF(12, 8, 2, 2), QRectF(15.5, 8, 2, 2),
                             QRectF(5, 12, 2, 2), QRectF(8.5, 12, 2, 2),
                             QRectF(12, 12, 7, 2)}) {
    painter.drawRoundedRect(key, 0.5, 0.5);
  }
}

void drawChevron(QPainter& painter, bool right) {
  QPainterPath path;
  if (right) {
    path.moveTo(9, 5);
    path.lineTo(16, 12);
    path.lineTo(9, 19);
  } else {
    path.moveTo(15, 5);
    path.lineTo(8, 12);
    path.lineTo(15, 19);
  }
  painter.drawPath(path);
}

void drawPlay(QPainter& painter) {
  QPainterPath path;
  path.moveTo(9, 5.5);
  path.lineTo(19, 12);
  path.lineTo(9, 18.5);
  path.closeSubpath();
  painter.drawPath(path);
}

void drawPause(QPainter& painter) {
  painter.drawLine(QPointF(9, 6), QPointF(9, 18));
  painter.drawLine(QPointF(15, 6), QPointF(15, 18));
}

void drawFullscreen(QPainter& painter) {
  QPainterPath path;
  path.moveTo(9, 4);
  path.lineTo(4, 4);
  path.lineTo(4, 9);
  path.moveTo(15, 4);
  path.lineTo(20, 4);
  path.lineTo(20, 9);
  path.moveTo(4, 15);
  path.lineTo(4, 20);
  path.lineTo(9, 20);
  path.moveTo(20, 15);
  path.lineTo(20, 20);
  path.lineTo(15, 20);
  painter.drawPath(path);
}

void drawCheck(QPainter& painter) {
  QPainterPath path;
  path.moveTo(4.5, 12);
  path.lineTo(9.5, 17);
  path.lineTo(19.5, 7);
  painter.drawPath(path);
}

void drawFolder(QPainter& painter) {
  QPainterPath path;
  path.moveTo(3.5, 7.5);
  path.lineTo(9, 7.5);
  path.lineTo(11, 5.5);
  path.lineTo(20.5, 5.5);
  path.lineTo(20.5, 18.5);
  path.lineTo(3.5, 18.5);
  path.closeSubpath();
  painter.drawPath(path);
  painter.drawLine(QPointF(3.5, 7.5), QPointF(20.5, 7.5));
}

void drawIcon(QPainter& painter, StudyIcon icon) {
  switch (icon) {
    case StudyIcon::Courses: drawCourses(painter); break;
    case StudyIcon::Activity: drawActivity(painter); break;
    case StudyIcon::Search: drawSearch(painter); break;
    case StudyIcon::Settings: drawSettings(painter); break;
    case StudyIcon::Keyboard: drawKeyboard(painter); break;
    case StudyIcon::ChevronLeft: drawChevron(painter, false); break;
    case StudyIcon::ChevronRight: drawChevron(painter, true); break;
    case StudyIcon::Play: drawPlay(painter); break;
    case StudyIcon::Pause: drawPause(painter); break;
    case StudyIcon::Fullscreen: drawFullscreen(painter); break;
    case StudyIcon::Check: drawCheck(painter); break;
    case StudyIcon::Folder: drawFolder(painter); break;
  }
}

class StudyIconEngine final : public QIconEngine {
public:
  StudyIconEngine(StudyIcon icon, QColor color) : icon_(icon), color_(std::move(color)) {}

  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override {
    if (!painter || rect.isEmpty()) return;

    const qreal side = std::min(rect.width(), rect.height());
    if (side <= 0) return;
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(modeColor(color_, mode), kStroke, Qt::SolidLine,
                         Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->translate(QRectF(rect).center());
    painter->scale(side / kCanvas, side / kCanvas);
    painter->translate(-kCanvas / 2, -kCanvas / 2);
    drawIcon(*painter, icon_);
    painter->restore();
  }

  QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override {
    return size;
  }

  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
    return scaledPixmap(size, mode, state, 1.0);
  }

  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
    QPixmap result(size * scale);
    result.fill(Qt::transparent);
    result.setDevicePixelRatio(scale);
    QPainter painter(&result);
    paint(&painter, QRect(QPoint(), size), mode, state);
    return result;
  }

  QString key() const override { return QStringLiteral("melearner.study-icon"); }

  QIconEngine* clone() const override { return new StudyIconEngine(icon_, color_); }

private:
  StudyIcon icon_;
  QColor color_;
};

}  // namespace

QIcon studyIcon(StudyIcon icon, QColor color) {
  return QIcon(new StudyIconEngine(icon, std::move(color)));
}

}  // namespace melearner
