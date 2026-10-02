#include "main_window.hpp"
#include "player.hpp"
#include "mpv_video_widget.hpp"
#include <QDir>
#include <QAccessible>
#include <shadcn/navigation.hpp>
#include <shadcn/widgets.hpp>
#include <QMenu>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QListView>
#include <QTreeView>
#include <QPushButton>
#include <QProcess>
#include <QProcessEnvironment>
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
    QVERIFY(QFile::copy(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/documents/blank-500-pages.pdf",
      root + "/Video course/Section/02 Reading.pdf"));
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
      QSignalSpy ended(player, &melearner::Player::playbackEnded);
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
      // The transport is built from shadcn components, so the menus are
      // shadcn dropdown menus and the timeline is a shadcn slider.
      QVERIFY(qobject_cast<shadcn::DropdownMenu*>(window.findChild<QMenu*>("videoSettings")));
      auto* audio = window.findChild<QMenu*>("audioTrack"); QVERIFY(audio);
      QTRY_VERIFY(!audio->actions().isEmpty());
      QCOMPARE(settings->menu()->objectName(), QString("videoSettings"));
      QVERIFY(controls->isAncestorOf(settings));
      auto* timeline = window.findChild<shadcn::Slider*>("playbackPosition"); QVERIFY(timeline);
      QVERIFY(controls->isAncestorOf(timeline));
      auto* accessibleVideo = QAccessible::queryAccessibleInterface(window.findChild<melearner::MpvVideoWidget*>());
      QVERIFY(accessibleVideo); QCOMPARE(accessibleVideo->role(), QAccessible::Animation);
      QVERIFY(accessibleVideo->imageInterface());
      QCOMPARE(accessibleVideo->text(QAccessible::Name), QString("Video: 01 Video"));
      if (launch == 0) {
        QVERIFY(player->setRate(0.5));
        QElapsedTimer responsiveness;
        qint64 previousTick = 0; qint64 worstGap = 0; int ticks = 0;
        const char* phase = "start";
        const char* worstPhase = "start";
        QTimer heartbeat;
        connect(&heartbeat, &QTimer::timeout, &window, [&] {
          const auto now = responsiveness.elapsed();
          if (now - previousTick > worstGap) { worstGap = now - previousTick; worstPhase = phase; }
          previousTick = now; ++ticks;
        });
        auto* surface = window.findChild<QWidget*>("videoSurface"); QVERIFY(surface);
        const auto movePointer = [surface](const QPoint& point) {
          if (QGuiApplication::platformName().startsWith("wayland")) {
            // Wayland may prohibit cursor warping. Deliver the activity event
            // directly instead of waiting for QTest's unsupported warp.
            QMouseEvent move(QEvent::MouseMove, QPointF(point),
                             QPointF(surface->mapToGlobal(point)), Qt::NoButton,
                             Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(surface, &move);
          } else {
            QTest::mouseMove(surface, point);
          }
        };
        QCOMPARE(controls->parentWidget(), surface);
        surface->setFocus(); QTest::keyClick(surface, Qt::Key_Space);
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(0).toLongLong() >= 300, 5000);
        responsiveness.start(); heartbeat.start(10);
        QTest::qWait(600);
        qInfo("Steady playback GUI heartbeat: %d samples, longest gap %lld ms", ticks, worstGap);
        QVERIFY(ticks >= 20); QVERIFY2(worstGap < 150, "Playback stalled the GUI event loop");
        auto* hideControls = window.findChild<QTimer*>("hidePlayerControls"); QVERIFY(hideControls);
        phase = "pointer controls";
        movePointer(QPoint(10, 10));
        QTRY_COMPARE(play->text(), QString("Pause"));
        QCOMPARE(play->accessibleName(), play->text());
        QKeyEvent repeatedSpace(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, " ", true);
        QApplication::sendEvent(surface, &repeatedSpace);
        QTest::qWait(30); QCOMPARE(play->text(), QString("Pause"));
        QTest::qWait(50);
        QVERIFY(!controls->underMouse());
        QVERIFY(QMetaObject::invokeMethod(hideControls, "timeout", Qt::DirectConnection));
        QTRY_VERIFY(!controls->isVisible());
        movePointer(QPoint(20, 20)); QTRY_VERIFY(controls->isVisible());
        surface->setFocus(Qt::TabFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), surface);
        QTest::keyClick(surface, Qt::Key_Tab); QTRY_VERIFY(controls->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(QMetaObject::invokeMethod(hideControls, "timeout", Qt::DirectConnection));
        QVERIFY(controls->isVisible());
        // Settings stay inside the player and keep their native keyboard navigation.
        phase = "settings";
        bool menuOpened = false;
        connect(settings->menu(), &QMenu::aboutToShow, settings, [&] {
          QTimer::singleShot(100, settings, [&] {
            menuOpened = settings->menu()->isVisible();
            QTest::keyClick(settings->menu(), Qt::Key_Escape);
          });
        }, Qt::SingleShotConnection);
        settings->setFocus();
        QProcess compositorKey;
        const auto privateX11 = qEnvironmentVariable("MELEARNER_TEST_WAYLAND_X11_DISPLAY");
        const bool compositorInput = QGuiApplication::platformName().startsWith("wayland") && !privateX11.isEmpty();
        if (compositorInput) {
          // A nested Wayland compositor needs a real input serial for popup grabs.
          // Inject through its private X server without blocking the Qt event loop.
          auto environment = QProcessEnvironment::systemEnvironment();
          environment.insert("DISPLAY", privateX11);
          compositorKey.setProcessEnvironment(environment);
          compositorKey.start("xdotool", {"key", "--clearmodifiers", "--delay", "100", "space"});
          QVERIFY(compositorKey.waitForStarted());
        } else {
          QTest::keyClick(settings, Qt::Key_Space);
        }
        QTRY_VERIFY(menuOpened);
        QTRY_VERIFY(!settings->menu()->isVisible());
        if (compositorInput) {
          QTRY_COMPARE(compositorKey.state(), QProcess::NotRunning);
          QCOMPARE(compositorKey.exitCode(), 0);
        }
        surface->setFocus();
        phase = "resize";
        window.resize(560, 720); QCoreApplication::processEvents();
        auto* outline = window.findChild<QPushButton*>("toggleOutline");
        phase = "hidden playback";
        QTest::mouseClick(outline, Qt::LeftButton); QVERIFY(!surface->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(0).toLongLong() >= 1100, 5000);
        QCOMPARE(ended.count(), 0);
        QTest::mouseClick(outline, Qt::LeftButton); QVERIFY(surface->isVisible());
        phase = "frame capture";
        auto* video = window.findChild<melearner::MpvVideoWidget*>();
        const auto previousFrame = video->grabFramebuffer();
        QTRY_VERIFY_WITH_TIMEOUT(video->grabFramebuffer() != previousFrame, 1000);
        QTest::mouseClick(play, Qt::LeftButton);
        QTRY_COMPARE(play->text(), QString("Play"));
        heartbeat.stop();
        // Keep resize and synchronous framebuffer readback visible in the log,
        // but do not count test-driven GPU readback as steady playback latency.
        qInfo("Interaction GUI heartbeat: %d samples, longest gap %lld ms during %s", ticks, worstGap, worstPhase);
        saved = positions.last().at(0).toLongLong();

        // Switching through a document must not leave the video player in a
        // stale loading state. The saved video position should survive the
        // PDF transition, and the returned video must still play.
        const auto sectionIndex = lessons->model()->index(0, 0);
        QTRY_COMPARE_WITH_TIMEOUT(lessons->model()->rowCount(sectionIndex), 2, 10000);
        QModelIndex videoIndex;
        QModelIndex pdfIndex;
        for (int row = 0; row < lessons->model()->rowCount(sectionIndex); ++row) {
          const auto index = lessons->model()->index(row, 0, sectionIndex);
          const auto text = lessons->model()->data(index, Qt::DisplayRole).toString();
          if (text.startsWith(QStringLiteral("01 Video"))) videoIndex = index;
          if (text.startsWith(QStringLiteral("02 Reading"))) pdfIndex = index;
        }
        QVERIFY(videoIndex.isValid()); QVERIFY(pdfIndex.isValid());
        auto* lessonTitle = window.findChild<QLabel*>("lessonTitle"); QVERIFY(lessonTitle);
        lessons->setCurrentIndex(pdfIndex); QTest::keyClick(lessons, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(lessonTitle->text(), QString("02 Reading"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!play->isEnabled(), 5000);
        lessons->setCurrentIndex(videoIndex); loaded.clear(); positions.clear();
        QTest::keyClick(lessons, Qt::Key_Return);
        QTRY_COMPARE_WITH_TIMEOUT(lessonTitle->text(), QString("01 Video"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(loaded.count() == 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(play->isEnabled(), 5000);
        const auto transitioned = loaded.first().at(2).toLongLong();
        QVERIFY2(qAbs(transitioned - saved) < 500,
          qPrintable(QString("PDF transition changed saved position from %1 ms to %2 ms").arg(saved).arg(transitioned)));
        QTest::mouseClick(play, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Pause"), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(0).toLongLong() > transitioned + 100, 5000);
        QTest::mouseClick(play, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Play"), 5000);
        saved = positions.last().at(0).toLongLong();
        for (const int width : {560, 768, 1280}) {
          window.resize(width, 720); QCoreApplication::processEvents(); QVERIFY(window.width() <= width);
          QVERIFY(surface->rect().contains(controls->geometry()));
          QTRY_COMPARE(timeline->width(), controls->width() - 24);
          const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
          if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/player-%1-%2x.png").arg(width).arg(fontScale)));
        }
        window.resize(560, 400); QCoreApplication::processEvents();
        QTest::qWait(100);
        QVERIFY2(window.width() <= 560 && window.height() <= 400, "Controls exceed the minimum supported window size");
        auto* scroll = window.findChild<QScrollArea*>("lessonScroll");
        QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        // Icon buttons retain text for accessibility, but do not paint it.
        // Check their accessible name and hit area instead of that hidden label.
        for (const auto* control : window.findChildren<QPushButton*>()) {
          if (!control->isVisible() || control->text().isEmpty()) continue;
          if (const auto* button = qobject_cast<const shadcn::Button*>(control);
              button && shadcn::button_metrics(button->buttonSize()).iconOnly && !button->icon().isNull()) {
            auto* accessible = QAccessible::queryAccessibleInterface(const_cast<shadcn::Button*>(button));
            QVERIFY(accessible);
            QVERIFY(!accessible->text(QAccessible::Name).isEmpty());
            QVERIFY(button->width() >= 24 && button->height() >= 24);
            continue;
          }
          const auto label = control->fontMetrics().horizontalAdvance(control->text());
          QVERIFY2(control->sizeHint().width() >= label,
            qPrintable(QString("Clipped button: %1 needs %2, hint is %3")
                         .arg(control->text()).arg(label).arg(control->sizeHint().width())));
          QVERIFY2(control->width() >= label,
            qPrintable(QString("Narrow button: %1 needs %2, has %3")
                         .arg(control->text()).arg(label).arg(control->width())));
        }
        const auto* next = window.findChild<QPushButton*>("nextLesson");
        QVERIFY(surface->rect().contains(controls->geometry()));
        QVERIFY(controls->mapTo(&window, QPoint(0, controls->height())).y() <= next->mapTo(&window, QPoint()).y());
        QSignalSpy rates(player, &melearner::Player::rateChanged);
        speed->actions().at(4)->trigger();
        QTRY_VERIFY(!rates.isEmpty()); QCOMPARE(rates.last().first().toDouble(), 1.5);
        const auto captureDirectory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
        if (!captureDirectory.isEmpty()) QVERIFY(window.grab().save(captureDirectory + QString("/player-minimum-%1x.png").arg(fontScale)));
        window.resize(1280, 720);
        // The application stays in neutral dark across playback and menu use.
        auto* appearance = window.findChild<QPushButton*>("appearance")->menu();
        QVERIFY(appearance);
        for (const auto* action : appearance->actions()) QVERIFY(action->text() != "Light");
        const auto page = shadcn::Theme::neutral(shadcn::ColorMode::Dark).color(shadcn::Role::Background);
        const auto expected = QColor::fromRgbF(static_cast<float>(page.r),
                                               static_cast<float>(page.g),
                                               static_cast<float>(page.b));
        QTRY_COMPARE(QApplication::palette().color(QPalette::Window).name(QColor::HexRgb),
                    expected.name(QColor::HexRgb));
        if (!captureDirectory.isEmpty())
          QVERIFY(window.grab().save(captureDirectory + QString("/player-dark-%1x.png").arg(fontScale)));
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
