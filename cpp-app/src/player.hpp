#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <memory>

struct mpv_handle;
struct mpv_render_context;

namespace melearner {

struct PlayerTrack {
    int id = -1;
    QString type;
    QString title;
    QString language;
    QString codec;
    bool selected = false;
    bool external = false;
};

struct PlayerChapter {
    int index = -1;
    QString title;
    qint64 timeMs = 0;
};

class Player final : public QObject {
    Q_OBJECT

public:
    using RequestId = quint64;
    using OpenGLProcAddress = void* (*)(void* context, const char* name);
    using RenderUpdateCallback = void (*)(void* context);

    enum class DecodeMode { Automatic, Software };
    explicit Player(QObject* parent = nullptr, DecodeMode decoding = DecodeMode::Automatic);
    ~Player() override;

    Player(const Player&) = delete;
    Player& operator=(const Player&) = delete;

    // These roots are copied before worker work begins. Every media and subtitle
    // path is canonicalized and checked against them again on the Player thread.
    void setApprovedRoots(const QStringList& roots);
    [[nodiscard]] QStringList approvedRoots() const;

    // Starts the isolated libmpv worker. Initialization and all normal libmpv
    // calls happen off the Qt GUI thread.
    void start();
    // Call on this object's Qt thread so attached OpenGL widgets can release
    // their renderer synchronously before the worker and libmpv handle stop.
    void shutdown();
    [[nodiscard]] bool isReady() const;

    [[nodiscard]] RequestId loadFile(const QString& path, qint64 savedPositionMs = 0);
    [[nodiscard]] RequestId addSubtitleFile(const QString& path);
    [[nodiscard]] RequestId play();
    [[nodiscard]] RequestId pause();
    [[nodiscard]] RequestId stop();
    [[nodiscard]] RequestId seek(qint64 positionMs);
    [[nodiscard]] RequestId seekRelative(qint64 deltaMs);
    [[nodiscard]] RequestId setVolume(double volume);
    [[nodiscard]] RequestId setMuted(bool muted);
    [[nodiscard]] RequestId setRate(double rate);
    [[nodiscard]] RequestId selectAudioTrack(int trackId);
    [[nodiscard]] RequestId selectSubtitleTrack(int trackId);
    [[nodiscard]] RequestId selectChapter(int chapterIndex);
    [[nodiscard]] RequestId frameStep();
    [[nodiscard]] RequestId screenshot(const QString& outputPath);

    // Called only while the widget's QOpenGLContext is current. Render methods
    // never take the command queue lock and never wait for the Player thread.
    [[nodiscard]] bool createRenderContext(
        OpenGLProcAddress getProcAddress,
        void* getProcAddressContext,
        RenderUpdateCallback updateCallback,
        void* updateCallbackContext);
    void destroyRenderContext();
    [[nodiscard]] bool renderFrame(int framebufferObject, int width, int height,
                                   int internalFormat = 0, bool flipY = true);
    [[nodiscard]] bool hasRenderContext() const;

signals:
    // Emitted synchronously before shutdown joins the worker or destroys the
    // libmpv handle. Attached render widgets use it to release their renderer
    // while their OpenGL context is still available.
    void aboutToShutdown();
    void initialized();
    void stopped();
    // The generation is the accepted load request ID. Consumers must compare it
    // with the selected lesson to reject a stale same-path completion.
    void fileLoading(QString path, melearner::Player::RequestId generation);
    void fileLoaded(QString path, qint64 durationMs, qint64 positionMs,
                    melearner::Player::RequestId generation);
    void pausedChanged(bool paused);
    void positionChanged(qint64 positionMs, qint64 durationMs);
    void volumeChanged(double volume);
    void mutedChanged(bool muted);
    void rateChanged(double rate);
    void tracksChanged(QVector<melearner::PlayerTrack> tracks);
    void chaptersChanged(QVector<melearner::PlayerChapter> chapters);
    void decoderChanged(QString decoder);
    void playbackEnded(QString path, bool failed);
    void commandFinished(melearner::Player::RequestId requestId);
    void commandFailed(melearner::Player::RequestId requestId, QString code, QString message);
    void fatalError(QString code, QString message);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace melearner

Q_DECLARE_METATYPE(melearner::PlayerTrack)
Q_DECLARE_METATYPE(melearner::PlayerChapter)
Q_DECLARE_METATYPE(QVector<melearner::PlayerTrack>)
Q_DECLARE_METATYPE(QVector<melearner::PlayerChapter>)
