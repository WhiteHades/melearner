#include "seek_feedback.hpp"
#include "theme.hpp"
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>

namespace melearner {
SeekFeedback::SeekFeedback(QWidget* parent) : QWidget(parent),
    hold_(new QTimer(this)), fade_(new QVariantAnimation(this)),
    arrowGesture_(new QVariantAnimation(this)) {
    setObjectName("seekFeedback");
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFixedSize(138, 64);
    hold_->setSingleShot(true);
    fade_->setEasingCurve(QEasingCurve::OutCubic);
    connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        opacity_ = value.toDouble(); update();
    });
    connect(fade_, &QVariantAnimation::finished, this, [this] {
        if (fade_->endValue().toDouble() == 0) hide();
    });
    arrowGesture_->setEasingCurve(QEasingCurve::OutBack);
    connect(arrowGesture_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        arrowOffset_ = value.toDouble(); update();
    });
    connect(arrowGesture_, &QVariantAnimation::finished, this, [this] {
        if (returningArrow_) return;
        returningArrow_ = true;
        arrowGesture_->setDuration(120);
        arrowGesture_->setEasingCurve(QEasingCurve::OutCubic);
        arrowGesture_->setStartValue(arrowOffset_);
        arrowGesture_->setEndValue(0.0);
        arrowGesture_->start();
    });
    connect(hold_, &QTimer::timeout, this, [this] {
        fade_->stop(); fade_->setDuration(reducedMotion() || highContrast() ? 0 : 150);
        fade_->setStartValue(opacity_); fade_->setEndValue(0.0); fade_->start();
    });
    hide();
}
void SeekFeedback::showSeek(qint64 deltaMs) {
    seconds_ = deltaMs / 1000;
    setProperty("deltaMs", deltaMs);
    setAccessibleName(tr("%1 %2 seconds").arg(seconds_ < 0 ? tr("Back") : tr("Forward"))
        .arg(qAbs(seconds_)));
    move(qRound(parentWidget()->width() * (seconds_ < 0 ? .27 : .73)) - width() / 2,
        (parentWidget()->height() - height()) / 2);
    const bool entering = !isVisible();
    show(); raise(); fade_->stop();
    if (!reducedMotion() && !highContrast()) {
        arrowGesture_->stop(); returningArrow_ = false;
        arrowGesture_->setDuration(150);
        arrowGesture_->setEasingCurve(QEasingCurve::OutBack);
        arrowGesture_->setStartValue(arrowOffset_);
        arrowGesture_->setEndValue(seconds_ < 0 ? -12.0 : 12.0);
        arrowGesture_->start();
    } else {
        arrowGesture_->stop(); arrowOffset_ = 0; update();
    }
    if (entering && !reducedMotion() && !highContrast()) {
        fade_->setDuration(140); fade_->setStartValue(0.0); fade_->setEndValue(1.0); fade_->start();
    } else { opacity_ = 1; update(); }
    hold_->start(650);
}
void SeekFeedback::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setOpacity(opacity_);
    p.setPen(Qt::NoPen); p.setBrush(QColor(0, 0, 0, 120));
    p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 20, 20);
    auto type = font(); type.setPixelSize(22); type.setWeight(QFont::DemiBold); p.setFont(type);
    p.setPen(Qt::white);
    const bool forward = seconds_ >= 0;
    // Leave more breathing room ahead of the animated arrow than behind it.
    const int contentOffset = forward ? -8 : 8;
    p.drawText(QRect((forward ? 24 : 36) + contentOffset, 0, 78, height()), Qt::AlignCenter,
        QStringLiteral("%1%2 s").arg(seconds_ < 0 ? "−" : "+").arg(qAbs(seconds_)));
    const double direction = forward ? 1 : -1;
    const double x = (forward ? width() - 28 : 28) + contentOffset + arrowOffset_;
    QPen pen(Qt::white, 2.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(pen);
    QPolygonF arrow; arrow << QPointF(x - direction * 5, 24) << QPointF(x + direction * 3, 32)
        << QPointF(x - direction * 5, 40); p.drawPolyline(arrow);
}
}
