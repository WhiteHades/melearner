#pragma once

#include <QOpenGLWidget>
#include <QMetaObject>
#include <QPointer>
#include <QImage>
#include <QByteArray>
#include <QColor>

#include <atomic>
#include <memory>
#include <optional>
class QTimer;

namespace melearner {

class Player;

class MpvVideoWidget final : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit MpvVideoWidget(Player* player = nullptr, QWidget* parent = nullptr);
    ~MpvVideoWidget() override;

    MpvVideoWidget(const MpvVideoWidget&) = delete;
    MpvVideoWidget& operator=(const MpvVideoWidget&) = delete;

    void setPlayer(Player* player);
    [[nodiscard]] Player* player() const;
    [[nodiscard]] bool isRenderContextReady() const;
    void setCornerRadii(qreal topLeft, qreal topRight, qreal bottomRight, qreal bottomLeft,
                        QColor background);
    void setTransportBackdrop(QRect logicalRect, qreal visibility);

signals:
    void clicked();
    void seekRequested(qint64 milliseconds, bool revertSingleClick);
    void fullscreenRequested(bool revertSingleClick);
    void renderContextReady();
    void renderContextLost();
    void renderError(QString code, QString message);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    struct CallbackState;

    void attachRenderContext();
    void detachRenderContext();
    void requestFrame();
    void deliverRenderUpdate();
    static void renderUpdateCallback(void* context);
    void connectPlayer(Player* player);
    bool renderSoftwareFrame(int width, int height);
    bool paintTransportBackdrop();
    void destroyTransportBackdrop();

    QPointer<Player> player_;
    std::unique_ptr<CallbackState> callbackState_;
    QMetaObject::Connection contextDestroyConnection_;
    std::atomic<bool> renderDirty_{false};
    std::atomic<bool> updateQueued_{false};
    bool renderContextReady_ = false;
    bool cleaningUp_ = false;
    bool softwareRendering_ = false;
    std::optional<QPointF> clickOrigin_;
    QTimer* doubleClickCandidateTimer_ = nullptr;
    bool doubleClickCandidate_ = false;
    QByteArray softwarePixels_;
    QImage softwareFrame_;
    QWidget* cornerCover_ = nullptr;
    QRect transportBackdropRect_;
    qreal transportBackdropVisibility_ = 0.0;
    struct BackdropResources;
    std::unique_ptr<BackdropResources> backdropResources_;
};

}  // namespace melearner
