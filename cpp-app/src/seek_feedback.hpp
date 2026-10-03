#pragma once

#include <QWidget>

class QTimer;
class QVariantAnimation;

namespace melearner {
// A bounded overlay. Seeking is immediate; only the acknowledgement animates.
class SeekFeedback final : public QWidget {
    Q_OBJECT
public:
    explicit SeekFeedback(QWidget* parent);
    void showSeek(qint64 deltaMs);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QTimer* hold_;
    QVariantAnimation* fade_;
    double opacity_ = 1;
    qint64 seconds_ = 0;
};
}
