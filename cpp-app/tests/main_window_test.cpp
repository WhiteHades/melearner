#include "main_window.hpp"
#include "pdf_view.hpp"
#include "search_dialog.hpp"
#include "theme.hpp"

#include <QDir>
#include <QComboBox>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
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
#include <zip.h>

class MainWindowTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); melearner::installTheme(true, 14); }

  void nativeReadersAndLibraryPresentation() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + "/Courses";
    const auto section = root + "/Native Readers/Section";
    QVERIFY(QDir().mkpath(section));
    const QList<QPair<QString, QByteArray>> lessons{
      {"01 Reading.md", "# Markdown lesson\n\nRead this in the app.\n\n- First idea\n- Second idea\n"},
      {"02 Reading.html", "<html><body><h1>HTML lesson</h1><p>Read this here too.</p></body></html>"},
      {"03 rom-cu.csv", "Address,Value\r\n0,\"A, B\"\r\n1,\"C\"\"D\"\r\n"},
      {"05 output.hex.txt", "v3.0 hex words addressed\n0000: 01 02 03"}};
    for (const auto& [name, bytes] : lessons) {
      QFile lesson(section + "/" + name);
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QCOMPARE(lesson.write(bytes), qint64(bytes.size()));
    }
    const auto workbookPath = QFile::encodeName(section + "/04 rom-cu.xlsx");
    auto* archive = zip_open(workbookPath.constData(), ZIP_CREATE | ZIP_TRUNCATE, nullptr);
    QVERIFY(archive);
    const QList<QPair<QByteArray, QByteArray>> entries{
      {"xl/workbook.xml", "<workbook xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\"><sheets><sheet name=\"Registers\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>"},
      {"xl/_rels/workbook.xml.rels", "<Relationships><Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/></Relationships>"},
      {"xl/sharedStrings.xml", "<sst><si><t>Address</t></si><si><t>Value</t></si></sst>"},
      {"xl/worksheets/sheet1.xml", "<worksheet><sheetData><row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c></row><row r=\"2\"><c r=\"A2\"><v>42</v></c><c r=\"B2\" t=\"inlineStr\"><is><t>Register</t></is></c></row></sheetData></worksheet>"}};
    for (const auto& [name, bytes] : entries) {
      auto* source = zip_source_buffer(archive, bytes.constData(), bytes.size(), 0);
      QVERIFY(source);
      QVERIFY(zip_file_add(archive, name.constData(), source, ZIP_FL_OVERWRITE) >= 0);
    }
    QCOMPARE(zip_close(archive), 0);
    for (int course = 1; course <= 11; ++course) {
      const auto path = root + QString("/Course %1/Section").arg(course, 2, 10, QChar('0'));
      QVERIFY(QDir().mkpath(path));
      QFile lesson(path + "/Reading.txt");
      QVERIFY(lesson.open(QIODevice::WriteOnly));
      QVERIFY(lesson.write("A quiet place to learn.") > 0);
    }
    MainWindow window(files.path() + "/library.sqlite3");
    window.resize(1440, 900); window.show();
    QTRY_VERIFY(window.findChild<QPushButton*>("chooseRoot")->isEnabled());
    window.chooseRoot(root);
    auto* courses = window.findChild<shadcn::ListView*>("courses");
    QTRY_COMPARE(courses->model()->rowCount(), 12);
    const auto capture = [&window](const QString& name) {
      const auto path = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS");
      return path.isEmpty() || window.grab().save(path + "/" + name + ".png");
    };
    auto* list = window.findChild<QPushButton*>("listView");
    auto* cards = window.findChild<QPushButton*>("cardsView");
    QTest::mouseClick(cards, Qt::LeftButton);
    QTRY_COMPARE(courses->presentation(), shadcn::ListPresentation::Cards);
    QTest::qWait(300);
    const auto rects = courses->visibleRowRects();
    QVERIFY(rects.size() >= 3);
    QVERIFY(rects[1].left() > rects[0].left());
    QVERIFY(rects[0].right() < rects[1].left());
    QVERIFY(window.findChild<QWidget*>("libraryCanvas")->width() <= 1120);
    QVERIFY(capture("library-cards"));
    QTest::mouseClick(list, Qt::LeftButton);
    QTRY_COMPARE(courses->presentation(), shadcn::ListPresentation::List);
    QVERIFY(capture("library-list"));
    auto* settings = window.findChild<QPushButton*>("appearance");
    QVERIFY(settings && settings->menu());
    QCOMPARE(settings->menu()->actions().size(), 4); // Three choices plus a separator.
    for (auto* action : settings->menu()->actions()) QVERIFY(!action->menu());
    const auto index = courses->model()->index(11, 0);
    QTRY_COMPARE(index.data().toString(), QString("Native Readers"));
    courses->setCurrentIndex(index);
    QTest::keyClick(courses, Qt::Key_Return);
    auto* document = window.findChild<QTextEdit*>("documentText");
    auto* next = window.findChild<QPushButton*>("nextLesson");
    auto* actions = window.findChild<QWidget*>("lessonActions");
    QTRY_VERIFY(document->toPlainText().contains("Markdown lesson"));
    QVERIFY(!window.findChild<QWidget*>("documentTools")->isVisible());
    QVERIFY(actions->mapTo(&window, QPoint()).y() < document->mapTo(&window, QPoint()).y());
    QVERIFY(document->width() <= 900);
    QVERIFY(capture("reader-markdown"));
    const QStringList expected{"HTML lesson", "A, B", "Register", "v3.0 hex words"};
    for (int item = 0; item < expected.size(); ++item) {
      QTest::mouseClick(next, Qt::LeftButton);
      QTRY_VERIFY(document->toPlainText().contains(expected[item]));
      QVERIFY(capture(QString("reader-%1").arg(item)));
    }
    QTest::mouseClick(window.findChild<QPushButton*>("backToLibrary"), Qt::LeftButton);
    QTRY_VERIFY(courses->isVisible());
    QVERIFY(!window.findChild<QWidget*>("appStatus")->isVisible());
  }

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
          const auto* viewer = window.findChild<QScrollArea*>("lessonScroll");
          QTRY_VERIFY(viewer->mapTo(&window, QPoint()).x() < 50);
          QTRY_VERIFY(document->mapTo(&window, QPoint(document->width(), 0)).x() <= window.width());
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
    window.resize(1920, 1080);
    QTRY_VERIFY(pdf->width() <= 1000);
    if (const auto captures = qEnvironmentVariable("MELEARNER_TEST_SCREENSHOTS"); !captures.isEmpty())
      QVERIFY(window.grab().save(captures + "/reader-pdf.png"));
    auto* zoom = window.findChild<QComboBox*>("pdfZoom");
    QVERIFY(zoom);
    bool zoomInvalidatedTiles = false;
    connect(zoom, &QComboBox::activated, &window, [&] { zoomInvalidatedTiles = pdf->cachedTiles() == 0; });
    QTest::keyClick(zoom, Qt::Key_End);
    QCOMPARE(pdf->zoomForTesting(), 64);
    QVERIFY(zoomInvalidatedTiles);
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
    QVERIFY(!window.findChild<QWidget*>("appStatus")->isVisible());
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

    window.activateWindow();
    courses->setFocus();
    QTRY_VERIFY(courses->hasFocus());
    courses->setCurrentIndex(courses->model()->index(1, 0));
    QTest::keyClick(courses, Qt::Key_G);
    input.setFocus();
    QTest::keyClick(&input, Qt::Key_X);
    QCOMPARE(input.text(), QString("jx"));
    window.activateWindow();
    courses->setFocus();
    QTRY_VERIFY(courses->hasFocus());
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
