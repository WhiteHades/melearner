#include "mpv_video_widget.hpp"

#include "player.hpp"

#include <QOpenGLContext>
#include <QMetaObject>
#include <QAccessibleWidget>

#include <thread>

namespace melearner {
namespace {

class AccessibleVideo final : public QAccessibleWidget, public QAccessibleImageInterface {
public:
    explicit AccessibleVideo(MpvVideoWidget* video) : QAccessibleWidget(video, QAccessible::Animation) {}
    void* interface_cast(QAccessible::InterfaceType type) override {
        if (type == QAccessible::ImageInterface) return static_cast<QAccessibleImageInterface*>(this);
        return QAccessibleWidget::interface_cast(type);
    }
    QString imageDescription() const override { return widget()->accessibleName(); }
    QSize imageSize() const override { return widget()->size(); }
    QPoint imagePosition() const override { return widget()->mapToGlobal(QPoint()); }
};

QAccessibleInterface* accessibleVideo(const QString&, QObject* object) {
    if (auto* video = qobject_cast<MpvVideoWidget*>(object)) return new AccessibleVideo(video);
    return nullptr;
}

void* resolveOpenGLProc(void* context, const char* name) {
    auto* glContext = static_cast<QOpenGLContext*>(context);
    if (glContext == nullptr || name == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(glContext->getProcAddress(name));
}

// Draw the focus border without introducing Qt's OpenGL paint-engine state
// into libmpv rendering. Scissored clears need no shader or vertex buffers.
void drawFocusBorder(int width, int height, qreal scale) {
    const int thickness = qMax(1, qRound(2 * scale));
    const auto rectangle = [width, height, thickness](int inset, GLfloat color) {
        const int w = width - 2 * inset;
        const int h = height - 2 * inset;
        if (w <= 0 || h <= 0) return;
        glClearColor(color, color, color, 1.0F);
        glScissor(inset, inset, w, qMin(thickness, h));
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(inset, height - inset - qMin(thickness, h), w, qMin(thickness, h));
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(inset, inset, qMin(thickness, w), h);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(width - inset - qMin(thickness, w), inset, qMin(thickness, w), h);
        glClear(GL_COLOR_BUFFER_BIT);
    };
    glEnable(GL_SCISSOR_TEST);
    rectangle(qMax(1, qRound(scale)), 0.0F);
    rectangle(qMax(1, qRound(3 * scale)), 1.0F);
    glDisable(GL_SCISSOR_TEST);
}

}  // namespace

struct MpvVideoWidget::CallbackState {
    std::atomic<bool> active{true};
    std::atomic<unsigned> inFlight{0};
    QPointer<MpvVideoWidget> widget;
};

MpvVideoWidget::MpvVideoWidget(Player* player, QWidget* parent)
    : QOpenGLWidget(parent) {
    static const bool registered = [] { QAccessible::installFactory(accessibleVideo); return true; }();
    Q_UNUSED(registered);
    callbackState_ = std::make_unique<CallbackState>();
    callbackState_->widget = this;
    setUpdateBehavior(QOpenGLWidget::NoPartialUpdate);
    setMinimumSize(320, 180);
    setAutoFillBackground(false);
    setPlayer(player);
}

MpvVideoWidget::~MpvVideoWidget() {
    QObject::disconnect(contextDestroyConnection_);
    detachRenderContext();
    callbackState_->widget = nullptr;
}

void MpvVideoWidget::setPlayer(Player* player) {
    if (player_ == player) {
        return;
    }
    detachRenderContext();
    if (player_ != nullptr) disconnect(player_, nullptr, this, nullptr);
    player_ = player;
    if (player_ == nullptr) {
        return;
    }
    connectPlayer(player_);
    if (context() != nullptr && player_->isReady()) {
        makeCurrent();
        if (QOpenGLContext::currentContext() == context()) {
            attachRenderContext();
            doneCurrent();
        }
    }
}

void MpvVideoWidget::connectPlayer(Player* player) {
    if (player == nullptr) {
        return;
    }
    connect(player, &QObject::destroyed, this, [this] {
        player_ = nullptr;
        callbackState_->active.store(false, std::memory_order_release);
        while (callbackState_->inFlight.load(std::memory_order_acquire) != 0U) {
            std::this_thread::yield();
        }
        renderContextReady_ = false;
        renderDirty_.store(false, std::memory_order_release);
        updateQueued_.store(false, std::memory_order_release);
        emit renderContextLost();
    });
    connect(player, &Player::aboutToShutdown, this, [this] {
        detachRenderContext();
    }, Qt::DirectConnection);
    connect(player, &Player::initialized, this, [this] {
        if (context() != nullptr) {
            makeCurrent();
        }
        if (context() != nullptr && QOpenGLContext::currentContext() == context()) {
            attachRenderContext();
            doneCurrent();
        }
    }, Qt::QueuedConnection);
}

Player* MpvVideoWidget::player() const { return player_; }

bool MpvVideoWidget::isRenderContextReady() const { return renderContextReady_; }

void MpvVideoWidget::initializeGL() {
    if (context() != nullptr) {
        QObject::disconnect(contextDestroyConnection_);
        contextDestroyConnection_ = connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this] {
            bool madeCurrent = false;
            if (context() != nullptr && QOpenGLContext::currentContext() != context()) {
                makeCurrent();
                madeCurrent = QOpenGLContext::currentContext() == context();
            }
            detachRenderContext();
            if (madeCurrent) {
                doneCurrent();
            }
        }, Qt::DirectConnection);
    }
    attachRenderContext();
}

void MpvVideoWidget::paintGL() {
    renderDirty_.store(false, std::memory_order_release);
    const auto pixelRatio = devicePixelRatioF();
    const auto pixelWidth = qMax(1, qRound(width() * pixelRatio));
    const auto pixelHeight = qMax(1, qRound(height() * pixelRatio));
    if (player_ == nullptr || !renderContextReady_) {
        glClearColor(0.09F, 0.07F, 0.06F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (!player_->renderFrame(defaultFramebufferObject(), pixelWidth, pixelHeight)) {
        emit renderError(QStringLiteral("render"), QStringLiteral("libmpv could not render the current frame."));
    }
    if (hasFocus()) {
        drawFocusBorder(pixelWidth, pixelHeight, pixelRatio);
    }
}

void MpvVideoWidget::resizeGL(int, int) { requestFrame(); }
void MpvVideoWidget::focusInEvent(QFocusEvent* event) { QOpenGLWidget::focusInEvent(event); update(); }
void MpvVideoWidget::focusOutEvent(QFocusEvent* event) { QOpenGLWidget::focusOutEvent(event); update(); }

void MpvVideoWidget::attachRenderContext() {
    if (renderContextReady_ || player_ == nullptr || !player_->isReady() || context() == nullptr) {
        return;
    }
    callbackState_->active.store(true, std::memory_order_release);
    if (!player_->createRenderContext(&resolveOpenGLProc, context(), &MpvVideoWidget::renderUpdateCallback,
                                      callbackState_.get())) {
        emit renderError(QStringLiteral("renderer_init"),
                         QStringLiteral("The OpenGL video renderer could not be initialized."));
        return;
    }
    renderContextReady_ = true;
    emit renderContextReady();
    requestFrame();
}

void MpvVideoWidget::detachRenderContext() {
    if (cleaningUp_) {
        return;
    }
    cleaningUp_ = true;
    callbackState_->active.store(false, std::memory_order_release);
    const auto* widgetContext = context();
    const bool alreadyCurrent = widgetContext != nullptr && QOpenGLContext::currentContext() == widgetContext;
    bool madeCurrent = false;
    if (!alreadyCurrent && widgetContext != nullptr) {
        makeCurrent();
        madeCurrent = QOpenGLContext::currentContext() == widgetContext;
    }
    const bool currentForRender = widgetContext != nullptr
        && QOpenGLContext::currentContext() == widgetContext;
    if (player_ != nullptr && player_->hasRenderContext() && currentForRender) {
        player_->destroyRenderContext();
    }
    while (callbackState_->inFlight.load(std::memory_order_acquire) != 0U) {
        std::this_thread::yield();
    }
    renderDirty_.store(false, std::memory_order_release);
    updateQueued_.store(false, std::memory_order_release);
    if (renderContextReady_) {
        renderContextReady_ = false;
        emit renderContextLost();
    }
    if (madeCurrent) {
        doneCurrent();
    }
    cleaningUp_ = false;
}

void MpvVideoWidget::requestFrame() {
    if (renderContextReady_) {
        update();
    }
}

void MpvVideoWidget::renderUpdateCallback(void* context) {
    auto* state = static_cast<CallbackState*>(context);
    if (state == nullptr) {
        return;
    }
    state->inFlight.fetch_add(1, std::memory_order_acq_rel);
    if (!state->active.load(std::memory_order_acquire)) {
        state->inFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    const QPointer<MpvVideoWidget> widget = state->widget;
    if (!widget) {
        state->inFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    widget->renderDirty_.store(true, std::memory_order_release);
    bool expected = false;
    if (!widget->updateQueued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        state->inFlight.fetch_sub(1, std::memory_order_acq_rel);
        return;
    }
    QMetaObject::invokeMethod(widget, [widget] {
        if (!widget) {
            return;
        }
        widget->deliverRenderUpdate();
    }, Qt::QueuedConnection);
    state->inFlight.fetch_sub(1, std::memory_order_acq_rel);
}

void MpvVideoWidget::deliverRenderUpdate() {
    for (;;) {
        if (renderDirty_.exchange(false, std::memory_order_acq_rel)) {
            update();
        }
        updateQueued_.store(false, std::memory_order_release);
        if (!renderDirty_.load(std::memory_order_acquire)) {
            return;
        }
        bool expected = false;
        if (!updateQueued_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            return;
        }
    }
}

}  // namespace melearner
