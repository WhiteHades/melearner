#include "mpv_video_widget.hpp"
#include "player.hpp"
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

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
    melearner::MpvVideoWidget video(&player);
    QSignalSpy rendered(&video, &melearner::MpvVideoWidget::renderContextReady);
    QSignalSpy loaded(&player, &melearner::Player::fileLoaded);
    QSignalSpy errors(&player, &melearner::Player::commandFailed);
    QSignalSpy fatal(&player, &melearner::Player::fatalError);
    QSignalSpy decoders(&player, &melearner::Player::decoderChanged);
    connect(&player, &melearner::Player::commandFailed, this,
      [](melearner::Player::RequestId id, const QString& code, const QString& message) {
        qWarning().noquote() << "Player command failed:" << id << code << message;
      });
    connect(&player, &melearner::Player::playbackEnded, this,
      [](const QString&, bool failed) { qInfo() << "Playback ended; failed:" << failed; });
    QSignalSpy positions(&player, &melearner::Player::positionChanged);
    player.setApprovedRoots({root, output.path()});
    video.resize(640, 360); video.show(); player.start();
    QTRY_VERIFY_WITH_TIMEOUT(rendered.count() == 1, 10000);
    QVERIFY(player.setVolume(0));
    QElapsedTimer firstFrame; firstFrame.start();
    QVERIFY(player.loadFile(QDir(root).filePath(relativePath)));
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() == 1, 10000);
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(errors.first().at(2).toString()));
    QVERIFY2(fatal.isEmpty(), fatal.isEmpty() ? "" : qPrintable(fatal.first().at(1).toString()));
    QVERIFY(player.play());
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
    qInfo("Visible first frame: %lld ms", firstFrame.elapsed());
    QTRY_VERIFY(!decoders.isEmpty() && !decoders.last().first().toString().isEmpty());
    qInfo().noquote() << "Active decoder:" << decoders.last().first().toString();
    if (softwareDecoding) QCOMPARE(decoders.last().first().toString(), QString("no"));
    QVERIFY2(firstFrame.elapsed() < (relativePath.contains("HEVC") ? 3000 : 2000), "First-frame budget exceeded");
    QTRY_VERIFY_WITH_TIMEOUT(video.grabFramebuffer() != initial, 5000);
    QVERIFY(player.pause());
    video.resize(800, 450);
    QTest::qWait(150);
    QVERIFY(!video.grabFramebuffer().isNull());
    const auto screenshotPath = output.path() + "/frame.png";
    QSignalSpy commands(&player, &melearner::Player::commandFinished);
    qInfo() << "Screenshot requested at position:"
            << (positions.isEmpty() ? -1 : positions.last().first().toLongLong()) << "ms";
    const auto directory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    if (!directory.isEmpty()) QVERIFY(video.grabFramebuffer().save(directory + '/' + QTest::currentDataTag() + ".png"));
    const auto screenshotId = player.screenshot(screenshotPath); QVERIFY(screenshotId);
    const auto hasReply = [screenshotId](const QSignalSpy& replies) {
      for (const auto& reply : replies) if (reply.first().toULongLong() == screenshotId) return true;
      return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(hasReply(commands) || hasReply(errors), 5000);
    QVERIFY2(hasReply(commands), "Screenshot failed; see Player command failure above");
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
    // The widget must release its renderer before shutdown joins libmpv.
    video.setPlayer(nullptr); player.shutdown();
  }
};
QTEST_MAIN(PlaybackRenderTest)
#include "playback_render_test.moc"
