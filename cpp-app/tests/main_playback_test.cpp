#include "main_window.hpp"
#include "player.hpp"
#include "mpv_video_widget.hpp"
#include "theme.hpp"
#include <QDir>
#include <QAccessible>
#include <shadcn/navigation.hpp>
#include <shadcn/widgets.hpp>
#include <QMenu>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
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

namespace {
bool hasValidVideoFrame(const QImage& image) {
  if (image.isNull()) return false;
  // Sample the top of the fixture's yellow bar. The moving diagonal crosses
  // its middle after seeking, and the widget can letterbox the 16:9 video.
  const auto videoSize = QSize(320, 180).scaled(image.size(), Qt::KeepAspectRatio);
  const QPoint offset((image.width() - videoSize.width()) / 2,
                      (image.height() - videoSize.height()) / 2);
  int yellow = 0, samples = 0;
  for (int y = offset.y() + videoSize.height() * 3 / 100; y < offset.y() + videoSize.height() * 7 / 100; ++y)
    for (int x = offset.x() + videoSize.width() * 38 / 100; x < offset.x() + videoSize.width() * 43 / 100; ++x) {
      const auto pixel = image.pixelColor(x, y);
      yellow += pixel.red() > 180 && pixel.green() > 180 && pixel.blue() < 100;
      ++samples;
    }
  return samples > 0 && yellow * 10 >= samples * 9;
}
}

class MainPlaybackTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); melearner::installTheme(true, 14); }
  void playsPausesAndRestoresPosition_data() {
    QTest::addColumn<int>("fontScale");
    QTest::addColumn<QString>("mediaFile");
    QTest::addColumn<int>("audioTracks");
    QTest::newRow("normal-text") << 1 << "Systems 日本語/01 H264 AAC.mp4" << 1;
    QTest::newRow("double-text") << 2 << "Systems 日本語/01 H264 AAC.mp4" << 1;
    QTest::newRow("hevc-main10") << 1 << "03 HEVC Main 10.mkv" << 0;
    QTest::newRow("multiple-audio-tracks") << 1 << "02 Multi audio chapters.mkv" << 2;
  }
  void playsPausesAndRestoresPosition() {
    QFETCH(int, fontScale);
    QFETCH(QString, mediaFile);
    QFETCH(int, audioTracks);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto scaledFont = originalFont;
    if (scaledFont.pixelSize() > 0) scaledFont.setPixelSize(originalFont.pixelSize() * fontScale);
    else scaledFont.setPointSizeF(originalFont.pointSizeF() * fontScale);
    QApplication::setFont(scaledFont);
    QTemporaryDir data;
    QVERIFY(data.isValid());
    const auto root = data.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Video course/Section"));
    QVERIFY(QFile::copy(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media/" + mediaFile,
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
      auto* rendererWidget = window.findChild<melearner::MpvVideoWidget*>(); QVERIFY(rendererWidget);
      rendererWidget->makeCurrent();
      QVERIFY(QOpenGLContext::currentContext() == rendererWidget->context());
      auto* gl = rendererWidget->context()->functions();
      qInfo("OpenGL: %s; %s", reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER)),
            reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)));
      rendererWidget->doneCurrent();
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
      QTRY_COMPARE(audio->actions().size(), audioTracks);
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
          // The controls must remain readable over bright footage. Sample a
          // blank padding pixel in the actual composed window, not a widget
          // palette or a standalone grab that could hide transparency.
          const auto backdrop = controls->mapTo(&window, QPoint(4, controls->height() - 4));
          QTRY_COMPARE(window.grab().toImage().pixelColor(backdrop).rgba(),
                       controls->palette().color(QPalette::Window).rgba());
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
        window.resize(std::max(1280, window.fontMetrics().height() * 40), 720);
        auto* outlinePane = window.findChild<QWidget*>("courseOutline");
        QVERIFY(outlinePane);
        // Qt applies the split layout on the next event pass after a resize.
        // Wait for the actual viewer geometry before capturing the wide page.
        QTRY_VERIFY(outlinePane->isVisible() && scroll->isVisible());
        QTRY_VERIFY(outlinePane->mapTo(&window, QPoint(outlinePane->width(), 0)).x() <=
                    scroll->mapTo(&window, QPoint()).x());
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
  void malformedVideoRecoversAndResumes() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto root = data.path() + "/Courses/Recovery course/Section";
    QVERIFY(QDir().mkpath(root));
    const auto media = QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media/";
    const auto brokenPath = root + "/01 Broken.mp4";
    const auto validPath = root + "/02 Valid.mp4";
    QVERIFY(QFile::copy(media + "corrupt-media.bin", brokenPath));
    // Keep the recovery flow away from EOF even on an instrumented software
    // renderer. Remux the checked-in clip without introducing another codec.
    QProcess extendVideo;
    extendVideo.start(QStringLiteral("ffmpeg"), {"-hide_banner", "-loglevel", "error",
      "-stream_loop", "7", "-i", media + "Systems 日本語/01 H264 AAC.mp4",
      "-c", "copy", "-t", "16", validPath});
    QVERIFY(extendVideo.waitForStarted());
    QVERIFY(extendVideo.waitForFinished(10000));
    QCOMPARE(extendVideo.exitStatus(), QProcess::NormalExit);
    QVERIFY2(extendVideo.exitCode() == 0, extendVideo.readAllStandardError().constData());
    const auto database = data.path() + "/library.sqlite3";
    qint64 savedPosition = 0;
    {
      MainWindow window(database, nullptr, true); window.show(); window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      auto* player = window.findChild<melearner::Player*>(); QVERIFY(player);
      QSignalSpy loaded(player, &melearner::Player::fileLoaded);
      QSignalSpy failures(player, &melearner::Player::fatalError);
      auto* choose = window.findChild<QPushButton*>("chooseRoot");
      QTRY_VERIFY_WITH_TIMEOUT(choose->isEnabled(), 5000);
      window.chooseRoot(data.path() + "/Courses");
      auto* courses = window.findChild<QListView*>("courses");
      QTRY_COMPARE_WITH_TIMEOUT(courses->model()->rowCount(), 1, 10000);
      courses->setCurrentIndex(courses->model()->index(0, 0)); QTest::keyClick(courses, Qt::Key_Return);
      auto* lessons = window.findChild<QTreeView*>("lessons"); QVERIFY(lessons);
      QTRY_COMPARE_WITH_TIMEOUT(lessons->model()->rowCount(), 1, 5000);
      const auto section = lessons->model()->index(0, 0);
      QTRY_COMPARE_WITH_TIMEOUT(lessons->model()->rowCount(section), 2, 10000);
      QModelIndex brokenIndex, validIndex;
      // The paged model exposes the total before its visible rows arrive.
      QTRY_VERIFY_WITH_TIMEOUT([&] {
        for (int row = 0; row < lessons->model()->rowCount(section); ++row) {
          const auto index = lessons->model()->index(row, 0, section);
          const auto title = lessons->model()->data(index, Qt::DisplayRole).toString();
          if (title.startsWith(QStringLiteral("01 Broken"))) brokenIndex = index;
          if (title.startsWith(QStringLiteral("02 Valid"))) validIndex = index;
        }
        return brokenIndex.isValid() && validIndex.isValid();
      }(), 10000);
      loaded.clear();
      lessons->setCurrentIndex(brokenIndex); QTest::keyClick(lessons, Qt::Key_Return);
      auto* status = window.findChild<QLabel*>("appStatus"); QVERIFY(status);
      QTRY_VERIFY2_WITH_TIMEOUT(!failures.isEmpty(),
        qPrintable(QString("Status: %1; lesson: %2; player ready: %3; renderer ready: %4; loaded: %5")
          .arg(status->text(), window.findChild<QLabel*>("lessonTitle")->text())
          .arg(player->isReady()).arg(window.findChild<melearner::MpvVideoWidget*>()->isRenderContextReady())
          .arg(loaded.count())), 10000);
      QTRY_COMPARE(status->text(), failures.last().at(1).toString());
      for (const auto& record : loaded)
        QVERIFY(!record.at(0).toString().endsWith(QStringLiteral("/01 Broken.mp4")));
      auto* play = window.findChild<QPushButton*>("playPause"); QVERIFY(play);
      auto* seek = window.findChild<QWidget*>("playbackPosition"); QVERIFY(seek);
      QVERIFY(!play->isEnabled()); QVERIFY(!seek->isEnabled());
      QCOMPARE(window.findChild<QLabel*>("lessonTitle")->text(), QString("01 Broken"));

      loaded.clear();
      lessons->setCurrentIndex(validIndex); QTest::keyClick(lessons, Qt::Key_Return);
      QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
      QVERIFY(loaded.first().at(0).toString().endsWith(QStringLiteral("/02 Valid.mp4")));
      QTRY_VERIFY_WITH_TIMEOUT(play->isEnabled(), 5000);
      auto* video = window.findChild<melearner::MpvVideoWidget*>(); QVERIFY(video);
      QTRY_VERIFY_WITH_TIMEOUT(video->isRenderContextReady(), 10000);
      QImage firstFrame;
      const bool firstReady = QTest::qWaitFor([&] {
        return hasValidVideoFrame(firstFrame = video->grabFramebuffer());
      }, 10000);
      const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captures.isEmpty()) QVERIFY(firstFrame.save(captures + "/recovery-first-video.png"));
      QVERIFY2(firstReady, "No intact frame after changing from a corrupt video to a valid video");
      QSignalSpy positions(player, &melearner::Player::positionChanged);
      QTest::mouseClick(play, Qt::LeftButton);
      QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Pause"), 5000);
      QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().first().toLongLong() >= 600, 5000);
      QTRY_VERIFY_WITH_TIMEOUT(video->grabFramebuffer() != firstFrame, 5000);
      QTest::mouseClick(play, Qt::LeftButton);
      QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Play"), 5000);
      savedPosition = positions.last().first().toLongLong();
      window.close();
    }
    {
      MainWindow window(database, nullptr, true); window.show(); window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      auto* resume = window.findChild<QPushButton*>("resumeLesson"); QVERIFY(resume);
      QTRY_VERIFY_WITH_TIMEOUT(resume->isVisible() && resume->isEnabled(), 10000);
      QCOMPARE(window.findChild<QLabel*>("resumeLessonTitle")->text(), QString("02 Valid"));
      auto* player = window.findChild<melearner::Player*>(); QVERIFY(player);
      QSignalSpy loaded(player, &melearner::Player::fileLoaded);
      QSignalSpy positions(player, &melearner::Player::positionChanged);
      QTest::mouseClick(resume, Qt::LeftButton);
      QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
      const auto restored = loaded.first().at(2).toLongLong();
      QVERIFY2(qAbs(restored - savedPosition) < 500,
               qPrintable(QString("Saved %1 ms, resumed at %2 ms").arg(savedPosition).arg(restored)));
      auto* play = window.findChild<QPushButton*>("playPause"); QVERIFY(play);
      auto* video = window.findChild<melearner::MpvVideoWidget*>(); QVERIFY(video);
      QImage restoredFrame;
      const bool frameReady = QTest::qWaitFor([&] {
        return hasValidVideoFrame(restoredFrame = video->grabFramebuffer());
      }, 10000);
      const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captures.isEmpty()) QVERIFY(restoredFrame.save(captures + "/recovered-video.png"));
      QVERIFY2(frameReady, "The resumed video has no intact decoded frame");
      QTRY_VERIFY_WITH_TIMEOUT(play->isEnabled(), 5000);
      QTest::mouseClick(play, Qt::LeftButton);
      QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Pause"), 5000);
      QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().first().toLongLong() > restored + 100, 5000);
      QTest::mouseClick(play, Qt::LeftButton);
      QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Play"), 5000);
      window.close();
    }
  }
};
QTEST_MAIN(MainPlaybackTest)
#include "main_playback_test.moc"
