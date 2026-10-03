#pragma once

#include <QWidget>
#include <QPointer>
#include "library.hpp"

class QEvent;
class QLabel;
class QResizeEvent;
class QShowEvent;
class QHideEvent;
class QFrame;
class QWindow;

namespace melearner { class MpvVideoWidget; class Player; }
namespace shadcn { class Button; }

namespace melearner {

class CoursePreview final : public QWidget {
    Q_OBJECT
public:
    explicit CoursePreview(QWidget* parent = nullptr, bool softwareDecoding = false);
    ~CoursePreview() override;

    void setPreview(const QString& approvedRoot, const melearner::library::Lesson& lesson);
    void clear();
    [[nodiscard]] bool hasPreview() const { return hasPreview_; }

signals:
    void activated();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    int heightForWidth(int width) const override;
    bool hasHeightForWidth() const override { return true; }

private:
    void syncPlayback();
    void startPreview();
    void loadWhenReady();
    void updateSurface();
    void updateMuteButton();
    void shutdownPlayer();

    bool softwareDecoding_ = false;
    bool hasPreview_ = false;
    bool muted_ = true;
    bool hovered_ = false;
    bool active_ = false;
    bool loadRequested_ = false;
    bool loaded_ = false;
    quint64 initialMuteRequest_ = 0;
    QString approvedRoot_;
    melearner::library::Lesson lesson_;
    melearner::Player* player_ = nullptr;
    melearner::MpvVideoWidget* video_ = nullptr;
    QFrame* surface_ = nullptr;
    QLabel* hint_ = nullptr;
    QLabel* continueLabel_ = nullptr;
    shadcn::Button* muteButton_ = nullptr;
    QPointer<QWindow> watchedWindow_;
    QMetaObject::Connection activeConnection_;
};

}  // namespace melearner
