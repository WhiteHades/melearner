#include "main_window.hpp"
#include "search_dialog.hpp"
#include "pdf_view.hpp"
#include "study_icons.hpp"
#include <QDir>
#include <QDialog>
#include <QAction>
#include <QFile>
#include <QLabel>
#include <shadcn/rows.hpp>
#include <QListWidget>
#include <QPushButton>
#include <QPointer>
#include <QPainter>
#include <QPdfWriter>
#include <shadcn/widgets.hpp>
#include <shadcn/data.hpp>
#include <shadcn/overlays.hpp>
#include <shadcn/navigation.hpp>
#include <QTableWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QtTest>

class MainWindowTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); }
  void iconsHaveTransparentBackgroundsAtEveryScale() {
    const auto icon = melearner::studyIcon(melearner::StudyIcon::Courses, QColor("#a72c23"));
    for (const auto mode : {QIcon::Normal, QIcon::Disabled}) {
      for (const qreal scale : {1.0, 1.5, 2.0}) {
        const auto pixmap = icon.pixmap(QSize(24, 24), scale, mode);
        const auto pixels = pixmap.toImage();
        QCOMPARE(pixmap.devicePixelRatio(), scale);
        QCOMPARE(pixels.pixelColor(0, 0).alpha(), 0);
        QVERIFY(pixels.pixelColor(pixels.width() / 2, pixels.height() / 2).alpha() > 0);
      }
    }
  }
  void iconsStayDistinctFromEachOther() {
    // Qt keys its global icon cache on the engine's key. Every icon used to
    // return the same constant, so the first icon painted at a size was what
    // every later icon at that size returned.
    const auto red = QColor("#a72c23");
    const auto blue = QColor("#1d4ed8");
    const auto size = QSize(24, 24);
    const auto render = [&](melearner::StudyIcon icon, const QColor& color) {
      return melearner::studyIcon(icon, color).pixmap(size).toImage();
    };
    const auto play = render(melearner::StudyIcon::Play, red);
    const auto pause = render(melearner::StudyIcon::Pause, red);
    const auto search = render(melearner::StudyIcon::Search, red);
    const auto otherColour = render(melearner::StudyIcon::Play, blue);
    QVERIFY(play != pause);
    QVERIFY(play != search);
    QVERIFY(pause != search);
    QVERIFY(play != otherColour);
    // The same request twice returns the same pixels, whether it came from the
    // cache or was rebuilt.
    QCOMPARE(render(melearner::StudyIcon::Play, red), play);
    QCOMPARE(render(melearner::StudyIcon::Play, blue), otherColour);
  }
  void routesErrorsToTheirCurrentOwner() {
    QTemporaryDir files; QVERIFY(files.isValid());
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    auto* choose = window.findChild<QPushButton*>("chooseRoot"); QTRY_VERIFY(choose->isEnabled());
    auto* library = window.findChild<melearner::library::Library*>(); QVERIFY(library);
    auto* status = window.findChild<QLabel*>("appStatus"); const auto ready = status->text();
    emit library->failed(1'000'000, {melearner::library::ErrorCode::database, "Old panel request failed", {}});
    QCOMPARE(status->text(), ready);
    window.chooseRoot(files.path() + "/missing-root");
    QTRY_VERIFY(status->text().contains("not a safe directory"));
    QTRY_VERIFY(choose->isEnabled());
  }
  void keyboardPopupAndTextInputStayScoped() {
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Keyboard/Section"));
    QFile lesson(root + "/Keyboard/Section/Read.txt");
    QVERIFY(lesson.open(QIODevice::WriteOnly)); lesson.write("Keyboard lesson"); lesson.close();
    QVERIFY(QDir().mkpath(root + "/Second/Section"));
    QVERIFY(QFile::copy(lesson.fileName(), root + "/Second/Section/Read.txt"));
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 2);
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));

    QTest::keyClick(&window, Qt::Key_F1);
    QPointer<QDialog> shortcuts = window.findChild<QDialog*>("shortcutHelp"); QTRY_VERIFY(shortcuts && shortcuts->isVisible());
    // The popup is a shadcn dialog holding a shadcn command list.
    auto* command = shortcuts->findChild<shadcn::Command*>("keyboardPopupCommand"); QVERIFY(command);
    auto* filter = static_cast<QLineEdit*>(&command->search()); QVERIFY(filter);
    auto* rows = &command->list(); QVERIFY(rows);
    QVERIFY(!shortcuts->property("shadcnPanel").isNull() || rows->count() > 0);
    QVERIFY(rows->count() > 10); filter->setText("play"); QTRY_VERIFY(rows->count() > 0);
    shortcuts->reject(); QTRY_VERIFY(shortcuts.isNull());

    auto* input = new QLineEdit(&window); input->setObjectName("keyboardInputProbe"); input->show(); input->setFocus();
    QTest::keyClick(input, Qt::Key_J); QCOMPARE(input->text(), QString("j"));
    QTest::keyClick(input, Qt::Key_F1);
    shortcuts = window.findChild<QDialog*>("shortcutHelp"); QTRY_VERIFY(shortcuts && shortcuts->isVisible());
    shortcuts->reject(); QTRY_VERIFY(shortcuts.isNull());
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    courses->setFocus(); courses->setCurrentIndex(courses->model()->index(1, 0));
    QTRY_VERIFY(courses->hasFocus());
    QTest::keyClick(courses, Qt::Key_G);
    input->setFocus(); QTRY_VERIFY(input->hasFocus()); QTest::keyClick(input, Qt::Key_X);
    courses->setFocus(); QTRY_VERIFY(courses->hasFocus());
    QTest::keyClick(courses, Qt::Key_G); QCOMPARE(courses->currentIndex().row(), 1);
    QTest::keyClick(courses, Qt::Key_G); QCOMPARE(courses->currentIndex().row(), 0);
    input->clearFocus(); input->deleteLater(); QCoreApplication::processEvents();

    QTest::keyClick(&window, Qt::Key_Space, Qt::ControlModifier);
    QPointer<QDialog> palette = window.findChild<QDialog*>("commandPalette");
    QTRY_VERIFY(palette && palette->isVisible());
    auto* paletteCommand = palette->findChild<shadcn::Command*>("keyboardPopupCommand");
    QVERIFY(paletteCommand);
    filter = static_cast<QLineEdit*>(&paletteCommand->search());
    rows = &paletteCommand->list();
    // The command list searches the label, the binding and the context, so a
    // command is reachable by any of the three. It hides unmatched rows rather
    // than removing them, so the count is over the visible rows.
    const auto visibleRows = [rows] {
      int count = 0;
      for (int row = 0; row < rows->count(); ++row)
        if (!rows->item(row)->isHidden()) ++count;
      return count;
    };
    filter->setText("rescan root"); QTRY_VERIFY(visibleRows() == 1);
    filter->setText("last item"); QTRY_VERIFY(visibleRows() == 1);
    filter->setText("no command matches this");
    QTRY_VERIFY(visibleRows() == 0);
    filter->setText("last item"); QTRY_VERIFY(visibleRows() == 1);
    // Return runs the highlighted command, so the selection moves in the window
    // rather than only closing the palette.
    QTest::keyClick(filter, Qt::Key_Return);
    QTRY_COMPARE(courses->currentIndex().row(), 1);
    // The dialog deletes itself on close, so a null pointer is the closed state
    // as much as a hidden one.
    QTRY_VERIFY(palette.isNull() || !palette->isVisible());
  }
  void statsFollowCourseProgress_data() {
    QTest::addColumn<int>("fontScale");
    QTest::newRow("normal-text") << 1;
    QTest::newRow("double-text") << 2;
  }
  void statsFollowCourseProgress() {
    QFETCH(int, fontScale);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    // The shadcn install sets a pixel size, so the text scale has to be applied
    // to the pixel size or the doubled-text case would render the same as the
    // normal one.
    auto font = originalFont;
    if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * qreal(fontScale)));
    else font.setPointSizeF(font.pointSizeF() * fontScale);
    QApplication::setFont(font);
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Reading/Section"));
    QFile lesson(root + "/Reading/Section/Read.txt");
    QVERIFY(lesson.open(QIODevice::WriteOnly)); lesson.write("Read a local lesson."); lesson.close();
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses"); QTRY_COMPARE(courses->model()->rowCount(), 1);
    // The stats page is reached from the rail now, not from a row of tabs under the
    // header, so the test drives it the way a reader does.
    auto* statsNav = window.findChild<QPushButton*>("navStats"); QVERIFY(statsNav);
    QVERIFY(statsNav->isCheckable());
    QTest::mouseClick(statsNav, Qt::LeftButton);
    auto* tabs = window.findChild<shadcn::Tabs*>("libraryStack"); QVERIFY(tabs);
    QCOMPARE(tabs->currentValue(), QString("stats"));
    auto* count = window.findChild<QLabel*>("coursesValue"); QTRY_COMPARE(count->text(), QString("1 / 1"));
    auto* completion = window.findChild<QLabel*>("completionValue"); QTRY_COMPARE(completion->text(), QString("0%"));
    // The activity grid is one focus stop over whole week columns, so it holds
    // no cells for a library that has recorded no activity yet.
    auto* activity = window.findChild<shadcn::Heatmap*>("activityGrid"); QVERIFY(activity);
    QCOMPARE(activity->rowCount(), 7);
    QTRY_COMPARE(activity->days().size(), 0);
    QVERIFY(activity->accessibleName().contains(QStringLiteral("activity"), Qt::CaseInsensitive));
    auto* rail = window.findChild<shadcn::Sidebar*>();
    for (int width : {560, 768, 1280}) {
      window.resize(width, 720); QCoreApplication::processEvents();
      // The rail slides rather than jumps, so a capture taken straight after a resize
      // photographs it part way across and every element in it looks truncated. A
      // capture that is going to be looked at has to wait for the width the rail is
      // going to keep.
      auto* scroll = window.findChild<QScrollArea*>("statsScroll");
      QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
      const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/stats-%1-%2x.png").arg(width).arg(fontScale)));
      scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
      QCoreApplication::processEvents();
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/stats-activity-%1-%2x.png").arg(width).arg(fontScale)));
      scroll->verticalScrollBar()->setValue(0);
    }
    tabs->setCurrentValue("courses");
    courses->setCurrentIndex(courses->model()->index(0, 0)); QTest::keyClick(courses, Qt::Key_Return);
    auto* complete = window.findChild<QPushButton*>("markComplete"); QTRY_VERIFY(complete->isEnabled());
    QTest::mouseClick(complete, Qt::LeftButton); QTRY_COMPARE(complete->text(), QString("Mark incomplete"));
    QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
    tabs->setCurrentValue("stats"); QTRY_COMPARE(completion->text(), QString("100%"));
  }
  /// A lesson body is the library's prose surface. This opens a real document,
  /// checks the surface is the one the library owns rather than a stock text edit,
  /// and captures it at the widths a reader actually uses.
  void opensADocumentOnTheProseSurface() {
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Reading Course/Section"));
    // A heading, prose, a list, a quote, code, and text that looks like markup. The
    // last one is the case that matters: a lesson is content, and content that
    // looks like a tag has to arrive as the text it is.
    {
      QFile file(root + "/Reading Course/Section/Notes.md");
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("# Lesson one\n\nA paragraph of the lesson body.\n\n"
                 "- First point\n- Second point\n\n"
                 "> A quoted line.\n\n"
                 "`signal -> process -> response`\n\n"
                 "Text with <b>markup</b> and an ampersand & a < sign.\n");
    }
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<shadcn::ListView*>("courses"); QVERIFY(courses);
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    auto* resume = window.findChild<QPushButton*>("resumeLesson"); QVERIFY(resume);
    QTRY_VERIFY(resume->isVisible()); QTest::mouseClick(resume, Qt::LeftButton);
    auto* lessons = window.findChild<shadcn::TreeView*>("lessons"); QVERIFY(lessons);
    QTRY_COMPARE(lessons->model()->rowCount(), 1);

    // The surface is the library's. The prose surface is a text edit underneath, so
    // the claim is not that there is no text edit: it is that every text edit in the
    // window is the library's surface and none of them is a stock one the window
    // configured by hand.
    auto* prose = window.findChild<shadcn::Prose*>(); QVERIFY(prose);
    const auto edits = window.findChildren<QTextEdit*>();
    QVERIFY(!edits.isEmpty());
    for (auto* edit : edits) {
        QVERIFY2(qobject_cast<shadcn::Prose*>(edit) == prose,
                 "the window holds a text edit that is not the library's prose surface");
    }
    QTRY_VERIFY(prose->isVisible());
    QVERIFY2(prose->readerMode(), "the lesson body can be typed into");
    // The document's structure reached the surface, which is what a table of
    // contents or a progress indicator would be built from.
    QVERIFY(!prose->headings().isEmpty());
    QVERIFY(prose->headings().first().contains(QStringLiteral("Lesson one")));
    // No caret. A lesson body takes focus so it can be scrolled, and a blinking
    // caret in a thing nobody can type into is the clearest signal that a reader
    // has been handed a form.
    prose->setFocus();
    QCoreApplication::processEvents();
    QVERIFY2(prose->cursorWidth() == 0,
             qPrintable(QStringLiteral("the lesson body shows a caret of width %1")
                            .arg(prose->cursorWidth())));
    QVERIFY2(!prose->isReadOnly() == false, "the lesson body can be typed into");
    // A vertical scrollbar belongs on the trailing edge of the surface it scrolls.
    // On the leading edge it reads as a gutter with a stray mark in it, and the
    // reader has no reason to think the surface scrolls at all.
    if (prose->verticalScrollBar()->isVisible()) {
      const auto surface = prose->geometry();
      const auto bar = prose->verticalScrollBar()->geometry();
      QVERIFY2(bar.x() > surface.center().x(),
               qPrintable(QStringLiteral("the lesson body's scrollbar is at x=%1 on a surface "
                                         "spanning %2 to %3, so it is on the leading edge")
                              .arg(bar.x()).arg(surface.x()).arg(surface.x() + surface.width())));
    }

    const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    for (const int width : {768, 1280, 1920}) {
      window.resize(width, 780); QTest::qWait(30); QCoreApplication::processEvents();
      if (!captures.isEmpty()) {
        QVERIFY(window.grab().save(captures + QString("/document-%1.png").arg(width)));
      }
    }
  }

  /// A field that looks typeable and throws the text away is worse than a button.
  ///
  /// This is the regression this catches: the window's search field was connected to
  /// the search, but the search never read it, so a reader who typed a name and
  /// pressed Enter got an empty dialog and had to type it again.
  void typedSearchReachesTheSearch() {
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Finding Course/Section"));
    {
      QFile file(root + "/Finding Course/Section/One.md");
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("A local lesson.\n");
    }
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<shadcn::ListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);

    auto* field = window.findChild<shadcn::Input*>("searchButton"); QVERIFY(field);
    QVERIFY2(field->isEnabled(), "the search field cannot be typed into");

    // The reader types a name and presses Enter.
    field->setFocus();
    QTest::keyClicks(field, QStringLiteral("Finding"));
    QCOMPARE(field->text(), QString("Finding"));
    QTest::keyClick(field, Qt::Key_Return);
    QCoreApplication::processEvents();

    auto* dialog = window.findChild<SearchDialog*>();
    QVERIFY2(dialog, "the search did not open");
    QTRY_VERIFY(dialog->isVisible());
    // The text reached the search rather than being discarded on the way.
    auto* inner = dialog->findChild<shadcn::Input*>("searchQuery"); QVERIFY(inner);
    QCOMPARE(inner->text(), QString("Finding"));
    // And the field gave the text away, so it shows its placeholder again instead of
    // last query's text when the dialog closes.
    QVERIFY(field->text().isEmpty());
    // The results belong to the query, so the course is found.
    auto* results = dialog->findChild<shadcn::ListView*>("searchResults"); QVERIFY(results);
    QTRY_COMPARE(results->model()->rowCount(), 1);
  }

  /// One click opens a course. The handler used to sit on `activated`, which is the
  /// platform's idea of "chosen" and which some styles deliver on a double click,
  /// so on those desktops a reader had to click twice for the same result. A list of
  /// courses is browsed by clicking, so the click is the action.
  void oneClickOpensACourse() {
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/First Course/Section"));
    QVERIFY(QDir().mkpath(root + "/Second Course/Section"));
    for (const auto name : {"First Course", "Second Course"}) {
      QFile file(root + "/" + name + "/Section/One.md");
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("A local lesson.\n");
    }
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<shadcn::ListView*>("courses"); QVERIFY(courses);
    QTRY_COMPARE(courses->model()->rowCount(), 2);
    auto* lessons = window.findChild<shadcn::TreeView*>("lessons"); QVERIFY(lessons);
    QTRY_COMPARE(lessons->model()->rowCount(), 0);

    // One press and one release, which is a single click and not a double one.
    const auto row = courses->visualRect(courses->model()->index(0, 0));
    QVERIFY(row.isValid());
    // The press and the release are sent separately, with a gap longer than the
    // double-click interval between them, so this is one slow click and cannot be
    // read as the first half of a double click that something completed.
    QTest::mousePress(courses->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    QTest::qWait(500);
    QTest::mouseRelease(courses->viewport(), Qt::LeftButton, Qt::NoModifier, row.center());
    QTRY_COMPARE(lessons->model()->rowCount(), 1);
    QVERIFY(window.findChild<QPushButton*>("backToLibrary")->isVisible());
  }

  void opensPdfWithinCourse() {
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/PDF Course/Section"));
    {
      QPdfWriter writer(root + "/PDF Course/Section/Reading.pdf"); writer.setResolution(72);
      QPainter painter(&writer);
      for (int page = 1; page <= 3; ++page) {
        if (page > 1) QVERIFY(writer.newPage());
        painter.drawText(50, 50, QString("Local PDF lesson · page %1").arg(page));
      }
    }
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses"); QTRY_COMPARE(courses->model()->rowCount(), 1);
    auto* resume = window.findChild<QPushButton*>("resumeLesson"); QVERIFY(resume);
    QTRY_VERIFY(resume->isVisible()); QTest::mouseClick(resume, Qt::LeftButton);
    auto* lessons = window.findChild<QTreeView*>("lessons"); QTRY_COMPARE(lessons->model()->rowCount(), 1);
    auto* pdf = window.findChild<PdfView*>(); QVERIFY(pdf);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* page = window.findChild<shadcn::Input*>("pdfPage"); QVERIFY(page);
    page->setText("3"); QTest::keyClick(page, Qt::Key_Return); QTRY_VERIFY(pdf->cachedTiles() > 0);
    QVERIFY(window.findChild<QPushButton*>("openDocumentExternally")->isVisible());
    QVERIFY(!window.findChild<QPushButton*>("playPause")->isVisible());
    window.resize(1600, 780); QCoreApplication::processEvents();
    QVERIFY(!window.findChild<QWidget*>("lessonNotesDock"));
    QVERIFY(!window.findChild<QPushButton*>("lessonNotes"));
    QVERIFY(!window.findChild<QAction*>("keyboard-notes"));
    for (const int width : {560, 768, 1280, 1920}) {
      window.resize(width, 720); QTest::qWait(30); QVERIFY(window.width() <= width);
      QTRY_COMPARE(page->text(), QString("3"));
      const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/pdf-%1.png").arg(width)));
    }
    QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
    QVERIFY(courses->isVisible()); QCOMPARE(pdf->cachedTiles(), 0);
  }
  void rootToCourseAtSupportedWidths_data() {
    QTest::addColumn<int>("fontScale");
    QTest::newRow("normal-text") << 1;
    QTest::newRow("double-text") << 2;
  }
  void rootToCourseAtSupportedWidths() {
    QFETCH(int, fontScale);
    // The theme is installed by main(), which a test binary does not run, so the
    // test installs it. The window then inherits the shadcn style and its font
    // rather than setting them itself.
    shadcn::install(*qApp, shadcn::Theme::neutral(), shadcn::MotionPolicy::Reduced);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    // The shadcn install sets a pixel size, so the text scale has to be applied
    // to the pixel size or the doubled-text case would render the same as the
    // normal one.
    auto font = originalFont;
    if (font.pixelSize() > 0) font.setPixelSize(qRound(font.pixelSize() * qreal(fontScale)));
    else font.setPointSizeF(font.pointSizeF() * fontScale);
    QApplication::setFont(font);
    QTemporaryDir files;
    QVERIFY(QDir(files.path()).mkpath("Courses/A Course/01 Section"));
    QFile lesson(files.path() + "/Courses/A Course/01 Section/01 Introduction.txt");
    QVERIFY(lesson.open(QIODevice::WriteOnly));
    lesson.write("A local lesson."); lesson.close();
    for (int number = 2; number <= 320; ++number) {
      QFile item(files.path() + QString("/Courses/A Course/01 Section/%1 A lesson with a longer title.txt").arg(number, 3, 10, QChar('0')));
      QVERIFY(item.open(QIODevice::WriteOnly)); item.write("Another local lesson.");
    }
    QVERIFY(QDir(files.path()).mkpath("Courses/A Course/02 Wrapup"));
    QFile summary(files.path() + "/Courses/A Course/02 Wrapup/Summary.txt");
    QVERIFY(summary.open(QIODevice::WriteOnly)); summary.write("Course summary."); summary.close();
    MainWindow window(files.path() + "/library.sqlite3");
    window.show();
    QCOMPARE(window.font().family(), QString("Geist"));
    // The window never shrinks the user's text size.
    if (font.pixelSize() > 0) QVERIFY(window.font().pixelSize() >= font.pixelSize());
    else QVERIFY(window.font().pointSizeF() >= font.pointSizeF());
    auto* choose = window.findChild<QPushButton*>("chooseRoot");
    QVERIFY(choose);
    QTRY_VERIFY(choose->isEnabled());
    window.chooseRoot(files.path() + "/Courses");
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_VERIFY2_WITH_TIMEOUT(courses->model()->rowCount() == 1,
      qPrintable(window.findChild<QLabel*>("appStatus")->text()), 15000);
    QVERIFY(!window.findChild<QWidget*>("navigationRail"));
    auto* shortcuts = window.findChild<QPushButton*>("showShortcuts"); QVERIFY(shortcuts);
    // The search is a field, not a button: a reader who can see a field types into
    // it, and a button that opens a dialog is a step they did not ask for.
    auto* searchButton = window.findChild<shadcn::Input*>("searchButton"); QVERIFY(searchButton);
    // Settings acts on the whole application, so it is in the header beside the other
    // controls that do. There is no rail: a column of navigation for two destinations
    // is not minimal.
    auto* settings = window.findChild<QPushButton*>("appearance"); QVERIFY(settings);
    QVERIFY(!window.findChild<shadcn::Sidebar*>());
    QVERIFY(!shortcuts->icon().isNull());
    QTRY_VERIFY(window.findChild<shadcn::Progress*>("resumeProgress")->isVisible());
    QCOMPARE(window.findChild<shadcn::Progress*>("resumeProgress")->value(), 0);
    // Both colour modes are captured, because a component that does not follow the
    // theme looks like a different component in the mode nobody photographed.
    for (const auto* mode : {"light", "dark"}) {
      auto* action = window.findChild<QAction*>(QString("appearance-") + mode);
      QVERIFY2(action, "the colour mode has no named action, so it cannot be captured");
      action->trigger();
      QCoreApplication::processEvents();
      // The colour mode is stored through the Library, so the switch is a round trip
      // rather than immediate. Wait for it, or the capture is of the old mode.
      QTRY_VERIFY(qApp->style() && qobject_cast<const shadcn::Style*>(qApp->style())
                  && qobject_cast<const shadcn::Style*>(qApp->style())->theme().mode()
                      == (QString(mode) == "dark" ? shadcn::ColorMode::Dark
                                                  : shadcn::ColorMode::Light));
      {
        // No component may keep a colour from the other mode. A component that only
        // misbehaves on a mode change is invisible to a suite that only ever renders
        // one mode, which is how a light rail item sat on a dark page unnoticed.
        const auto* style = qobject_cast<const shadcn::Style*>(qApp->style());
        const auto theme = style->theme();
        const auto asColour = [&theme](shadcn::Role role) {
            const auto value = theme.color(role);
            return QColor::fromRgbF(static_cast<float>(value.r), static_cast<float>(value.g),
                                    static_cast<float>(value.b));
        };
        const auto isDark = theme.mode() == shadcn::ColorMode::Dark;
        const auto page = asColour(shadcn::Role::Background);
        const auto shot = window.grab().toImage();
        // The lightest thing on the page, and the most common colour, both have to
        // belong to this mode. A component wearing the other mode's accent is far
        // lighter than a dark page allows or far darker than a light one allows.
        auto lightest = 0;
        QHash<QRgb, int> tally;
        for (int y = 0; y < shot.height(); y += 3)
            for (int x = 0; x < shot.width(); x += 3) {
                const auto pixel = shot.pixelColor(x, y);
                lightest = std::max(lightest, pixel.lightness());
                ++tally[pixel.rgb()];
            }
        auto held = 0;
        QRgb common = 0;
        for (auto it = tally.constBegin(); it != tally.constEnd(); ++it)
            if (it.value() > held) { held = it.value(); common = it.key(); }
        const auto commonColour = QColor::fromRgb(common);
        const auto floor = isDark ? 150 : 40;
        const auto ceiling = isDark ? 255 : 245;
        QVERIFY2(commonColour.lightness() <= ceiling || !isDark,
                 qPrintable(QStringLiteral("in %1 mode the most common colour is %2 (%3), which is "
                                           "lighter than a %1 page should hold")
                                .arg(QString::fromLatin1(mode), commonColour.name())
                                .arg(commonColour.lightness())));
        // Which theme does the rail actually resolve, and is its own style the one
        // the application has? A widget that keeps a style object of its own shadows
        // the application style for every colour lookup, so a theme switch never
        // reaches it.
        if (auto* railWidget = window.findChild<shadcn::Sidebar*>()) {
            const auto* railStyle = qobject_cast<const shadcn::Style*>(railWidget->style());
            QVERIFY2(railStyle == style,
                     "the rail holds a style of its own, so the application theme never reaches it");
        }
        Q_UNUSED(floor);
        Q_UNUSED(lightest);
      }
      for (const int width : {560, 768, 1280, 1920}) {
        window.resize(width, 720); QTest::qWait(30);
          const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
        if (!captures.isEmpty()) {
          QVERIFY(window.grab().save(captures + QString("/library-%1-%2-%3x.png")
                                      .arg(mode).arg(width).arg(fontScale)));
        }
      }
    }
    auto* comfortableAction = window.findChild<QAction*>("appearance-light");
    if (comfortableAction) comfortableAction->trigger();
    QCoreApplication::processEvents();
    for (const int width : {560, 768, 1280, 1920}) {
      window.resize(width, 720); QTest::qWait(30);
      QVERIFY(window.width() <= width);
      QVERIFY(shortcuts->isVisible()); QVERIFY(searchButton->isVisible());
      // The rail is open where there is room for it and a page, and closed where
      // there is not, so the page never has to scroll sideways to show a table.
      // The threshold is the rail's own width plus the room a page needs, so the
      // test asks the window rather than repeating a number that would be wrong the
      QCOMPARE(settings->isVisible(), true);
      QCOMPARE(courses->horizontalScrollBar()->maximum(), 0);
      const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/library-%1-%2x.png").arg(width).arg(fontScale)));
    }
    const int comfortableHeight = courses->sizeHintForRow(0);
    auto* compact = window.findChild<QAction*>("presentation-compact"); QVERIFY(compact); compact->trigger();
    QTRY_VERIFY(courses->sizeHintForRow(0) < comfortableHeight);
    auto* comfortable = window.findChild<QAction*>("presentation-comfortable"); QVERIFY(comfortable); comfortable->trigger();
    QTRY_COMPARE(courses->sizeHintForRow(0), comfortableHeight);
    courses->setCurrentIndex(courses->model()->index(0, 0));
    QTest::keyClick(courses, Qt::Key_Return);
    auto* lessons = window.findChild<QTreeView*>("lessons");
    QTRY_COMPARE(lessons->model()->rowCount(), 2);
    const auto section = lessons->model()->index(0, 0);
    QTRY_COMPARE(lessons->model()->rowCount(section), 320);
    QTRY_VERIFY(lessons->isExpanded(section));
    QTRY_COMPARE(window.findChild<QTextEdit*>("documentText")->toPlainText(), QString("A local lesson."));
    QVERIFY(!searchButton->isVisible());
    for (const int width : {560, 768, 1280, 1920}) {
      window.resize(width, 720);
      QCoreApplication::processEvents();
      // Below the width the app switches to its compact layout and the outline sits
      // beside the content rather than taking its place.
      if (!lessons->isVisible()) QTest::mouseClick(window.findChild<QPushButton*>("toggleOutline"), Qt::LeftButton);
      QVERIFY(window.width() <= width);
      QVERIFY(lessons->isVisible());
      QVERIFY(lessons->width() >= 200);
      QVERIFY(shortcuts->isVisible()); QVERIFY(!searchButton->isVisible());
      if (window.findChild<QScrollArea*>("lessonScroll")->isVisible()) {
        const auto* outline = window.findChild<QWidget*>("courseOutline");
        const auto* viewer = window.findChild<QScrollArea*>("lessonScroll");
        QVERIFY(outline->mapTo(&window, QPoint(outline->width(), 0)).x() <= viewer->mapTo(&window, QPoint()).x());
      }
      if (window.findChild<QPushButton*>("toggleOutline")->isVisible()) {
        // The outline is offered where there is room for it beside the content, and
        // takes the page where there is not. Either way it fits inside the page: the
        // page is the window less the rail when the rail is beside it, and the whole
        // window when the rail is off-canvas.
        const auto page = window.width();
        // The outline never overflows the page, and the page is the window less the
        // rail when the rail is beside it and the whole window when the rail is
        // off-canvas. Whether the outline takes the page or sits beside the content
        // is the layout's decision and moves with the reader's text size, so it is
        // not asserted here: what is asserted is that the outline is inside its page
        // and, above the width where it is usable on its own, has the room to be.
        QVERIFY2(lessons->width() <= page,
                 qPrintable(QStringLiteral("the outline is %1 wide on a %2 pixel page")
                                .arg(lessons->width()).arg(page)));
        QTRY_VERIFY(lessons->width() >= 200);
      }
      const auto captureDirectory = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captureDirectory.isEmpty())
        QVERIFY(window.grab().save(captureDirectory + QString("/outline-%1-%2x.png").arg(width).arg(fontScale)));
    }
    const auto farLesson = lessons->model()->index(300, 0, section);
    lessons->scrollTo(farLesson);
    QTRY_VERIFY(!farLesson.data(Qt::UserRole).toString().isEmpty());
    lessons->setCurrentIndex(farLesson);
    QTest::keyClick(lessons, Qt::Key_Return);
    QVERIFY(window.findChild<QPushButton*>("markComplete")->isEnabled());
    auto* document = window.findChild<QTextEdit*>("documentText");
    QTRY_COMPARE(document->toPlainText(), QString("Another local lesson."));
    QVERIFY(document->isVisible());
    window.resize(560, 720);
    QTRY_VERIFY(!lessons->isVisible());
    QVERIFY(document->isVisible());
    auto* toggle = window.findChild<QPushButton*>("toggleOutline");
    QTest::mouseClick(toggle, Qt::LeftButton);
    QVERIFY(lessons->isVisible());
    QVERIFY(!document->isVisible());
    QTest::mouseClick(toggle, Qt::LeftButton);
    QVERIFY(document->isVisible());
    QCOMPARE(document->toPlainText(), QString("Another local lesson."));
    QTRY_COMPARE(window.findChild<QScrollArea*>("lessonScroll")->horizontalScrollBar()->maximum(), 0);
    const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
    if (!captures.isEmpty())
      QVERIFY(window.grab().save(captures + QString("/document-560-%1x.png").arg(fontScale)));
    const auto lastInSection = lessons->model()->index(319, 0, section);
    lessons->scrollTo(lastInSection); QTRY_VERIFY(!lastInSection.data(Qt::UserRole).toString().isEmpty());
    lessons->setCurrentIndex(lastInSection); QTest::keyClick(lessons, Qt::Key_Return);
    QTRY_VERIFY(window.findChild<QLabel*>("lessonTitle")->text().startsWith("320 "));
    QTest::mouseClick(window.findChild<QPushButton*>("nextLesson"), Qt::LeftButton);
    QTRY_COMPARE(document->toPlainText(), QString("Course summary."));
    QTRY_COMPARE(lessons->currentIndex().parent().row(), 1);
    QTest::mouseClick(window.findChild<QPushButton*>("previousLesson"), Qt::LeftButton);
    QTRY_VERIFY(window.findChild<QLabel*>("lessonTitle")->text().startsWith("320 "));
    QTRY_COMPARE(lessons->currentIndex().parent().row(), 0);
    window.activateWindow(); QVERIFY(QTest::qWaitForWindowActive(&window));
    QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
    QTRY_VERIFY(window.findChild<SearchDialog*>());
    auto* search = window.findChild<SearchDialog*>();
    auto* query = search->findChild<QLineEdit*>("searchQuery"); query->setText("Introduction");
    auto* results = search->findChild<QListView*>("searchResults"); QTRY_COMPARE(results->model()->rowCount(), 1);
    QTest::keyClick(query, Qt::Key_Return);
    QTRY_COMPARE(document->toPlainText(), QString("A local lesson."));
  }
};
QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
