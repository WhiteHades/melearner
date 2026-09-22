#include "main_window.hpp"
#include "search_dialog.hpp"
#include "pdf_view.hpp"
#include <QDir>
#include <QDockWidget>
#include <QAction>
#include <QFile>
#include <QLabel>
#include <QListView>
#include <QTreeView>
#include <QLineEdit>
#include <QPushButton>
#include <QPainter>
#include <QPdfWriter>
#include <QSpinBox>
#include <QTabWidget>
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
  void statsFollowCourseProgress_data() {
    QTest::addColumn<int>("fontScale");
    QTest::newRow("normal-text") << 1;
    QTest::newRow("double-text") << 2;
  }
  void statsFollowCourseProgress() {
    QFETCH(int, fontScale);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto font = originalFont; font.setPointSizeF(font.pointSizeF() * fontScale); QApplication::setFont(font);
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Reading/Section"));
    QFile lesson(root + "/Reading/Section/Read.txt");
    QVERIFY(lesson.open(QIODevice::WriteOnly)); lesson.write("Read a local lesson."); lesson.close();
    MainWindow window(files.path() + "/library.sqlite3"); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled()); window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses"); QTRY_COMPARE(courses->model()->rowCount(), 1);
    auto* tabs = window.findChild<QTabWidget*>("libraryTabs"); QVERIFY(tabs); tabs->setCurrentIndex(1);
    auto* count = window.findChild<QLabel*>("coursesValue"); QTRY_COMPARE(count->text(), QString("1 / 1"));
    auto* completion = window.findChild<QLabel*>("completionValue"); QTRY_COMPARE(completion->text(), QString("0%"));
    auto* activity = window.findChild<QTableWidget*>("activityGrid"); QTRY_VERIFY(activity->item(6, 11));
    for (int width : {560, 768, 1280}) {
      window.resize(width, 720); QCoreApplication::processEvents();
      auto* scroll = window.findChild<QScrollArea*>("statsScroll");
      QTRY_COMPARE(scroll->horizontalScrollBar()->maximum(), 0);
      const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/stats-%1-%2x.png").arg(width).arg(fontScale)));
      scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
      QCoreApplication::processEvents();
      if (!captures.isEmpty()) QVERIFY(window.grab().save(captures + QString("/stats-activity-%1-%2x.png").arg(width).arg(fontScale)));
      scroll->verticalScrollBar()->setValue(0);
    }
    tabs->setCurrentIndex(0);
    courses->setCurrentIndex(courses->model()->index(0, 0)); QTest::keyClick(courses, Qt::Key_Return);
    auto* complete = window.findChild<QPushButton*>("markComplete"); QTRY_VERIFY(complete->isEnabled());
    QTest::mouseClick(complete, Qt::LeftButton); QTRY_COMPARE(complete->text(), QString("Mark incomplete"));
    QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
    tabs->setCurrentIndex(1); QTRY_COMPARE(completion->text(), QString("100%"));
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
    auto* page = window.findChild<QSpinBox*>("pdfPage"); QTRY_COMPARE(page->maximum(), 3);
    page->setValue(3); QTRY_VERIFY(pdf->cachedTiles() > 0);
    QVERIFY(window.findChild<QPushButton*>("openDocumentExternally")->isVisible());
    QVERIFY(!window.findChild<QPushButton*>("playPause")->isVisible());
    window.resize(1600, 780); QCoreApplication::processEvents();
    auto* notes = window.findChild<QDockWidget*>("lessonNotesDock"); QVERIFY(notes);
    QVERIFY(!notes->isVisible());
    auto* notesButton = window.findChild<QPushButton*>("lessonNotes"); QVERIFY(notesButton->isVisible());
    QTest::mouseClick(notesButton, Qt::LeftButton); QTRY_VERIFY(notes->isVisible());
    QVERIFY(notesButton->isChecked());
    QTest::mouseClick(notesButton, Qt::LeftButton); QTRY_VERIFY(!notes->isVisible());
    for (const int width : {560, 768, 1280, 1920}) {
      window.resize(width, 720); QTest::qWait(30); QVERIFY(window.width() <= width);
      QTRY_COMPARE(page->value(), 3);
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
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto font = originalFont; font.setPointSizeF(font.pointSizeF() * fontScale); QApplication::setFont(font);
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
    auto* choose = window.findChild<QPushButton*>("chooseRoot");
    QVERIFY(choose);
    QTRY_VERIFY(choose->isEnabled());
    window.chooseRoot(files.path() + "/Courses");
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_VERIFY2_WITH_TIMEOUT(courses->model()->rowCount() == 1,
      qPrintable(window.findChild<QLabel*>("appStatus")->text()), 15000);
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
    for (const int width : {560, 768, 1280, 1920}) {
      window.resize(width, 720);
      QCoreApplication::processEvents();
      if (!lessons->isVisible()) QTest::mouseClick(window.findChild<QPushButton*>("toggleOutline"), Qt::LeftButton);
      QVERIFY(window.width() <= width);
      QVERIFY(lessons->isVisible());
      QVERIFY(lessons->width() >= 200);
      if (window.findChild<QPushButton*>("toggleOutline")->isVisible())
        QTRY_VERIFY(lessons->width() >= window.width() - 80);
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
