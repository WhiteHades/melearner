#include "main_window.hpp"
#include <QGraphicsEffect>
#include "player.hpp"
#include "mpv_video_widget.hpp"
#include "theme.hpp"
#include "course_rows.hpp"
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QFileInfo>
#include <QDir>
#include <QAccessible>
#include <QClipboard>
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
#include <QPointer>
#include <QVariantAnimation>
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
    qint64 thumbnailModified = 0;
    for (int launch = 0; launch < 2; ++launch) {
      MainWindow window(database, nullptr, true); window.show(); window.activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(&window));
      auto* choose = window.findChild<QPushButton*>("chooseRoot");
      QTRY_VERIFY_WITH_TIMEOUT(choose->isEnabled(), 5000);
      if (launch == 0) window.chooseRoot(root);
      auto* courses = window.findChild<QListView*>("courses");
      QTRY_VERIFY2_WITH_TIMEOUT(courses->model()->rowCount() == 1,
        qPrintable(window.findChild<QLabel*>("appStatus")->text()), 10000);
      const auto courseIndex = courses->model()->index(0, 0);
      QPixmap thumbnail;
      QTRY_VERIFY_WITH_TIMEOUT(!(thumbnail = courseIndex.data(melearner::CourseThumbnailRole).value<QPixmap>()).isNull(), 10000);
      QCOMPARE(thumbnail.size(), QSize(320, 180));
      QVERIFY(hasValidVideoFrame(thumbnail.toImage()));
      const auto hash = QCryptographicHash::hash(courseIndex.data(Qt::UserRole).toString().toUtf8(), QCryptographicHash::Sha256).toHex();
      const auto thumbnailPath = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath("course-thumbnails/" + QString::fromLatin1(hash) + ".png");
      QVERIFY(QFileInfo::exists(thumbnailPath));
      const auto modified = QFileInfo(thumbnailPath).lastModified().toMSecsSinceEpoch();
      if (launch == 0) thumbnailModified = modified;
      else QCOMPARE(modified, thumbnailModified); // Reopen uses the local image, not another decode.
      if (launch == 0 && fontScale == 1 && mediaFile.startsWith("Systems")) {
        const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
        window.findChild<QPushButton*>("cardsView")->click();
        QTRY_COMPARE(static_cast<melearner::CourseListView*>(courses)->presentation(), shadcn::ListPresentation::Cards);
        QTest::qWait(300);
        if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + "/photo-cards.png"));
        window.findChild<QPushButton*>("listView")->click();
        QTRY_COMPARE(static_cast<melearner::CourseListView*>(courses)->presentation(), shadcn::ListPresentation::List);
        QTest::qWait(100);
        if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + "/photo-list.png"));
      }
      auto* player = window.findChild<melearner::Player*>("lessonPlayer"); QVERIFY(player);
      QSignalSpy loaded(player, &melearner::Player::fileLoaded);
      QSignalSpy positions(player, &melearner::Player::positionChanged);
      QSignalSpy tracksChanged(player, &melearner::Player::tracksChanged);
      QSignalSpy ended(player, &melearner::Player::playbackEnded);
      QElapsedTimer courseOpen; courseOpen.start();
      courses->setCurrentIndex(courses->model()->index(0, 0)); QTest::keyClick(courses, Qt::Key_Return);
      auto* lessons = window.findChild<QTreeView*>("lessons");
      QTRY_COMPARE_WITH_TIMEOUT(lessons->model()->rowCount(), 1, 5000);
      QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
      qInfo("Course click to native file loaded: %lld ms", courseOpen.elapsed());
      auto* rendererWidget = window.findChild<melearner::MpvVideoWidget*>("videoSurface"); QVERIFY(rendererWidget);
      rendererWidget->makeCurrent();
      QVERIFY(QOpenGLContext::currentContext() == rendererWidget->context());
      auto* gl = rendererWidget->context()->functions();
      qInfo("OpenGL: %s; %s", reinterpret_cast<const char*>(gl->glGetString(GL_RENDERER)),
            reinterpret_cast<const char*>(gl->glGetString(GL_VERSION)));
      rendererWidget->doneCurrent();
      QVERIFY(player->setVolume(0));
      QSignalSpy volumes(player, &melearner::Player::volumeChanged);
      QSignalSpy muted(player, &melearner::Player::mutedChanged);
      auto* play = window.findChild<QPushButton*>("playPause");
      QTRY_VERIFY2(play->isEnabled(), qPrintable(window.findChild<QLabel*>("appStatus")->text()));
      QCOMPARE(play->text(), QString("Play"));
      if (mediaFile == QStringLiteral("Systems 日本語/01 H264 AAC.mp4")) {
        auto* surface = window.findChild<QWidget*>("videoSurface"); QVERIFY(surface);
        QSignalSpy clicks(rendererWidget, &melearner::MpvVideoWidget::clicked);
        QElapsedTimer clickLatency; clickLatency.start();
        const auto videoCenter = surface->rect().center();
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, videoCenter);
        QCOMPARE(clicks.count(), 1); // Dispatched on release, with no double-click wait.
        QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Pause"), 5000);
        qInfo("Video click to playback acknowledgement: %lld ms", clickLatency.elapsed());
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, videoCenter);
        QTRY_COMPARE_WITH_TIMEOUT(play->text(), QString("Play"), 5000);
      }
      auto* controls = window.findChild<QWidget*>("playerControls"); QVERIFY(controls);
      QVERIFY(!window.findChild<QPushButton*>("playbackOptions"));
      QVERIFY(!window.findChild<QMenu*>("videoSettings"));
      auto* speed = window.findChild<QMenu*>("playbackSpeed"); QVERIFY(speed);
      auto* timeline = window.findChild<shadcn::Slider*>("playbackPosition"); QVERIFY(timeline);
      QVERIFY(controls->isAncestorOf(timeline));
      for (const auto* name : {"volumeButton", "playbackRate", "subtitleTrackButton",
                               "frameStep", "addSubtitle", "screenshot", "fullscreen"}) {
        auto* directControl = window.findChild<QPushButton*>(name);
        QVERIFY2(directControl, name);
        QVERIFY2(controls->isAncestorOf(directControl), name);
      }
      QVERIFY(!window.findChild<QPushButton*>("rewind"));
      QVERIFY(!window.findChild<QPushButton*>("forward"));
      QVERIFY(!window.findChild<QPushButton*>("mute"));
      QVERIFY(!window.findChild<QPushButton*>("audioTrackButton"));
      QVERIFY(!window.findChild<QPushButton*>("chapterButton"));
      QVERIFY(controls->isAncestorOf(window.findChild<shadcn::Switch*>("autoplay")));
      auto* volumeButton = window.findChild<QPushButton*>("volumeButton"); QVERIFY(volumeButton);
      auto* volume = window.findChild<shadcn::Slider*>("volume"); QVERIFY(volume);
      QCOMPARE(volume->orientation(), Qt::Horizontal);
      QCOMPARE(volume->size(), QSize(92, 36));
      QVERIFY(controls->isAncestorOf(volume));
      QVERIFY(!window.findChild<QWidget*>("volumeMenu"));
      QTRY_COMPARE(volume->values().first(), 0.0);
      volume->setValues({100});
      QTRY_VERIFY_WITH_TIMEOUT(!volumes.isEmpty() && qAbs(volumes.last().first().toDouble() - 100.0) <= 1.0, 3000);
      QTRY_VERIFY(!tracksChanged.isEmpty());
      const auto loadedTracks = tracksChanged.last().first().value<QVector<melearner::PlayerTrack>>();
      QCOMPARE(std::count_if(loadedTracks.cbegin(), loadedTracks.cend(),
                             [](const auto& track) { return track.type == QStringLiteral("audio"); }), audioTracks);
      if (fontScale == 1 && mediaFile == QStringLiteral("Systems 日本語/01 H264 AAC.mp4")) {
        // The volume control is inline; its neighboring button toggles mute.
        volumeButton->click();
        QTRY_VERIFY(!muted.isEmpty() && muted.last().first().toBool());
        QTest::mouseClick(volume, Qt::LeftButton, Qt::NoModifier, volume->rect().center());
        QTRY_VERIFY_WITH_TIMEOUT(!volumes.isEmpty(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(volumes.last().first().toDouble() - 50.0) <= 1.0, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!muted.isEmpty() && !muted.last().first().toBool(), 3000);
        volume->setValues({0});
        QCOMPARE(volumeButton->accessibleName(), QString("Unmute"));
        QTRY_VERIFY_WITH_TIMEOUT(!muted.isEmpty() && muted.last().first().toBool(), 3000);
        volumeButton->click();
        QCOMPARE(volumeButton->accessibleName(), QString("Mute"));
        QVERIFY(volume->values().first() > 0);
        QTRY_VERIFY_WITH_TIMEOUT(!muted.isEmpty() && !muted.last().first().toBool(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(volume->values().first() > 0, 3000);
        // Rapid edits must retain the last choice while the worker catches up.
        for (const double value : {0.0, 75.0, 0.0, 75.0}) volume->setValues({value});
        QCOMPARE(volume->values().first(), 75.0);
        QCOMPARE(volumeButton->accessibleName(), QString("Mute"));
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(volumes.last().first().toDouble() - 75.0) < 0.01, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(!muted.last().first().toBool(), 3000);
        QCOMPARE(volume->values().first(), 75.0);
        volumeButton->click(); volumeButton->click();
        QCOMPARE(volumeButton->accessibleName(), QString("Mute"));
        QTRY_VERIFY_WITH_TIMEOUT(!muted.last().first().toBool(), 3000);
      }
      auto* screenshot = window.findChild<QPushButton*>("screenshot"); QVERIFY(screenshot);
      screenshot->click();
      QTRY_VERIFY(!QApplication::clipboard()->image().isNull());
      QCOMPARE(QApplication::clipboard()->image().size(), rendererWidget->grabFramebuffer().size());
      auto* accessibleVideo = QAccessible::queryAccessibleInterface(window.findChild<melearner::MpvVideoWidget*>("videoSurface"));
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
        QElapsedTimer pointerBatch;
        pointerBatch.start();
        for (int sample = 0; sample < 1000; ++sample) {
          const QPoint point(10 + sample % 100, 10 + sample % 50);
          QMouseEvent move(QEvent::MouseMove, QPointF(point),
                           QPointF(surface->mapToGlobal(point)), Qt::NoButton,
                           Qt::NoButton, Qt::NoModifier);
          QApplication::sendEvent(surface, &move);
        }
        qInfo("Pointer motion batch: 1000 events in %lld us", pointerBatch.nsecsElapsed() / 1000);
        QVERIFY(controls->isVisible());
        auto* hideControls = window.findChild<QTimer*>("hidePlayerControls"); QVERIFY(hideControls);
        QCOMPARE(hideControls->interval(), 4000);
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
        auto* reveal = window.findChild<QVariantAnimation*>("transportReveal"); QVERIFY(reveal);
        if (!melearner::reducedMotion() && !melearner::highContrast()) {
          QCOMPARE(reveal->duration(), 200);
          QCOMPARE(reveal->state(), QAbstractAnimation::Running);
          auto* effect = qobject_cast<QGraphicsBlurEffect*>(controls->graphicsEffect()); QVERIFY(effect);
          QVERIFY(effect->blurRadius() > 0);
          reveal->setCurrentTime(25);
          QVERIFY(effect->property("opacity").toDouble() > 0 && effect->property("opacity").toDouble() < 1);
          if (!qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS").isEmpty())
            QVERIFY(window.grab().save(qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS") + "/transport-revealing.png"));
        }
        QTRY_COMPARE(controls->graphicsEffect()->property("opacity").toDouble(), 1.0);
        QCOMPARE(hideControls->interval(), 4000);
        surface->setFocus(Qt::TabFocusReason);
        QTRY_COMPARE(QApplication::focusWidget(), surface);
        QTest::keyClick(surface, Qt::Key_Tab); QTRY_VERIFY(controls->isAncestorOf(QApplication::focusWidget()));
        QVERIFY(QMetaObject::invokeMethod(hideControls, "timeout", Qt::DirectConnection));
        QVERIFY(controls->isVisible());
        // Every frequently used operation is now a visible shadcn button. The
        // only popups are focused choices such as playback speed.
        phase = "player controls";
        auto* speedButton = window.findChild<QPushButton*>("playbackRate"); QVERIFY(speedButton);
        QVERIFY(speedButton->menu() == speed);
        QVERIFY(controls->isAncestorOf(volumeButton));
        surface->setFocus();
        phase = "resize";
        window.resize(560, 720); QCoreApplication::processEvents();
        auto* outline = window.findChild<QPushButton*>("toggleOutline");
        phase = "hidden playback";
        QTest::mouseClick(outline, Qt::LeftButton); QVERIFY(!surface->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!positions.isEmpty() && positions.last().at(0).toLongLong() >= 1100, 5000);
        QCOMPARE(ended.count(), 0);
        QTest::mouseClick(outline, Qt::LeftButton); QTRY_VERIFY(surface->isVisible());
        phase = "frame capture";
        auto* video = window.findChild<melearner::MpvVideoWidget*>("videoSurface");
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
          QTRY_COMPARE(timeline->width(), controls->width() - 40);
          if (width == 1280) {
            QVERIFY(controls->width() <= 820 * fontScale);
            QVERIFY(qAbs(controls->geometry().center().x() - surface->rect().center().x()) <= 1);
            QVERIFY(surface->width() <= 1600);
            const auto* fullscreen = window.findChild<QPushButton*>("fullscreen");
            const auto* capture = window.findChild<QPushButton*>("screenshot");
            QVERIFY(fullscreen->x() > capture->x());
            QCOMPARE(fullscreen->geometry().center().y(), play->geometry().center().y());
            QVERIFY(fullscreen->height() >= 36);
            QVERIFY(!surface->mask().isEmpty());
            QVERIFY(!surface->mask().contains(QPoint(0, 0)));
          }
          // The controls must remain readable over bright footage. Sample a
          // blank padding pixel in the actual composed window, not a widget
          // palette or a standalone grab that could hide transparency.
          const auto backdrop = controls->mapTo(&window,
            QPoint(controls->width() / 2, controls->height() - 5));
          QTRY_COMPARE(window.grab().toImage().pixelColor(backdrop).rgba(),
                       melearner::roleColor(shadcn::Role::Popover).rgba());
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
        QVERIFY(next->mapTo(&window, QPoint()).y() >= surface->mapTo(&window, QPoint(0, surface->height())).y());
        QSignalSpy rates(player, &melearner::Player::rateChanged);
        speed->actions().at(4)->trigger();
        QTRY_VERIFY(!rates.isEmpty()); QCOMPARE(rates.last().first().toDouble(), 1.5);
        const auto captureDirectory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
        movePointer(QPoint(20, 20));
        QTRY_COMPARE(controls->graphicsEffect()->property("opacity").toDouble(), 1.0);
        if (!captureDirectory.isEmpty()) QVERIFY(window.grab().save(captureDirectory + QString("/player-minimum-%1x.png").arg(fontScale)));
        window.resize(std::max(1280, window.fontMetrics().height() * 40), 720);
        auto* outlinePane = window.findChild<QWidget*>("courseOutline");
        QVERIFY(outlinePane);
        QTRY_VERIFY(outline->isVisible());
        if (!outlinePane->isVisible()) QTest::mouseClick(outline, Qt::LeftButton);
        QTRY_VERIFY(outlinePane->isVisible());
        QTest::mouseClick(outline, Qt::LeftButton);
        QTRY_VERIFY(!outlinePane->isVisible());
        QTRY_VERIFY(scroll->isVisible());
        QTest::mouseClick(outline, Qt::LeftButton);
        QTRY_VERIFY(outlinePane->isVisible());
        // Qt applies the split layout on the next event pass after a resize.
        // Wait for the actual viewer geometry before capturing the wide page.
        QTRY_VERIFY(outlinePane->isVisible() && scroll->isVisible());
        QTRY_COMPARE(outlinePane->graphicsEffect()->property("opacity").toDouble(), 1.0);
        QTRY_VERIFY(outlinePane->mapTo(&window, QPoint(outlinePane->width(), 0)).x() <=
                    scroll->mapTo(&window, QPoint()).x());
        window.resize(1920, 1080);
        QTRY_VERIFY(window.findChild<QWidget*>("mediaFrame")->height() > 500);
        QTRY_COMPARE(surface->height(), window.findChild<QWidget*>("mediaFrame")->height());
        QTRY_VERIFY(surface->mapTo(scroll->widget(), QPoint()).y() <= 1);
        QTRY_VERIFY(lessonTitle->mapTo(scroll->widget(), QPoint()).y() >= surface->height());
        QTRY_VERIFY(surface->height() <= 1600 * 9 / 16);
        if (!captureDirectory.isEmpty()) QVERIFY(window.grab().save(captureDirectory + QString("/player-wide-%1x.png").arg(fontScale)));
        window.resize(std::max(1280, window.fontMetrics().height() * 40), 720);
        if (fontScale == 1 && mediaFile == QStringLiteral("Systems 日本語/01 H264 AAC.mp4")) {
          auto* fullscreen = window.findChild<QPushButton*>("fullscreen"); QVERIFY(fullscreen);
          auto* header = window.findChild<QWidget*>("headerHost"); QVERIFY(header);
          auto* lessonHeader = window.findChild<QWidget*>("lessonHeader"); QVERIFY(lessonHeader);
          auto* actions = window.findChild<QWidget*>("lessonActions"); QVERIFY(actions);
          fullscreen->click(); QTRY_VERIFY(window.isFullScreen());
          QTRY_VERIFY(!header->isVisible() && !outlinePane->isVisible());
          QTRY_VERIFY(!lessonHeader->isVisible() && !actions->isVisible());
          QTRY_COMPARE(QRect(surface->mapTo(window.centralWidget(), QPoint()), surface->size()),
                       window.centralWidget()->rect());
          if (!captureDirectory.isEmpty()) QVERIFY(window.grab().save(captureDirectory + "/player-fullscreen.png"));
          fullscreen->click(); QTRY_VERIFY(!window.isFullScreen());
          QTRY_VERIFY(header->isVisible() && outlinePane->isVisible());
          QTRY_VERIFY(lessonHeader->isVisible() && actions->isVisible());
          QTRY_VERIFY(surface->height() <= window.height());
          QTRY_VERIFY(!window.findChild<QLabel*>("appStatus")->isVisible());
        }
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
  void autoplayAndDoubleClickSeeking() {
    QTemporaryDir data; QVERIFY(data.isValid());
    const auto folder = data.path() + "/Courses/Video course/Section";
    QVERIFY(QDir().mkpath(folder));
    const auto clip = QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/media/Systems 日本語/01 H264 AAC.mp4";
    QProcess extend;
    extend.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-stream_loop", "7", "-i", clip,
      "-c", "copy", "-t", "16", folder + "/01 First.mp4"});
    QVERIFY(extend.waitForFinished(10000)); QCOMPARE(extend.exitCode(), 0);
    QVERIFY(QFile::copy(QStringLiteral(MELEARNER_SOURCE_DIR) + "/fixtures/parity/documents/blank-500-pages.pdf", folder + "/02 Reading.pdf"));
    QVERIFY(QFile::copy(clip, folder + "/03 Next.mp4"));
    MainWindow window(data.path() + "/library.sqlite3", nullptr, true); window.show(); window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(data.path() + "/Courses");
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE_WITH_TIMEOUT(courses->model()->rowCount(), 1, 10000);
    QPointer<melearner::Player> previewPlayer;
    QTRY_VERIFY((previewPlayer = window.findChild<melearner::Player*>("previewPlayer")));
    QSignalSpy previewPositions(previewPlayer, &melearner::Player::positionChanged);
    QSignalSpy previewMuted(previewPlayer, &melearner::Player::mutedChanged);
    QSignalSpy previewPaused(previewPlayer, &melearner::Player::pausedChanged);
    QSignalSpy previewSaves(window.findChild<melearner::library::Library*>(), &melearner::library::Library::progressSaved);
    auto* previewVideo = window.findChild<melearner::MpvVideoWidget*>("previewVideo"); QVERIFY(previewVideo);
    const bool previewPlaying = QTest::qWaitFor([&] {
      return !previewPositions.empty() && previewPositions.last()[0].toLongLong() > 250;
    }, 10000);
    if (!previewPlaying && !qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS").isEmpty())
      window.grab().save(qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS") + "/preview-startup.png");
    QVERIFY2(previewPlaying,
      qPrintable(QString("Preview ready %1, renderer ready %2, visible %3, active %4, hint %5, size %6x%7, context %8")
        .arg(previewPlayer->isReady()).arg(previewVideo->isRenderContextReady())
        .arg(previewVideo->isVisible()).arg(window.isActiveWindow())
        .arg(window.findChild<QLabel*>("coursePreviewHint")->text())
        .arg(previewVideo->width()).arg(previewVideo->height()).arg(previewVideo->context() != nullptr)));
    QTRY_VERIFY(hasValidVideoFrame(previewVideo->grabFramebuffer()));
    auto* previewMute = window.findChild<QPushButton*>("previewMute"); QVERIFY(previewMute);
    QCOMPARE(previewMute->accessibleName(), QString("Unmute preview"));
    QVERIFY(!previewMuted.empty() && previewMuted.last()[0].toBool());
    previewMute->click(); QTRY_VERIFY(!previewMuted.last()[0].toBool());
    previewMute->click(); QTRY_VERIFY(previewMuted.last()[0].toBool());
    QCOMPARE(previewSaves.count(), 0);
    const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + "/dashboard-preview.png"));
    for (const auto height : {720, 400}) {
      window.resize(560, height); QTest::qWait(100);
      QCOMPARE(window.width(), 560); QCOMPARE(window.height(), height);
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/dashboard-560-%1.png").arg(height)));
    }
    window.resize(1200, 780); QTest::qWait(100);
    window.findChild<QPushButton*>("navStats")->click();
    QTRY_VERIFY(!previewPaused.empty() && previewPaused.last()[0].toBool());
    window.findChild<QPushButton*>("navStats")->click();
    QTRY_VERIFY(!previewPaused.last()[0].toBool());
    auto* player = window.findChild<melearner::Player*>("lessonPlayer");
    QSignalSpy loaded(player, &melearner::Player::fileLoaded);
    QSignalSpy positions(player, &melearner::Player::positionChanged);
    QSignalSpy ended(player, &melearner::Player::playbackEnded);
    courses->setCurrentIndex(courses->model()->index(0, 0)); QTest::keyClick(courses, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 1, 10000);
    QVERIFY(previewPlayer.isNull());
    auto* play = window.findChild<QPushButton*>("playPause");
    auto* autoplay = window.findChild<shadcn::Switch*>("autoplay");
    autoplay->setChecked(false);
    auto* surface = window.findChild<QWidget*>("videoSurface");
    auto* time = window.findChild<QLabel*>("playbackTime");
    auto* duration = window.findChild<QLabel*>("playbackDuration");
    auto* readout = window.findChild<QWidget*>("timeReadout");
    QVERIFY(time && duration && readout);
    QCOMPARE(time->parentWidget(), duration->parentWidget());
    QVERIFY(readout->isAncestorOf(time) && readout->isAncestorOf(duration));
    const auto clockWidth = time->width(); const auto durationWidth = duration->width();
    QVERIFY(!time->text().contains(' ') && !duration->text().contains(' '));
    QVERIFY(player->seek(7000)); QTRY_VERIFY(!positions.empty() && positions.last()[0].toLongLong() >= 6800);
    surface->setFocus();
    QTest::keyClick(surface, Qt::Key_Right);
    auto* feedback = window.findChild<QWidget*>("seekFeedback"); QVERIFY(feedback);
    QCOMPARE(feedback->property("deltaMs").toLongLong(), 3000);
    QVERIFY(feedback->isVisible());
    QTRY_VERIFY(positions.last()[0].toLongLong() >= 9800 && positions.last()[0].toLongLong() <= 10200);
    QTest::keyClick(surface, Qt::Key_Left);
    QCOMPARE(feedback->property("deltaMs").toLongLong(), -3000);
    QTRY_VERIFY(positions.last()[0].toLongLong() >= 6800 && positions.last()[0].toLongLong() <= 7200);
    if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + "/seek-feedback.png"));
    QTRY_VERIFY(!feedback->isVisible());
    QTest::keyClick(surface, Qt::Key_Space); QTRY_COMPARE(play->text(), QString("Pause"));
    QTest::keyClick(surface, Qt::Key_Space); QTRY_COMPARE(play->text(), QString("Play"));
    QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width() / 4, 20));
    QTest::mouseDClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width() / 4, 20));
    QTRY_VERIFY(positions.last()[0].toLongLong() < 2500);
    QCOMPARE(time->width(), clockWidth); QCOMPARE(duration->width(), durationWidth);
    QTest::qWait(QApplication::doubleClickInterval() + 30); QCOMPARE(play->text(), QString("Play"));
    QTest::mouseDClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width() * 3 / 4, 20));
    QTRY_VERIFY(positions.last()[0].toLongLong() >= 6800);
    QCOMPARE(time->width(), clockWidth);
    QCOMPARE(duration->width(), durationWidth);
    auto* next = window.findChild<QPushButton*>("nextLesson");
    QTRY_COMPARE(next->accessibleDescription(), QString("02 Reading"));
    autoplay->setChecked(true);
    QVERIFY(player->seek(15000)); play->click();
    QTRY_VERIFY_WITH_TIMEOUT(!ended.empty(), 5000);
    auto* indicator = window.findChild<QWidget*>("autoplayIndicator");
    QTRY_VERIFY_WITH_TIMEOUT(indicator->isVisible(), 5000);
    if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + "/autoplay-countdown.png"));
    QTest::mouseClick(window.findChild<QPushButton*>("cancelAutoplay"), Qt::LeftButton);
    QVERIFY(!indicator->isVisible());
    QVERIFY(!window.findChild<QTimer*>("autoplayCountdown")->isActive());
    QCOMPARE(loaded.size(), 1);
    auto* lessons = window.findChild<QTreeView*>("lessons");
    QTest::keyClick(lessons, Qt::Key_Return);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 2, 10000);
    QTRY_COMPARE(play->text(), QString("Play"));
    ended.clear(); QVERIFY(player->seek(15000)); play->click();
    QTRY_VERIFY_WITH_TIMEOUT(!ended.empty(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(indicator->isVisible(), 5000);
    QTRY_COMPARE_WITH_TIMEOUT(loaded.size(), 3, 8000);
    QVERIFY(loaded.last()[0].toString().endsWith("03 Next.mp4"));
    QTRY_COMPARE(play->text(), QString("Pause"));
    QVERIFY(!indicator->isVisible());
    autoplay->setChecked(false); window.close();
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
      auto* player = window.findChild<melearner::Player*>("lessonPlayer"); QVERIFY(player);
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
          .arg(player->isReady()).arg(window.findChild<melearner::MpvVideoWidget*>("videoSurface")->isRenderContextReady())
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
      auto* video = window.findChild<melearner::MpvVideoWidget*>("videoSurface"); QVERIFY(video);
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
      auto* player = window.findChild<melearner::Player*>("lessonPlayer"); QVERIFY(player);
      QSignalSpy loaded(player, &melearner::Player::fileLoaded);
      QSignalSpy positions(player, &melearner::Player::positionChanged);
      QTest::mouseClick(resume, Qt::LeftButton);
      QTRY_COMPARE_WITH_TIMEOUT(loaded.count(), 1, 10000);
      const auto restored = loaded.first().at(2).toLongLong();
      QVERIFY2(qAbs(restored - savedPosition) < 500,
               qPrintable(QString("Saved %1 ms, resumed at %2 ms").arg(savedPosition).arg(restored)));
      auto* play = window.findChild<QPushButton*>("playPause"); QVERIFY(play);
      auto* video = window.findChild<melearner::MpvVideoWidget*>("videoSurface"); QVERIFY(video);
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
