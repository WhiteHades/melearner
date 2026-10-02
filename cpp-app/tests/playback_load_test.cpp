#include "mpv_video_widget.hpp"
#include "player.hpp"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSignalSpy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>

#include <algorithm>

namespace {
qint64 privateResidentKiB() {
  QFile memory(QStringLiteral("/proc/self/smaps_rollup"));
  if (!memory.open(QIODevice::ReadOnly)) return -1;
  qint64 total = 0;
  for (const auto& rawLine : memory.readAll().split('\n')) {
    const auto line = rawLine.simplified();
    if (line.startsWith("Private_Clean:") || line.startsWith("Private_Dirty:") ||
        line.startsWith("Private_Hugetlb:")) total += line.split(' ').value(1).toLongLong();
  }
  return total;
}

bool hasNonBlackPixels(const QImage& image) {
  if (image.isNull()) return false;
  for (int y = 0; y < image.height(); y += 8)
    for (int x = 0; x < image.width(); x += 8)
      if (image.pixelColor(x, y).lightness() > 12) return true;
  return false;
}
}

class PlaybackLoadTest final : public QObject {
  Q_OBJECT
private slots:
  void sustainedPlaybackAndRepeatedTeardown() {
    const auto media = qEnvironmentVariable("MELEARNER_PLAYBACK_LOAD_FILE");
    QVERIFY2(!media.isEmpty(), "Set MELEARNER_PLAYBACK_LOAD_FILE to valid media at least 20 seconds long.");
    const QFileInfo mediaInfo(media);
    QVERIFY2(mediaInfo.isFile() && mediaInfo.isReadable(), qPrintable("Media file is unavailable: " + media));
    const auto software = qEnvironmentVariableIntValue("MELEARNER_PLAYBACK_LOAD_SOFTWARE") == 1;
    const auto mode = software ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic;
    const auto cyclesText = qEnvironmentVariable("MELEARNER_PLAYBACK_LOAD_CYCLES", QStringLiteral("4"));
    bool validCycles = false;
    const auto cycles = cyclesText.toInt(&validCycles);
    QVERIFY2(validCycles && cycles >= 1 && cycles <= 20, "Playback load cycles must be between 1 and 20.");

    qint64 previousTick = 0, worstGap = 0;
    int ticks = 0;
    QElapsedTimer heartbeatElapsed;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this, [&] {
      const auto now = heartbeatElapsed.elapsed();
      worstGap = std::max(worstGap, now - previousTick);
      previousTick = now;
      ++ticks;
    });

    qint64 firstFrameMs = -1;
    QString decoder;
    qint64 totalProgress = 0;
    qint64 minMemory = -1, maxMemory = -1;
    for (int cycle = 0; cycle < cycles; ++cycle) {
      const auto beforeMemory = privateResidentKiB();
      if (beforeMemory >= 0) {
        minMemory = minMemory < 0 ? beforeMemory : std::min(minMemory, beforeMemory);
        maxMemory = std::max(maxMemory, beforeMemory);
      }
      {
        melearner::Player player(nullptr, mode);
        player.setApprovedRoots({mediaInfo.absolutePath()});
        QWidget shell;
        auto* layout = new QVBoxLayout(&shell);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* video = new melearner::MpvVideoWidget(&player, &shell);
        layout->addWidget(video);
        shell.resize(1280, 720);
        QSignalSpy loaded(&player, &melearner::Player::fileLoaded);
        QSignalSpy positions(&player, &melearner::Player::positionChanged);
        QSignalSpy decoders(&player, &melearner::Player::decoderChanged);
        QSignalSpy pausedChanges(&player, &melearner::Player::pausedChanged);
        QSignalSpy fatal(&player, &melearner::Player::fatalError);
        QSignalSpy renderErrors(video, &melearner::MpvVideoWidget::renderError);
        shell.show();
        QVERIFY(QTest::qWaitForWindowExposed(&shell));
        player.start();
        QTRY_VERIFY_WITH_TIMEOUT(player.isReady() && video->isRenderContextReady(), 15000);
        QElapsedTimer frameTimer; frameTimer.start();
        QVERIFY(player.loadFile(mediaInfo.canonicalFilePath()));
        QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty() || !fatal.isEmpty(), 15000);
        QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.first().at(1).toString()));
        QVERIFY(player.play());
        QImage frame;
        const bool frameReady = QTest::qWaitFor([&] {
          frame = video->grabFramebuffer();
          return hasNonBlackPixels(frame);
        }, 10000);
        if (cycle == 0) firstFrameMs = frameTimer.elapsed();
        QVERIFY2(frameReady, "No nonblack rendered video frame was observed.");
        // Metadata arrives through observed properties after fileLoaded.
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(1).toLongLong() >= 20000, 5000);
        QVERIFY2(renderErrors.isEmpty(), renderErrors.isEmpty() ? "" : qPrintable(renderErrors.first().at(1).toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!decoders.isEmpty(), 5000);
        decoder = decoders.last().first().toString();
        if (software) QCOMPARE(decoder, QStringLiteral("no"));
        if (cycle == 0) {
          video->makeCurrent();
          if (video->context() && video->context()->functions()) {
            const auto renderer = video->context()->functions()->glGetString(GL_RENDERER);
            qInfo("OpenGL renderer: %s", renderer ? reinterpret_cast<const char*>(renderer) : "unknown");
          }
          video->doneCurrent();
          QVERIFY(player.pause() != 0);
          QTRY_VERIFY_WITH_TIMEOUT(!pausedChanges.isEmpty() && pausedChanges.last().first().toBool(), 5000);
          QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty(), 3000);
          const auto pausedAt = positions.last().first().toLongLong();
          const auto pausedPositionCount = positions.size();
          QTest::qWait(300);
          if (positions.size() > pausedPositionCount)
            QVERIFY(qAbs(positions.last().first().toLongLong() - pausedAt) <= 100);
          QVERIFY(player.play() != 0);
          QTRY_VERIFY_WITH_TIMEOUT(!pausedChanges.isEmpty() && !pausedChanges.last().first().toBool(), 5000);
          QVERIFY(player.seek(pausedAt + 1000) != 0);
          QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().first().toLongLong() >= pausedAt + 900, 5000);
        }
        const auto startPosition = positions.isEmpty() ? 0 : positions.last().first().toLongLong();
        heartbeatElapsed.start();
        previousTick = 0;
        heartbeat.start();
        QTest::qWait(5000);
        const auto progress = positions.last().first().toLongLong() - startPosition;
        totalProgress += progress;
        QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.first().at(1).toString()));
        QVERIFY2(renderErrors.isEmpty(), renderErrors.isEmpty() ? "" : qPrintable(renderErrors.first().at(1).toString()));
        heartbeat.stop();
        qInfo("Cycle %d playback progress: %lld ms in %lld ms", cycle + 1, progress, heartbeatElapsed.elapsed());
        QVERIFY2(progress >= 4000, "Playback advanced less than four seconds in the five-second sample.");
        QVERIFY(ticks > 0);
        QVERIFY(player.pause() != 0);
        shell.hide();
        player.shutdown();
      }
      QCoreApplication::processEvents();
      const auto afterMemory = privateResidentKiB();
      if (afterMemory >= 0) {
        minMemory = minMemory < 0 ? afterMemory : std::min(minMemory, afterMemory);
        maxMemory = std::max(maxMemory, afterMemory);
      }
      qInfo("Cycle %d private resident memory: before %lld KiB, after %lld KiB", cycle + 1, beforeMemory, afterMemory);
    }
    QVERIFY2(totalProgress > 0, "Playback position did not progress.");
    QVERIFY(ticks > 0);
    qInfo("Playback load diagnostic: decoder=%s, first frame=%lld ms, heartbeat=%d samples, worst gap=%lld ms, position progress=%lld ms, private resident range=%lld..%lld KiB",
          qPrintable(decoder), firstFrameMs, ticks, worstGap, totalProgress, minMemory, maxMemory);
  }
};

QTEST_MAIN(PlaybackLoadTest)
#include "playback_load_test.moc"
