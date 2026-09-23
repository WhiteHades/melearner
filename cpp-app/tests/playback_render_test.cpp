#include "mpv_video_widget.hpp"
#include "player.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>

namespace {
// The generated corpus has a solid yellow bar here. Nonblank/changing-frame
// checks alone do not detect the black grid caused by corrupted GL state.
bool hasIntactColorBar(const QImage& image) {
  if (image.isNull()) return false;
  int yellow = 0;
  int total = 0;
  for (int y = image.height() * 55 / 100; y < image.height() * 65 / 100; ++y)
    for (int x = image.width() * 38 / 100; x < image.width() * 43 / 100; ++x) {
      const auto pixel = image.pixel(x, y);
      yellow += qRed(pixel) > 180 && qGreen(pixel) > 180 && qBlue(pixel) < 100;
      ++total;
    }
  return total > 0 && yellow * 10 >= total * 9;
}
}

class PlaybackRenderTest final : public QObject {
  Q_OBJECT
private slots:
  void rendersSoftwareDecodedFrames_data() {
    QTest::addColumn<QString>("relativePath");
    QTest::addColumn<bool>("softwareDecoding");
    QTest::newRow("h264") << "Systems 日本語/01 H264 AAC.mp4" << true;
    QTest::newRow("hevc-main10") << "03 HEVC Main 10.mkv" << true;
    QTest::newRow("multi-audio") << "02 Multi audio chapters.mkv" << true;
    QTest::newRow("automatic-decoder") << "Systems 日本語/01 H264 AAC.mp4" << false;
  }
  void rendersSoftwareDecodedFrames() {
    QFETCH(QString, relativePath);
    QFETCH(bool, softwareDecoding);
    const auto root = QDir(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media").canonicalPath();
    QVERIFY2(!root.isEmpty(), "Checked-in media corpus missing");
    melearner::Player player(nullptr, softwareDecoding ? melearner::Player::DecodeMode::Software : melearner::Player::DecodeMode::Automatic);
    QTemporaryDir output; QVERIFY(output.isValid());
    connect(&player, &melearner::Player::aboutToShutdown, &player, [] {
      QTest::qSleep(100);
    }, Qt::DirectConnection);
    QWidget shell;
    shell.setObjectName(QStringLiteral("renderComposite"));
    shell.setAttribute(Qt::WA_StyledBackground);
    shell.setStyleSheet(QStringLiteral("QWidget#renderComposite { background: #1b1917; }"));
    shell.resize(640, 360);
    auto* shellLayout = new QVBoxLayout(&shell);
    shellLayout->setContentsMargins(0, 0, 0, 0);
    melearner::MpvVideoWidget video(&player, &shell);
    shellLayout->addWidget(&video);
    QSignalSpy rendered(&video, &melearner::MpvVideoWidget::renderContextReady);
    QSignalSpy renderErrors(&video, &melearner::MpvVideoWidget::renderError);
    QSignalSpy loaded(&player, &melearner::Player::fileLoaded);
    QSignalSpy errors(&player, &melearner::Player::commandFailed);
    QSignalSpy fatal(&player, &melearner::Player::fatalError);
    QSignalSpy decoders(&player, &melearner::Player::decoderChanged);
    connect(&player, &melearner::Player::commandFailed, &video,
      [](melearner::Player::RequestId id, const QString& code, const QString& message) {
        qWarning().noquote() << "Player command failed:" << id << code << message;
      });
    connect(&player, &melearner::Player::playbackEnded, &video,
      [](const QString&, bool failed) { qInfo() << "Playback ended; failed:" << failed; });
    QSignalSpy positions(&player, &melearner::Player::positionChanged);
    QSignalSpy commands(&player, &melearner::Player::commandFinished);
    const auto hasReply = [](const QSignalSpy& replies, melearner::Player::RequestId id) {
      for (const auto& reply : replies) if (reply.first().toULongLong() == id) return true;
      return false;
    };
    player.setApprovedRoots({root, output.path()});
    QWidget controls(&video);
    controls.setObjectName(QStringLiteral("playerControls"));
    controls.setAttribute(Qt::WA_StyledBackground);
    controls.setStyleSheet(QStringLiteral("QWidget#playerControls { background: rgba(18, 18, 18, 235); }"));
    auto* controlsOpacity = new QGraphicsOpacityEffect(&controls);
    controlsOpacity->setOpacity(1.0);
    controls.setGraphicsEffect(controlsOpacity);
    const auto alignControls = [&controls, &video] {
        controls.setGeometry(0, qMax(0, video.height() - 48), video.width(), 48);
        controls.raise();
    };
    shell.show();
    alignControls();
    player.start();
    QTRY_VERIFY_WITH_TIMEOUT(rendered.count() == 1, 10000);
    QVERIFY(player.setVolume(0));
    QElapsedTimer firstFrame; firstFrame.start();
    QVERIFY(player.loadFile(QDir(root).filePath(relativePath)));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() == 1, 10000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().at(2).toString()));
    QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.first().at(1).toString()));
    QImage initial;
    QTRY_VERIFY_WITH_TIMEOUT([&] {
      initial = video.grabFramebuffer();
      if (initial.isNull()) return false;
      const auto small = initial.scaled(32, 18);
      const auto first = small.pixel(0, 0);
      for (int y = 0; y < small.height(); ++y)
        for (int x = 0; x < small.width(); ++x)
          if (small.pixel(x, y) != first) return true;
      return false;
    }(), 10000);
    const auto directory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    if (!directory.isEmpty()) QVERIFY(initial.save(directory + '/' + QTest::currentDataTag() + "-initial.png"));
    QVERIFY2(hasIntactColorBar(initial), "Decoded color bar is corrupted in the framebuffer");
    QCoreApplication::processEvents();
    const auto composite = shell.grab().toImage().copy(video.geometry());
    if (!directory.isEmpty()) QVERIFY(composite.save(directory + '/' + QTest::currentDataTag() + "-composite.png"));
    QVERIFY2(hasIntactColorBar(composite), "Window compositing dimmed the decoded color bar");
    qInfo("Visible first frame: %lld ms", firstFrame.elapsed());
    QTRY_VERIFY(!decoders.isEmpty() && !decoders.last().first().toString().isEmpty());
    qInfo().noquote() << "Active decoder:" << decoders.last().first().toString();
    if (softwareDecoding) QCOMPARE(decoders.last().first().toString(), QString("no"));
    QVERIFY2(firstFrame.elapsed() < (relativePath.contains("HEVC") ? 3000 : 2000), "First-frame budget exceeded");
    // Keep the two-second corpus paused. A slow test runner must not reach EOF
    // before screenshot capture. MainPlaybackTest covers continuous playback.
    const auto stepId = player.frameStep(); QVERIFY(stepId);
    QTRY_VERIFY_WITH_TIMEOUT(hasReply(commands, stepId) || hasReply(errors, stepId), 5000);
    QVERIFY2(hasReply(commands, stepId), "Frame step failed; see Player command failure above");
    QTRY_VERIFY_WITH_TIMEOUT(video.grabFramebuffer() != initial, 5000);
    const auto pauseId = player.pause(); QVERIFY(pauseId);
    QTRY_VERIFY_WITH_TIMEOUT(hasReply(commands, pauseId) || hasReply(errors, pauseId), 5000);
    QVERIFY2(hasReply(commands, pauseId), "Pause failed; see Player command failure above");
    shell.resize(800, 450);
    QTest::qWait(150);
    alignControls();
    QVERIFY(!video.grabFramebuffer().isNull());
    const auto screenshotPath = output.path() + "/frame.png";
    qInfo() << "Screenshot requested at position:"
            << (positions.isEmpty() ? -1 : positions.last().first().toLongLong()) << "ms";
    QVERIFY2(hasIntactColorBar(video.grabFramebuffer()), "Resize corrupted the decoded color bar");
    QCoreApplication::processEvents();
    QVERIFY2(hasIntactColorBar(shell.grab().toImage().copy(video.geometry())),
             "Window compositing dimmed the decoded color bar after resize");
    if (!directory.isEmpty()) QVERIFY(video.grabFramebuffer().save(directory + '/' + QTest::currentDataTag() + ".png"));
    const auto screenshotId = player.screenshot(screenshotPath); QVERIFY(screenshotId);
    QTRY_VERIFY_WITH_TIMEOUT(hasReply(commands, screenshotId) || hasReply(errors, screenshotId), 5000);
    QVERIFY2(hasReply(commands, screenshotId), "Screenshot failed; see Player command failure above");
    QVERIFY(!QImage(screenshotPath).isNull());
    if (relativePath.contains("H264")) {
      QSignalSpy tracks(&player, &melearner::Player::tracksChanged);
      QVERIFY(player.addSubtitleFile(root + "/01 H264 AAC.en.srt"));
      QVERIFY(player.addSubtitleFile(root + "/01 H264 AAC.ja.vtt"));
      QTRY_VERIFY_WITH_TIMEOUT([&] {
        if (tracks.isEmpty()) return false;
        int external = 0;
        for (const auto& track : qvariant_cast<QVector<melearner::PlayerTrack>>(tracks.last().first()))
          external += track.external && track.type == "sub";
        return external == 2;
      }(), 5000);
    }
    QCOMPARE(QApplication::topLevelWidgets().size(), 1);
    // Shutdown must ask the attached widget to release its renderer before
    // joining libmpv; this intentionally exercises the reverse cleanup order.
    player.shutdown();
    QVERIFY2(renderErrors.isEmpty(), renderErrors.isEmpty()
        ? "" : qPrintable(renderErrors.first().at(1).toString()));
    QVERIFY(!video.isRenderContextReady());
  }
};
QTEST_MAIN(PlaybackRenderTest)
#include "playback_render_test.moc"
