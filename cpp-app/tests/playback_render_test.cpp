#include "mpv_video_widget.hpp"
#include "player.hpp"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGraphicsOpacityEffect>
#include <QImage>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>
#include <memory>

namespace {
// The generated corpus has a solid yellow bar here. Nonblank/changing-frame
// checks alone do not detect corrupted or nearly black frames. This region
// stays yellow throughout the corpus, above its moving diagonal.
bool hasIntactColorBar(const QImage& image) {
  if (image.isNull()) return false;
  int yellow = 0;
  int total = 0;
  for (int y = image.height() * 3 / 100; y < image.height() * 7 / 100; ++y)
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
  void playsWhileHidden_data() {
    QTest::addColumn<bool>("minimize");
    QTest::newRow("hidden-widget") << false;
    QTest::newRow("minimized-window") << true;
  }
  void playsWhileHidden() {
    QFETCH(bool, minimize);
    const auto root = QDir(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media").canonicalPath();
    melearner::Player player(nullptr, melearner::Player::DecodeMode::Software);
    player.setApprovedRoots({root});
    QWidget shell;
    auto* layout = new QVBoxLayout(&shell);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* video = new melearner::MpvVideoWidget(&player, &shell);
    layout->addWidget(video);
    shell.resize(640, 360); shell.show();
    QVERIFY(QTest::qWaitForWindowExposed(&shell));
    player.start();
    QTRY_VERIFY_WITH_TIMEOUT(player.isReady() && video->isRenderContextReady(), 10000);
    if (minimize) {
      shell.showMinimized();
      QTRY_VERIFY(shell.isMinimized());
    } else {
      video->hide();
      QVERIFY(!video->isVisible());
    }
    QSignalSpy loaded(&player, &melearner::Player::fileLoaded);
    QSignalSpy positions(&player, &melearner::Player::positionChanged);
    QSignalSpy fatal(&player, &melearner::Player::fatalError);
    QVERIFY(player.setRate(0.25));
    QVERIFY(player.loadFile(root + "/Systems 日本語/01 H264 AAC.mp4"));
    QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 5000);
    QVERIFY(player.play());
    // Readback would itself paint the hidden widget and conceal a stalled
    // render callback. Only observe the playback clock until revealing it.
    QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().first().toLongLong() >= 600, 5000);
    QVERIFY(player.pause());
    if (minimize) shell.showNormal(); else video->show();
    QVERIFY(QTest::qWaitForWindowExposed(&shell));
    QTRY_VERIFY_WITH_TIMEOUT(hasIntactColorBar(video->grabFramebuffer()), 5000);
    QVERIFY(fatal.isEmpty());
  }
  void clearsUnloadedSurfaceToBlack() {
    melearner::MpvVideoWidget video(nullptr);
    video.resize(640, 360);
    video.show();
    QVERIFY(QTest::qWaitForWindowExposed(&video));
    QImage frame;
    QTRY_VERIFY(!(frame = video.grabFramebuffer()).isNull());
    QCOMPARE(frame.pixelColor(frame.width() / 2, frame.height() / 2), QColor(Qt::black));
  }
  void repeatedlyLoadsAndClosesInEitherOrder() {
    const auto root = QDir(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media").canonicalPath();
    QVERIFY2(!root.isEmpty(), "Checked-in media corpus missing");
    const auto videoPath = root + "/Systems 日本語/01 H264 AAC.mp4";
    const auto topLevelCount = QApplication::topLevelWidgets().size();
    for (int cycle = 0; cycle < 4; ++cycle) {
      auto player = std::make_unique<melearner::Player>(nullptr, melearner::Player::DecodeMode::Software);
      player->setApprovedRoots({root});
      auto shell = std::make_unique<QWidget>();
      shell->resize(640, 360);
      auto* layout = new QVBoxLayout(shell.get());
      layout->setContentsMargins(0, 0, 0, 0);
      auto* video = new melearner::MpvVideoWidget(player.get(), shell.get());
      layout->addWidget(video);
      QSignalSpy rendered(video, &melearner::MpvVideoWidget::renderContextReady);
      QSignalSpy renderErrors(video, &melearner::MpvVideoWidget::renderError);
      QSignalSpy loaded(player.get(), &melearner::Player::fileLoaded);
      QSignalSpy fatal(player.get(), &melearner::Player::fatalError);
      shell->show();
      QVERIFY(QTest::qWaitForWindowExposed(shell.get()));
      player->start();
      QTRY_VERIFY_WITH_TIMEOUT(player->isReady() && video->isRenderContextReady(), 10000);
      QCOMPARE(rendered.count(), 1);
      QVERIFY(player->loadFile(videoPath));
      QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
      QImage frame;
      const bool frameReady = QTest::qWaitFor([&] {
        return hasIntactColorBar(frame = video->grabFramebuffer());
      }, 10000);
      const auto directory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!directory.isEmpty()) QVERIFY(frame.save(directory + QString("/cycle-%1.png").arg(cycle)));
      QVERIFY2(frameReady, qPrintable(QString("No intact frame in cleanup cycle %1").arg(cycle)));
      QVERIFY2(renderErrors.isEmpty(), renderErrors.isEmpty()
          ? "" : qPrintable(renderErrors.first().at(1).toString()));
      QVERIFY(fatal.isEmpty());

      if (cycle % 2 == 0) {
        // First release the widget and its GL renderer, then stop the player.
        shell.reset();
        QVERIFY(!player->hasRenderContext());
        player->shutdown();
      } else {
        // Also exercise Player's synchronous aboutToShutdown detach while the
        // widget and its GL context are still alive.
        player->shutdown();
        QVERIFY(!video->isRenderContextReady());
        QVERIFY(!player->hasRenderContext());
        shell.reset();
      }
      player.reset();
      QCOMPARE(QApplication::topLevelWidgets().size(), topLevelCount);
    }
  }
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
    QTRY_VERIFY_WITH_TIMEOUT(rendered.count() == 1 || !fatal.isEmpty() || !renderErrors.isEmpty(), 10000);
    QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.first().at(1).toString()));
    QVERIFY2(renderErrors.isEmpty(), renderErrors.isEmpty() ? "" : qPrintable(renderErrors.first().at(1).toString()));
    QCOMPARE(rendered.count(), 1);
    video.makeCurrent();
    QVERIFY(QOpenGLContext::currentContext() == video.context());
    auto* gl = video.context()->functions();
    qInfo("OpenGL: %s; %s", reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER)),
          reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)));
    video.doneCurrent();
    QVERIFY(player.setVolume(0));
    QElapsedTimer firstFrame; firstFrame.start();
    QVERIFY(player.loadFile(QDir(root).filePath(relativePath)));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() == 1, 10000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().at(2).toString()));
    QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.first().at(1).toString()));
    QImage initial;
    // fileLoaded can precede the first presentation. Dithered near-black pixels
    // are not a decoded frame, so wait for the corpus's known solid color bar.
    const bool firstFrameReady = QTest::qWaitFor([&] {
      return hasIntactColorBar(initial = video.grabFramebuffer());
    }, 10000);
    const auto directory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    if (!directory.isEmpty()) QVERIFY(initial.save(directory + '/' + QTest::currentDataTag() + "-initial.png"));
    QVERIFY2(firstFrameReady, qPrintable(QString("No intact decoded frame after %1 ms; framebuffer %2x%3, render errors %4, fatal errors %5")
        .arg(firstFrame.elapsed()).arg(initial.width()).arg(initial.height())
        .arg(renderErrors.count()).arg(fatal.count())));
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
    QTRY_VERIFY2_WITH_TIMEOUT(hasIntactColorBar(video.grabFramebuffer()),
                             "Resize corrupted the decoded color bar", 5000);
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
