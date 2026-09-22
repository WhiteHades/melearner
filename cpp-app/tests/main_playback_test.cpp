#include "main_window.hpp"
#include "player.hpp"
#include "mpv_video_widget.hpp"
#include <QDir>
#include <QAccessible>
#include <QMenu>
#include <QSlider>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QListView>
#include <QTreeView>
#include <QPushButton>
#include <QSignalSpy>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>

class MainPlaybackTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); }
  void playsPausesAndRestoresPosition_data() {
    QTest::addColumn<int>("fontScale");
    QTest::newRow("normal-text") << 1;
    QTest::newRow("double-text") << 2;
  }
  void playsPausesAndRestoresPosition() {
    QFETCH(int, fontScale);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto scaledFont = originalFont; scaledFont.setPointSizeF(originalFont.pointSizeF() * fontScale); QApplication::setFont(scaledFont);
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const auto root = data.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Video course/Section"));
    QVERIFY(QFile::copy(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media/Systems 日本語/01 H264 AAC.mp4",
      root + "/Video course/Section/01 Video.mp4"));
    const auto database = data.path() + "/library.sqlite3";
    qint64 saved = 0;
    for (int launch = 0; launch < 2; ++launch) {
      MainWindow window(database, nullptr, true); window.show(); window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      auto* choose = window.findChild<QPushButton*>("chooseRoot");
      QTRY_VERIFY_WITH_TIMEOUT(choose->isEnabled(), 5000);
      if (launch == 0) window.chooseRoot(root);
      auto* courses = window.findChild<QListView*>("courses");
      QTRY_VERIFY2_WITH_TIMEOUT(courses->model()->rowCount() == 1,
        qPrintable(window.findChild<QLabel*>("appStatus")->text()), 10000);
      auto* player = window.findChild<melearner::Player*>(); QVERIFY(player);
      QSignalSpy loaded(player, &melearner::Player::fileLoaded);
      QSignalSpy positions(player, &melearner::Player::positionChanged);
      courses->setCurrentIndex(courses->model()->index(0, 0)); QTest::keyClick(courses, Qt::Key_Return);
      auto* lessons = window.findChild<QTreeView*>("lessons");
      QTRY_COMPARE_WITH_TIMEOUT(lessons->model()->rowCount(), 1, 5000);
      QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
      QVERIFY(player->setVolume(0));
      auto* play = window.findChild<QPushButton*>("playPause");
      QTRY_VERIFY2(play->isEnabled(), qPrintable(window.findChild<QLabel*>("appStatus")->text()));
      QCOMPARE(play->text(), QString("Play"));
      auto* controls = window.findChild<QWidget*>("playerControls"); QVERIFY(controls);
      auto* settings = window.findChild<QPushButton*>("playbackOptions"); QVERIFY(settings);
      auto* speed = window.findChild<QMenu*>("playbackSpeed"); QVERIFY(speed);
      auto* audio = window.findChild<QMenu*>("audioTrack"); QVERIFY(audio);
      QTRY_VERIFY(!audio->actions().isEmpty());
      QCOMPARE(settings->menu()->objectName(), QString("videoSettings"));
      QVERIFY(controls->isAncestorOf(settings));
      QVERIFY(controls->isAncestorOf(window.findChild<QSlider*>("playbackPosition")));
      auto* accessibleVideo = QAccessible::queryAccessibleInterface(window.findChild<melearner::MpvVideoWidget*>());
      QVERIFY(accessibleVideo); QCOMPARE(accessibleVideo->role(), QAccessible::Animation);
      QVERIFY(accessibleVideo->imageInterface());
      QCOMPARE(accessibleVideo->text(QAccessible::Name), QString("Video: 01 Video"));
      if (launch == 0) {
        QVERIFY(player->setRate(0.5));
        QElapsedTimer responsiveness; responsiveness.start();
        qint64 previousTick = 0; qint64 worstGap = 0; int ticks = 0;
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &window, [&] {
          const auto now = responsiveness.elapsed(); worstGap = std::max(worstGap, now - previousTick); previousTick = now; ++ticks;
        });
        heartbeat.start(10);
        auto* surface = window.findChild<QWidget*>("videoSurface"); QVERIFY(surface);
        QCOMPARE(controls->parentWidget(), surface);
        surface->setFocus(); QTest::keyClick(surface, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(0).toLongLong() >= 300, 5000);
        auto* hideControls = window.findChild<QTimer*>("hidePlayerControls"); QVERIFY(hideControls);
        QTest::mouseMove(surface, QPoint(10, 10));
        QTRY_COMPARE(play->text(), QString("Pause"));
        QCOMPARE(play->accessibleName(), play->text());
        QTest::qWait(50);
        QVERIFY(!controls->underMouse());
        QVERIFY(QMetaObject::invokeMethod(hideControls, "timeout", Qt::DirectConnection));
        QTRY_VERIFY(!controls->isVisible());
        QTest::mouseMove(surface, QPoint(20, 20)); QTRY_VERIFY(controls->isVisible());
        QTest::keyClick(surface, Qt::Key_Tab); QTRY_VERIFY(controls->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(QMetaObject::invokeMethod(hideControls, "timeout", Qt::DirectConnection));
        QVERIFY(controls->isVisible());
        // Settings stay inside the player and keep their native keyboard navigation.
        bool menuOpened = false;
        QTimer::singleShot(100, settings, [&] {
          menuOpened = settings->menu()->isVisible();
          QTest::keyClick(settings->menu(), Qt::Key_Escape);
        });
        settings->setFocus(); QTest::keyClick(settings, Qt::Key_Space);
        QTRY_VERIFY(menuOpened);
        QTRY_VERIFY(!settings->menu()->isVisible());
        surface->setFocus();
        window.resize(560, 720); QCoreApplication::processEvents();
        auto* outline = window.findChild<QPushButton*>("toggleOutline");
        QTest::mouseClick(outline, Qt::LeftButton); QVERIFY(!surface->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(0).toLongLong() >= 1100, 5000);
        QTest::mouseClick(outline, Qt::LeftButton); QVERIFY(surface->isVisible());
        auto* video = window.findChild<melearner::MpvVideoWidget*>();
        const auto previousFrame = video->grabFramebuffer();
        QTRY_VERIFY_WITH_TIMEOUT(video->grabFramebuffer() != previousFrame, 1000);
        QTest::mouseClick(play, Qt::LeftButton);
        QTRY_COMPARE(play->text(), QString("Play"));
        heartbeat.stop();
        qInfo("Playback GUI heartbeat: %d samples, longest gap %lld ms", ticks, worstGap);
        QVERIFY(ticks >= 20); QVERIFY2(worstGap < 150, "Playback stalled the GUI event loop");
        saved = positions.last().at(0).toLongLong();
        for (const int width : {560, 768, 1280}) {
          window.resize(width, 720); QCoreApplication::processEvents(); QVERIFY(window.width() <= width);
          QVERIFY(surface->rect().contains(controls->geometry()));
          const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
          if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/player-%1-%2x.png").arg(width).arg(fontScale)));
        }
        window.resize(560, 400); QCoreApplication::processEvents();
        QTest::qWait(100);
        QVERIFY2(window.width() <= 560 && window.height() <= 400, "Controls exceed the minimum supported window size");
        auto* scroll = window.findChild<QScrollArea*>("lessonScroll");
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        for (const auto* control : window.findChildren<QPushButton*>()) {
          if (control->isVisible() && !control->text().isEmpty())
            QVERIFY2(control->width() >= control->fontMetrics().horizontalAdvance(control->text()) + 12,
              qPrintable(QString("Clipped button: %1").arg(control->text())));
        }
        const auto* next = window.findChild<QPushButton*>("nextLesson");
        QVERIFY(surface->rect().contains(controls->geometry()));
        QVERIFY(controls->mapTo(&window, QPoint(0, controls->height())).y() <= next->mapTo(&window, QPoint()).y());
        QSignalSpy rates(player, &melearner::Player::rateChanged);
        speed->actions().at(4)->trigger();
        QTRY_VERIFY(!rates.isEmpty()); QCOMPARE(rates.last().first().toDouble(), 1.5);
        const auto captureDirectory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
        if (!captureDirectory.isEmpty()) QVERIFY(window.grab().save(captureDirectory + QString("/player-minimum-%1x.png").arg(fontScale)));
      } else {
        const auto restored = loaded.first().at(2).toLongLong();
        QVERIFY2(qAbs(restored - saved) < 500, qPrintable(QString("Saved %1 ms, restored %2 ms").arg(saved).arg(restored)));
      }
      window.close();
    }
  }
};
QTEST_MAIN(MainPlaybackTest)
#include "main_playback_test.moc"
