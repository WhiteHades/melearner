#include "main_window.hpp"
#include "pdf_view.hpp"
#include "search_dialog.hpp"
#include "theme.hpp"

#include <QDir>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QPdfWriter>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTreeView>
#include <shadcn/widgets.hpp>
#include <QtTest>

class MainWindowTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); melearner::installTheme(true, 14); }

  void importSearchCompleteAndRestoreProgress() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/Reading Course/01 Section"));
    for (const auto& item : {QPair<QString, QByteArray>{"01 Introduction.txt", "Introduction text."},
                              {"02 Follow-up.txt", "Follow-up text."}}) {
      QFile lesson(root + "/Reading Course/01 Section/" + item.first);
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QCOMPARE(lesson.write(item.second), qint64(item.second.size()));
    }

    const auto database = files.path() + "/library.sqlite3";
    {
      MainWindow window(database);
      window.show();
      auto* choose = window.findChild<QPushButton*>("chooseRoot");
      QTRY_VERIFY(choose && choose->isEnabled());
      window.chooseRoot(root);
      auto* courses = window.findChild<QListView*>("courses");
      QTRY_COMPARE(courses->model()->rowCount(), 1);

      // The library's search result is a real route: select it and read the file.
      window.activateWindow();
      QTest::keyClick(&window, Qt::Key_K, Qt::ControlModifier);
      auto* search = window.findChild<SearchDialog*>();
      QTRY_VERIFY(search && search->isVisible());
      auto* query = search->findChild<QLineEdit*>("searchQuery");
      QVERIFY(query);
      query->setText("Introduction");
      auto* results = search->findChild<QListView*>("searchResults");
      QTRY_COMPARE(results->model()->rowCount(), 1);
      results->setCurrentIndex(results->model()->index(0, 0));
      QTest::keyClick(results, Qt::Key_Return);

      auto* document = window.findChild<QTextEdit*>("documentText");
      QTRY_COMPARE(document->toPlainText(), QString("Introduction text."));
      QVERIFY(document->isVisible());

      // Search selected a real lesson and opened it in its course route. At a
      // compact width, let the reader explicitly reveal the outline if it starts
      // tucked away; at desktop width, the outline and document share the page.
      auto* lessons = window.findChild<QTreeView*>("lessons");
      auto* toggle = window.findChild<QPushButton*>("toggleOutline");
      QVERIFY(lessons && toggle);
      for (const int width : {560, 1280}) {
        window.resize(width, 720);
        QTRY_VERIFY(window.width() <= width);
        if (width == 560) {
          QTRY_VERIFY(toggle->isVisible());
          QTRY_VERIFY(document->isVisible());
          QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(lessons->isVisible());
          QVERIFY(!document->isVisible());
          QTest::mouseClick(toggle, Qt::LeftButton);
          QTRY_VERIFY(document->isVisible());
          QVERIFY(!lessons->isVisible());
        } else {
          QTRY_VERIFY(lessons->isVisible());
          QTRY_VERIFY(document->isVisible());
          const auto* outline = window.findChild<QWidget*>("courseOutline");
          const auto* viewer = window.findChild<QScrollArea*>("lessonScroll");
          QTRY_VERIFY(outline->mapTo(&window, QPoint(outline->width(), 0)).x() <=
                      viewer->mapTo(&window, QPoint()).x());
        }
        if (const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS"); !captures.isEmpty())
          QVERIFY(window.grab().save(captures + QString("/course-%1.png").arg(width)));
      }
      QVERIFY(!window.findChild<QLineEdit*>("searchButton")->isVisible());

      auto* complete = window.findChild<QPushButton*>("markComplete");
      QTRY_VERIFY(complete && complete->isEnabled());
      QTest::mouseClick(complete, Qt::LeftButton);
      QTRY_COMPARE(complete->text(), QString("Mark incomplete"));
      QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
      QVERIFY(courses->isVisible());
    }

    // A new window on the same database proves completion survived close/reopen.
    MainWindow reopened(database);
    reopened.show();
    auto* choose = reopened.findChild<QPushButton*>("chooseRoot");
    QTRY_VERIFY(choose->isEnabled());
    reopened.chooseRoot(root);
    QTRY_VERIFY(choose->isEnabled());
    auto* courses = reopened.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    const auto originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([originalFont] { QApplication::setFont(originalFont); });
    auto largeFont = originalFont;
    if (largeFont.pixelSize() > 0) largeFont.setPixelSize(largeFont.pixelSize() * 2);
    else largeFont.setPointSizeF(largeFont.pointSizeF() * 2);
    QApplication::setFont(largeFont);
    reopened.resize(560, 720);
    QTRY_VERIFY(reopened.width() <= 560);
    QTRY_COMPARE(courses->horizontalScrollBar()->maximum(), 0);
    auto* resume = reopened.findChild<QPushButton*>("resumeLesson");
    QTRY_VERIFY(resume->isVisible());
    QCOMPARE(reopened.findChild<shadcn::Progress*>("resumeProgress")->value(), 50);
    QTRY_COMPARE(reopened.findChild<QLabel*>("resumeLessonTitle")->text(), QString("02 Follow-up"));
    QTest::mouseClick(resume, Qt::LeftButton);
    auto* complete = reopened.findChild<QPushButton*>("markComplete");
    auto* document = reopened.findChild<QTextEdit*>("documentText");
    QTRY_VERIFY2(complete->isEnabled(), qPrintable(reopened.findChild<QLabel*>("appStatus")->text()));
    QTRY_COMPARE(reopened.findChild<QLabel*>("lessonTitle")->text(), QString("02 Follow-up"));
    QTRY_COMPARE(document->toPlainText(), QString("Follow-up text."));

    // Resume chooses the unfinished lesson. Search back to the completed lesson
    // to prove its saved state survived closing and reopening the library.
    reopened.activateWindow();
    QTest::keyClick(&reopened, Qt::Key_K, Qt::ControlModifier);
    auto* search = reopened.findChild<SearchDialog*>();
    QTRY_VERIFY(search && search->isVisible());
    auto* query = search->findChild<QLineEdit*>("searchQuery");
    query->setText("Introduction");
    auto* results = search->findChild<QListView*>("searchResults");
    QTRY_COMPARE(results->model()->rowCount(), 1);
    results->setCurrentIndex(results->model()->index(0, 0));
    QTest::keyClick(results, Qt::Key_Return);
    QTRY_COMPARE(document->toPlainText(), QString("Introduction text."));
    QTRY_COMPARE(complete->text(), QString("Mark incomplete"));
  }

  void openPdfFromCourseOutline() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    QVERIFY(QDir().mkpath(root + "/PDF Course/Section"));
    {
      QPdfWriter writer(root + "/PDF Course/Section/Reading.pdf");
      writer.setResolution(72);
      QPainter painter(&writer);
      for (int page = 1; page <= 3; ++page) {
        if (page > 1) QVERIFY(writer.newPage());
        painter.drawText(50, 50, QString("Local PDF lesson · page %1").arg(page));
      }
    }
    MainWindow window(files.path() + "/library.sqlite3");
    window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 1);
    QTest::keyClick(courses, Qt::Key_Return);
    auto* lessons = window.findChild<QTreeView*>("lessons");
    QTRY_COMPARE(lessons->model()->rowCount(), 1);
    QTest::keyClick(lessons, Qt::Key_Return);
    auto* pdf = window.findChild<PdfView*>();
    QVERIFY(pdf);
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* page = window.findChild<QLineEdit*>("pdfPage");
    QVERIFY(page);
    page->setText("3");
    QTest::keyClick(page, Qt::Key_Return);
    QTRY_COMPARE(page->text(), QString("3"));
    QTRY_VERIFY(pdf->cachedTiles() > 0);
    auto* back = window.findChild<QPushButton*>("backToLibrary");
    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    QCOMPARE(pdf->cachedTiles(), 0);
  }

  void keyboardPopupAndTextInputStayScoped() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    for (const auto& name : {"Keyboard", "Second"}) {
      QVERIFY(QDir().mkpath(root + "/" + name + "/Section"));
      QFile lesson(root + "/" + name + "/Section/Read.txt");
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QCOMPARE(lesson.write("Keyboard lesson"), qint64(15));
    }
    MainWindow window(files.path() + "/library.sqlite3");
    window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 2);
    window.activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(&window));

    QTest::keyClick(&window, Qt::Key_F1);
    QPointer<QDialog> shortcuts = window.findChild<QDialog*>("shortcutHelp");
    QTRY_VERIFY(shortcuts && shortcuts->isVisible());
    shortcuts->reject();
    QTRY_VERIFY(shortcuts.isNull());

    QLineEdit input(&window);
    input.show();
    input.setFocus();
    QTest::keyClick(&input, Qt::Key_J);
    QCOMPARE(input.text(), QString("j"));
    QTest::keyClick(&input, Qt::Key_F1);
    shortcuts = window.findChild<QDialog*>("shortcutHelp");
    QTRY_VERIFY(shortcuts && shortcuts->isVisible());
    shortcuts->reject();
    QTRY_VERIFY(shortcuts.isNull());

    courses->setFocus();
    courses->setCurrentIndex(courses->model()->index(1, 0));
    QTest::keyClick(courses, Qt::Key_G);
    input.setFocus();
    QTest::keyClick(&input, Qt::Key_X);
    QCOMPARE(input.text(), QString("jx"));
    courses->setFocus();
    QTest::keyClick(courses, Qt::Key_G);
    QCOMPARE(courses->currentIndex().row(), 1);
    QTest::keyClick(courses, Qt::Key_G);
    QCOMPARE(courses->currentIndex().row(), 0);

    QTest::keyClick(&window, Qt::Key_Space, Qt::ControlModifier);
    QPointer<QDialog> palette = window.findChild<QDialog*>("commandPalette");
    QTRY_VERIFY(palette && palette->isVisible());
    auto* paletteSearch = palette->findChild<QLineEdit*>("keyboardPopupCommand");
    if (!paletteSearch) paletteSearch = palette->findChild<QLineEdit*>();
    QVERIFY(paletteSearch);
    paletteSearch->setText("last item");
    QTest::keyClick(paletteSearch, Qt::Key_Return);
    QTRY_VERIFY(palette.isNull() || !palette->isVisible());
    QCOMPARE(courses->currentIndex().row(), 1);
  }
};

QTEST_MAIN(MainWindowTest)
#include "main_window_test.moc"
