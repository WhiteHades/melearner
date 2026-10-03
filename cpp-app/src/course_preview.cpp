#include "course_preview.hpp"

#include "mpv_video_widget.hpp"
#include "player.hpp"
#include "study_icons.hpp"
#include "theme.hpp"

#include <shadcn/controls.hpp>

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QLabel>
#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWindow>
#include <QPainterPath>
#include <QRegion>
#include <algorithm>

namespace melearner {

CoursePreview::CoursePreview(QWidget* parent, bool softwareDecoding)
    : QWidget(parent), softwareDecoding_(softwareDecoding) {
    setObjectName("coursePreview");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    surface_ = new QFrame(this);
    surface_->setObjectName("coursePreviewSurface");
    surface_->setStyleSheet(QStringLiteral("QFrame#coursePreviewSurface { background: %1; border-radius: 12px; }")
        .arg(melearner::roleColor(this, shadcn::Role::Card).name()));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(surface_);

    hint_ = new QLabel(tr("Video preview unavailable"), surface_);
    hint_->setAlignment(Qt::AlignCenter);
    hint_->setWordWrap(true);
    hint_->setStyleSheet(QStringLiteral("QLabel { color: %1; background: transparent; }")
        .arg(melearner::roleColor(this, shadcn::Role::MutedForeground).name()));
    hint_->setObjectName("coursePreviewHint");

    continueLabel_ = new QLabel(tr("Continue watching"), surface_);
    continueLabel_->setAlignment(Qt::AlignCenter);
    continueLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
    continueLabel_->setStyleSheet(QStringLiteral(
        "QLabel { color: white; background: rgba(0, 0, 0, 155); padding: 10px 18px; border-radius: 8px; }") );
    continueLabel_->hide();

    muteButton_ = new shadcn::Button;
    muteButton_->setObjectName("previewMute");
    muteButton_->setVariant(shadcn::Variant::Secondary);
    muteButton_->setButtonSize(shadcn::ButtonSize::IconSm);
    muteButton_->setParent(surface_);
    muteButton_->setFixedSize(muteButton_->sizeHint());
    muteButton_->raise();
    updateMuteButton();
    connect(muteButton_, &QPushButton::clicked, this, [this] {
        muted_ = !muted_;
        if (player_) (void)player_->setMuted(muted_);
        updateMuteButton();
    });

    surface_->installEventFilter(this);
    hint_->installEventFilter(this);
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this] { syncPlayback(); });
}

CoursePreview::~CoursePreview() { shutdownPlayer(); }

void CoursePreview::setPreview(const QString& approvedRoot, const melearner::library::Lesson& lesson) {
    clear();
    muted_ = true;
    muteButton_->setEnabled(false);
    updateMuteButton();
    approvedRoot_ = approvedRoot;
    lesson_ = lesson;
    hasPreview_ = lesson.type == QStringLiteral("video") && !lesson.path.isEmpty();
    hint_->setText(hasPreview_ ? QString{} : tr("No video preview available"));
    hint_->setVisible(!hasPreview_);
    if (!hasPreview_) return;
    syncPlayback();
}

void CoursePreview::startPreview() {
    if (player_ || !hasPreview_) return;
    player_ = new melearner::Player(this, softwareDecoding_
        ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic);
    player_->setObjectName("previewPlayer");
    player_->setApprovedRoots({approvedRoot_});
    video_ = new melearner::MpvVideoWidget(player_, surface_);
    video_->setObjectName("previewVideo");
    video_->setMinimumSize(0, 0);
    video_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    video_->lower();
    connect(video_, &melearner::MpvVideoWidget::clicked, this, &CoursePreview::activated);
    connect(video_, &melearner::MpvVideoWidget::renderError, this, [](const QString&, const QString&) {});
    video_->installEventFilter(this);
    connect(player_, &Player::initialized, this, &CoursePreview::loadWhenReady);
    connect(video_, &MpvVideoWidget::renderContextReady, this, &CoursePreview::loadWhenReady);
    connect(player_, &Player::fileLoaded, this, [this] {
        loaded_ = true; muteButton_->setEnabled(true);
        if (active_) (void)player_->play(); else (void)player_->pause();
    });
    connect(player_, &Player::commandFinished, this, [this](auto id) {
        if (id != initialMuteRequest_) return;
        initialMuteRequest_ = 0;
        const auto savedMs = static_cast<qint64>(std::max(0.0, lesson_.lastPosition) * 1000);
        loadRequested_ = player_->loadFile(lesson_.path, savedMs) != 0;
    });
    connect(player_, &Player::commandFailed, this, [this](auto, const QString&, const QString&) {
        hint_->setText(tr("Video preview unavailable")); hint_->show(); hint_->raise(); muteButton_->raise();
    });
    updateSurface(); video_->show();
    player_->start();
}
void CoursePreview::loadWhenReady() {
    if (!player_ || !video_ || loadRequested_ || initialMuteRequest_ ||
        !player_->isReady() || !video_->isRenderContextReady()) return;
    // Mute commands coalesce separately from loads. Wait for acknowledgement,
    // rather than assuming enqueue order makes initial playback inaudible.
    initialMuteRequest_ = player_->setMuted(muted_);
}

void CoursePreview::clear() {
    hasPreview_ = false;
    active_ = false; loadRequested_ = false; loaded_ = false; initialMuteRequest_ = 0;
    hint_->setText(tr("No video preview available"));
    hint_->show();
    continueLabel_->hide();
    shutdownPlayer();
    approvedRoot_.clear();
    lesson_ = {};
    update();
}

void CoursePreview::shutdownPlayer() {
    if (player_ == nullptr) return;
    (void)player_->pause();
    if (video_) {
        video_->removeEventFilter(this);
        video_->setPlayer(nullptr); // Releases the renderer while its GL context is alive.
        delete video_;
        video_ = nullptr;
    }
    player_->shutdown();
    delete player_;
    player_ = nullptr;
}

void CoursePreview::syncPlayback() {
    if (!hasPreview_) return;
    auto* window = this->window()->windowHandle();
    if (window != watchedWindow_) {
        if (watchedWindow_) watchedWindow_->removeEventFilter(this);
        disconnect(activeConnection_);
        watchedWindow_ = window;
        if (watchedWindow_) {
            watchedWindow_->installEventFilter(this);
            activeConnection_ = connect(watchedWindow_, &QWindow::activeChanged, this, &CoursePreview::syncPlayback);
        }
    }
    active_ = isVisible() && window && window->isActive()
        && window->windowState() != Qt::WindowMinimized
        && qApp->applicationState() == Qt::ApplicationActive;
    if (active_) {
        startPreview();
        if (loaded_) (void)player_->play();
    } else if (player_) {
        (void)player_->pause();
    }
}

void CoursePreview::updateMuteButton() {
    muteButton_->setAccessibleName(muted_ ? tr("Unmute preview") : tr("Mute preview"));
    muteButton_->setToolTip(muteButton_->accessibleName());
    muteButton_->setIcon(melearner::studyIcon(muted_ ? melearner::StudyIcon::Muted : melearner::StudyIcon::Volume,
        melearner::roleColor(this, shadcn::Role::Foreground), 1.0));
}

bool CoursePreview::eventFilter(QObject* watched, QEvent* event) {
    if (watched == this && (event->type() == QEvent::ParentChange || event->type() == QEvent::WinIdChange)) {
        syncPlayback();
    }
    if (watched == watchedWindow_ && (event->type() == QEvent::WindowActivate ||
        event->type() == QEvent::WindowDeactivate || event->type() == QEvent::WindowStateChange)) {
        syncPlayback();
    }
    if ((watched == surface_ || watched == video_ || watched == hint_) &&
        (event->type() == QEvent::Enter || event->type() == QEvent::Leave)) {
        hovered_ = event->type() == QEvent::Enter;
        continueLabel_->setVisible(hovered_ && hasPreview_);
    }
    return QWidget::eventFilter(watched, event);
}

void CoursePreview::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    syncPlayback();
}

void CoursePreview::hideEvent(QHideEvent* event) {
    active_ = false;
    if (player_) (void)player_->pause();
    QWidget::hideEvent(event);
}

void CoursePreview::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateSurface();
}
void CoursePreview::updateSurface() {
    surface_->setGeometry(rect());
    if (video_) {
        video_->setGeometry(surface_->rect());
        QPainterPath corners;
        const auto radius = themeFor(this).radius() * 1.4;
        corners.addRoundedRect(QRectF(video_->rect()), radius, radius);
        video_->setMask(QRegion(corners.toFillPolygon().toPolygon()));
    }
    hint_->setGeometry(surface_->rect());
    continueLabel_->adjustSize();
    continueLabel_->move((surface_->width() - continueLabel_->width()) / 2,
                         (surface_->height() - continueLabel_->height()) / 2);
    muteButton_->move(surface_->width() - muteButton_->width() - 12, 12);
}

int CoursePreview::heightForWidth(int width) const { return width * 9 / 16; }

}  // namespace melearner
