#pragma once

#include <QOpenGLWidget>
#include <QMetaObject>
#include <QPointer>

#include <atomic>
#include <memory>

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

signals:
    void renderContextReady();
    void renderContextLost();
    void renderError(QString code, QString message);

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int width, int height) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    struct CallbackState;

    void attachRenderContext();
    void detachRenderContext();
    void requestFrame();
    void deliverRenderUpdate();
    static void renderUpdateCallback(void* context);
    void connectPlayer(Player* player);

    QPointer<Player> player_;
    std::unique_ptr<CallbackState> callbackState_;
    QMetaObject::Connection contextDestroyConnection_;
    std::atomic<bool> renderDirty_{false};
    std::atomic<bool> updateQueued_{false};
    bool renderContextReady_ = false;
    bool cleaningUp_ = false;
};

}  // namespace melearner
