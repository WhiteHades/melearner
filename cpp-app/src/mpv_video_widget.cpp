#include "mpv_video_widget.hpp"

#include "player.hpp"

#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLBuffer>
#include <QMetaObject>
#include <QAccessibleWidget>
#include <QWindow>
#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QApplication>
#include <QTimer>

#include <thread>
#include <new>
#include <utility>

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

struct BackdropGlState {
    explicit BackdropGlState(QOpenGLExtraFunctions* functions) : gl(functions) {
        gl->glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        gl->glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        gl->glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        gl->glGetIntegerv(GL_VIEWPORT, viewport);
        gl->glGetIntegerv(GL_READ_BUFFER, &readBuffer);
        gl->glGetIntegerv(GL_DRAW_BUFFER, &drawBuffer);
        gl->glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        gl->glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        gl->glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        gl->glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        gl->glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEqRgb);
        gl->glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEqAlpha);
        blend = gl->glIsEnabled(GL_BLEND);
        scissor = gl->glIsEnabled(GL_SCISSOR_TEST);
        depth = gl->glIsEnabled(GL_DEPTH_TEST);
        stencil = gl->glIsEnabled(GL_STENCIL_TEST);
        cull = gl->glIsEnabled(GL_CULL_FACE);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
        gl->glActiveTexture(activeTexture);
    }
    ~BackdropGlState() {
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFbo));
        gl->glReadBuffer(GLenum(readBuffer));
        gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(drawFbo));
        const GLenum drawBuffers[] = {GLenum(drawBuffer)};
        gl->glDrawBuffers(1, drawBuffers);
        gl->glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        gl->glUseProgram(GLuint(program));
        gl->glBindVertexArray(GLuint(vao));
        gl->glBindBuffer(GL_ARRAY_BUFFER, GLuint(arrayBuffer));
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(GL_TEXTURE_2D, GLuint(texture0));
        gl->glActiveTexture(GLenum(activeTexture));
        gl->glBlendFuncSeparate(GLenum(blendSrcRgb), GLenum(blendDstRgb), GLenum(blendSrcAlpha), GLenum(blendDstAlpha));
        gl->glBlendEquationSeparate(GLenum(blendEqRgb), GLenum(blendEqAlpha));
        set(GL_BLEND, blend);
        set(GL_SCISSOR_TEST, scissor);
        set(GL_DEPTH_TEST, depth);
        set(GL_STENCIL_TEST, stencil);
        set(GL_CULL_FACE, cull);
    }
    void set(GLenum capability, GLboolean enabled) {
        if (enabled) gl->glEnable(capability); else gl->glDisable(capability);
    }
    QOpenGLExtraFunctions* gl;
    GLint readFbo = 0, drawFbo = 0, program = 0, vao = 0, arrayBuffer = 0;
    GLint activeTexture = GL_TEXTURE0, texture0 = 0, viewport[4] = {};
    GLint readBuffer = 0, drawBuffer = 0;
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
    GLint blendEqRgb = 0, blendEqAlpha = 0;
    GLboolean blend = GL_FALSE, scissor = GL_FALSE, depth = GL_FALSE, stencil = GL_FALSE, cull = GL_FALSE;
};

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
    setMinimumSize(0, 0);
    setAutoFillBackground(false);
    doubleClickCandidateTimer_ = new QTimer(this);
    doubleClickCandidateTimer_->setSingleShot(true);
    connect(doubleClickCandidateTimer_, &QTimer::timeout, this, [this] { doubleClickCandidate_ = false; });
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
    connect(player, &Player::fileLoaded, this, [this] {
        doubleClickCandidateTimer_->stop();
        doubleClickCandidate_ = false;
    });
    connect(player, &Player::playbackEnded, this, [this] {
        doubleClickCandidateTimer_->stop();
        doubleClickCandidate_ = false;
    });
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

void MpvVideoWidget::setCornerRadii(qreal topLeft, qreal topRight, qreal bottomRight,
                                    qreal bottomLeft, QColor background) {
    if (topLeftRadius_ == topLeft && topRightRadius_ == topRight &&
        bottomRightRadius_ == bottomRight && bottomLeftRadius_ == bottomLeft &&
        cornerBackground_ == background) return;
    topLeftRadius_ = qMax<qreal>(0.0, topLeft);
    topRightRadius_ = qMax<qreal>(0.0, topRight);
    bottomRightRadius_ = qMax<qreal>(0.0, bottomRight);
    bottomLeftRadius_ = qMax<qreal>(0.0, bottomLeft);
    cornerBackground_ = std::move(background);
    update();
}

void MpvVideoWidget::setTransportBackdrop(QRect logicalRect, qreal visibility) {
    visibility = qBound<qreal>(0.0, visibility, 1.0);
    if (transportBackdropRect_ == logicalRect && qFuzzyCompare(transportBackdropVisibility_ + 1.0, visibility + 1.0)) return;
    transportBackdropRect_ = logicalRect;
    transportBackdropVisibility_ = visibility;
    update();
}

void MpvVideoWidget::initializeGL() {
    const auto renderer = QByteArray(reinterpret_cast<const char*>(glGetString(GL_RENDERER))).toLower();
    // Mesa's CPU OpenGL drivers can corrupt libmpv's shader output. Let mpv
    // convert and scale on the CPU, then let Qt present the opaque image. The
    // hardware OpenGL path remains unchanged.
    softwareRendering_ = renderer.contains("llvmpipe") || renderer.contains("softpipe");
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
    // Qt enables blending while preparing QOpenGLWidget painting. libmpv
    // requires its default (disabled), including for intermediate plane passes.
    glDisable(GL_BLEND);
    renderDirty_.store(false, std::memory_order_release);
    const auto pixelRatio = devicePixelRatioF();
    const auto pixelWidth = qMax(1, qRound(width() * pixelRatio));
    const auto pixelHeight = qMax(1, qRound(height() * pixelRatio));
    if (player_ == nullptr || !renderContextReady_) {
        glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (softwareRendering_) {
        if (!renderSoftwareFrame(pixelWidth, pixelHeight)) {
            glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT);
            emit renderError(QStringLiteral("render"), QStringLiteral("The software video frame could not be rendered."));
        }
    } else if (!player_->renderFrame(defaultFramebufferObject(), pixelWidth, pixelHeight)) {
        emit renderError(QStringLiteral("render"), QStringLiteral("libmpv could not render the current frame."));
    }
    paintTransportBackdrop();
    paintCornerCover();
}

struct MpvVideoWidget::BackdropResources {
    std::unique_ptr<QOpenGLFramebufferObject> crop;
    std::unique_ptr<QOpenGLShaderProgram> program;
    std::unique_ptr<QOpenGLVertexArrayObject> vao;
    std::unique_ptr<QOpenGLBuffer> vertices;
    QSize cropSize;
};

bool MpvVideoWidget::paintTransportBackdrop() {
    if (transportBackdropVisibility_ <= 0.0 || transportBackdropRect_.isEmpty()) return false;
    const qreal ratio = devicePixelRatioF();
    const QRect bounds(0, 0, qRound(width() * ratio), qRound(height() * ratio));
    const QRect cropRect = QRect(qRound(transportBackdropRect_.x() * ratio),
                                 qRound(transportBackdropRect_.y() * ratio),
                                 qRound(transportBackdropRect_.width() * ratio),
                                 qRound(transportBackdropRect_.height() * ratio)).intersected(bounds);
    if (cropRect.width() < 3 || cropRect.height() < 3 || cropRect.width() > 2048 ||
        cropRect.height() > 512 || context() == nullptr) return false;

    auto* gl = context()->extraFunctions();
    if (gl == nullptr) return false;
    gl->initializeOpenGLFunctions();
    BackdropGlState restoreState(gl);
    const QSize smallSize(qMax(1, (cropRect.width() + 2) / 3), qMax(1, (cropRect.height() + 2) / 3));
    if (!backdropResources_) backdropResources_ = std::make_unique<BackdropResources>();
    auto& resources = *backdropResources_;
    if (!resources.program) {
        resources.program = std::make_unique<QOpenGLShaderProgram>();
        static const char vertexShader[] =
            "attribute vec2 position; varying vec2 uv;"
            "void main(){ uv=position*0.5+0.5; gl_Position=vec4(position,0.0,1.0); }";
        static const char fragmentShader[] =
            "uniform sampler2D image; uniform vec2 texel; uniform vec2 size; uniform float visibility; uniform float radius;"
            "varying vec2 uv; void main(){"
            "vec2 p=uv*size; float r=radius;"
            "vec2 c=clamp(p,vec2(r),size-vec2(r));"
            "float coverage=1.0-smoothstep(r-1.0,r+1.0,length(p-c));"
            "if(coverage<=0.0) discard;"
            "vec4 s=vec4(0.0);"
            "s+=texture2D(image,uv+texel*vec2(-1.0,-1.0));"
            "s+=2.0*texture2D(image,uv+texel*vec2(0.0,-1.0));"
            "s+=texture2D(image,uv+texel*vec2(1.0,-1.0));"
            "s+=2.0*texture2D(image,uv+texel*vec2(-1.0,0.0));"
            "s+=4.0*texture2D(image,uv);"
            "s+=2.0*texture2D(image,uv+texel*vec2(1.0,0.0));"
            "s+=texture2D(image,uv+texel*vec2(-1.0,1.0));"
            "s+=2.0*texture2D(image,uv+texel*vec2(0.0,1.0));"
            "s+=texture2D(image,uv+texel*vec2(1.0,1.0)); gl_FragColor=vec4((s/16.0).rgb,visibility*coverage); }";
        if (!resources.program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader) ||
            !resources.program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader)) {
            resources.program.reset();
            return false;
        }
        resources.program->bindAttributeLocation("position", 0);
        if (!resources.program->link()) { resources.program.reset(); return false; }
        resources.vao = std::make_unique<QOpenGLVertexArrayObject>();
        resources.vertices = std::make_unique<QOpenGLBuffer>(QOpenGLBuffer::VertexBuffer);
        if (!resources.vao->create() || !resources.vertices->create()) {
            resources.vertices.reset(); resources.vao.reset(); resources.program.reset();
            return false;
        }
        static const GLfloat quad[] = {-1.f,-1.f, 1.f,-1.f, -1.f,1.f, 1.f,1.f};
        resources.vao->bind();
        resources.vertices->bind();
        resources.vertices->allocate(quad, sizeof(quad));
        resources.program->bind();
        resources.program->enableAttributeArray(0);
        resources.program->setAttributeBuffer(0, GL_FLOAT, 0, 2, 2 * sizeof(GLfloat));
        resources.program->release();
        resources.vertices->release();
        resources.vao->release();
    }
    if (!resources.crop || resources.cropSize != smallSize) {
        resources.crop.reset();
        QOpenGLFramebufferObjectFormat format;
        format.setAttachment(QOpenGLFramebufferObject::NoAttachment);
        format.setTextureTarget(GL_TEXTURE_2D);
        format.setInternalTextureFormat(GL_RGBA8);
        resources.crop = std::make_unique<QOpenGLFramebufferObject>(smallSize, format);
        if (!resources.crop->isValid()) { resources.crop.reset(); return false; }
        resources.cropSize = smallSize;
    }

    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, defaultFramebufferObject());
    gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resources.crop->handle());
    const GLenum colorAttachment = GL_COLOR_ATTACHMENT0;
    gl->glDrawBuffers(1, &colorAttachment);
    gl->glDisable(GL_SCISSOR_TEST);
    gl->glBlitFramebuffer(cropRect.x(), bounds.height() - cropRect.bottom() - 1,
                          cropRect.x() + cropRect.width(), bounds.height() - cropRect.y(),
                          0, 0, smallSize.width(), smallSize.height(), GL_COLOR_BUFFER_BIT, GL_LINEAR);

    gl->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, defaultFramebufferObject());
    gl->glViewport(cropRect.x(), bounds.height() - cropRect.y() - cropRect.height(), cropRect.width(), cropRect.height());
    gl->glEnable(GL_BLEND);
    gl->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    gl->glBlendEquation(GL_FUNC_ADD);
    gl->glDisable(GL_DEPTH_TEST);
    gl->glDisable(GL_STENCIL_TEST);
    gl->glDisable(GL_CULL_FACE);
    resources.program->bind();
    gl->glBindVertexArray(resources.vao->objectId());
    gl->glBindBuffer(GL_ARRAY_BUFFER, resources.vertices->bufferId());
    gl->glActiveTexture(GL_TEXTURE0);
    gl->glBindTexture(GL_TEXTURE_2D, resources.crop->texture());
    resources.program->setUniformValue("image", 0);
    resources.program->setUniformValue("texel", 1.0F / smallSize.width(), 1.0F / smallSize.height());
    resources.program->setUniformValue("size", GLfloat(cropRect.width()), GLfloat(cropRect.height()));
    resources.program->setUniformValue("radius", GLfloat(qMin(28.0 * ratio, cropRect.height() / 2.0)));
    resources.program->setUniformValue("visibility", GLfloat(transportBackdropVisibility_));
    gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    return true;
}

void MpvVideoWidget::destroyTransportBackdrop() {
    backdropResources_.reset();
}

void MpvVideoWidget::paintCornerCover() {
    const QRectF bounds(rect());
    if (bounds.isEmpty() || (topLeftRadius_ <= 0.0 && topRightRadius_ <= 0.0 &&
                            bottomRightRadius_ <= 0.0 && bottomLeftRadius_ <= 0.0)) {
        return;
    }

    const qreal maxRadius = qMin(bounds.width(), bounds.height()) / 2.0;
    const qreal tl = qMin(topLeftRadius_, maxRadius);
    const qreal tr = qMin(topRightRadius_, maxRadius);
    const qreal br = qMin(bottomRightRadius_, maxRadius);
    const qreal bl = qMin(bottomLeftRadius_, maxRadius);

    QPainterPath cover;
    cover.setFillRule(Qt::OddEvenFill);
    cover.addRect(bounds);
    QPainterPath rounded;
    rounded.moveTo(bounds.left() + tl, bounds.top());
    rounded.lineTo(bounds.right() - tr, bounds.top());
    if (tr > 0) rounded.arcTo(QRectF(bounds.right() - 2 * tr, bounds.top(), 2 * tr, 2 * tr), 90, -90);
    rounded.lineTo(bounds.right(), bounds.bottom() - br);
    if (br > 0) rounded.arcTo(QRectF(bounds.right() - 2 * br, bounds.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
    rounded.lineTo(bounds.left() + bl, bounds.bottom());
    if (bl > 0) rounded.arcTo(QRectF(bounds.left(), bounds.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
    rounded.lineTo(bounds.left(), bounds.top() + tl);
    if (tl > 0) rounded.arcTo(QRectF(bounds.left(), bounds.top(), 2 * tl, 2 * tl), 180, -90);
    rounded.closeSubpath();
    cover.addPath(rounded);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(cornerBackground_);
    painter.drawPath(cover);
}

bool MpvVideoWidget::renderSoftwareFrame(int width, int height) {
    if (softwareFrame_.size() != QSize(width, height)) {
        if (width <= 0 || height <= 0 || width > 16384 || height > 16384) return false;
        const auto stride = (static_cast<qsizetype>(width) * 4 + 63) & ~qsizetype(63);
        const auto bytes = stride * height;
        // An 8K presentation fits, but a corrupt/extreme window geometry must
        // not cause an unbounded allocation on the GUI thread.
        if (bytes > 128 * 1024 * 1024) return false;
        softwareFrame_ = {};
        try {
            softwarePixels_.resize(bytes + 63);
        } catch (const std::bad_alloc&) {
            return false;
        }
        const auto aligned = (reinterpret_cast<quintptr>(softwarePixels_.data()) + 63) & ~quintptr(63);
        softwareFrame_ = QImage(reinterpret_cast<uchar*>(aligned), width, height, stride, QImage::Format_RGBX8888);
    }
    if (!player_->renderSoftwareFrame(softwareFrame_)) return false;
    QPainter painter(this);
    painter.drawImage(rect(), softwareFrame_);
    return true;
}

void MpvVideoWidget::resizeGL(int, int) { requestFrame(); }
void MpvVideoWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        clickOrigin_ = event->position();
        setFocus(Qt::MouseFocusReason);
        event->accept();
        return;
    }
    QOpenGLWidget::mousePressEvent(event);
}
void MpvVideoWidget::mouseReleaseEvent(QMouseEvent* event) {
    const auto origin = std::exchange(clickOrigin_, std::nullopt);
    if (event->button() == Qt::LeftButton && origin && rect().contains(event->position().toPoint()) &&
        (event->position() - *origin).manhattanLength() < QApplication::startDragDistance()) {
        doubleClickCandidate_ = true;
        doubleClickCandidateTimer_->start(QApplication::doubleClickInterval());
        emit clicked();
        event->accept();
        return;
    }
    QOpenGLWidget::mouseReleaseEvent(event);
}
void MpvVideoWidget::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        QOpenGLWidget::mouseDoubleClickEvent(event);
        return;
    }
    doubleClickCandidateTimer_->stop();
    const bool revertSingleClick = doubleClickCandidate_;
    doubleClickCandidate_ = false;
    clickOrigin_.reset();
    emit seekRequested(event->position().x() < width() / 2.0 ? -5000 : 5000, revertSingleClick);
    event->accept();
}

void MpvVideoWidget::attachRenderContext() {
    if (renderContextReady_ || player_ == nullptr || !player_->isReady() || context() == nullptr) {
        return;
    }
    callbackState_->active.store(true, std::memory_order_release);
    if (!player_->createRenderContext(&resolveOpenGLProc, context(), &MpvVideoWidget::renderUpdateCallback,
                                      callbackState_.get(), softwareRendering_ ? Player::RenderMode::Software
                                                                             : Player::RenderMode::OpenGL)) {
        emit renderError(QStringLiteral("renderer_init"),
                         QStringLiteral("The video renderer could not be initialized."));
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
    if (player_ != nullptr && player_->hasRenderContext() && (currentForRender || softwareRendering_)) {
        player_->destroyRenderContext();
    }
    if (currentForRender) destroyTransportBackdrop();
    while (callbackState_->inFlight.load(std::memory_order_acquire) != 0U) {
        std::this_thread::yield();
    }
    renderDirty_.store(false, std::memory_order_release);
    updateQueued_.store(false, std::memory_order_release);
    softwareFrame_ = {};
    softwarePixels_.clear();
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
            const auto* surface = window()->windowHandle();
            if (isVisible() && !window()->isMinimized() && surface != nullptr && surface->isExposed()) {
                update();
            } else if (renderContextReady_ && player_ != nullptr && context() != nullptr) {
                // Qt does not paint hidden or minimized widgets. Advanced
                // libmpv rendering still needs callbacks serviced on the GL
                // owner thread so decoder allocations and playback can run.
                makeCurrent();
                if (QOpenGLContext::currentContext() == context()) {
                    glDisable(GL_BLEND);
                    if (!player_->processHiddenRenderUpdate()) {
                        emit renderError(QStringLiteral("render"),
                                         QStringLiteral("libmpv could not process the hidden frame."));
                    }
                    context()->functions()->glBindFramebuffer(GL_FRAMEBUFFER, defaultFramebufferObject());
                    doneCurrent();
                }
            }
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
