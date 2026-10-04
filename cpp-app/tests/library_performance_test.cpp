#include "main_window.hpp"
#include "search_dialog.hpp"
#include "course_document_view.hpp"
#include "theme.hpp"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QPushButton>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QDateTime>
#include <QSettings>
#include <QTreeView>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtTest>

class LibraryPerformanceTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    Q_INIT_RESOURCE(assets);
    melearner::installTheme(true, 14);
  }

  void startupSearchAndNavigation() {
    QElapsedTimer runClock;
    runClock.start();
    const int lessonCount = qEnvironmentVariableIntValue("MELEARNER_PERF_LESSONS");
    QVERIFY(lessonCount == 10'000 || lessonCount == 30'000);
    QTemporaryDir temp(qEnvironmentVariable("MELEARNER_PERF_TMPDIR"));
    QVERIFY(temp.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("melearner-performance"));
    QCoreApplication::setApplicationName(QStringLiteral("library-workload"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path() + "/settings");
    QSettings preferences;
    preferences.setValue("updates/automatic", false);
    preferences.setValue("updates/lastCheck", QDateTime::currentSecsSinceEpoch());
    preferences.sync();
    const auto root = temp.path() + "/Courses";
    const int courseCount = lessonCount / 1'000;
    for (int course = 0; course < courseCount; ++course) {
      const auto section = root + QString("/Course %1/Section").arg(course, 2, 10, QChar('0'));
      QVERIFY(QDir().mkpath(section));
      for (int lesson = 0; lesson < 1'000; ++lesson) {
        const auto lessonName = course == 0 && lesson == 0
            ? QStringLiteral("00 Needle unique searchable lesson.html")
            : QString("Lesson %1 %2.html").arg(course, 2, 10, QChar('0')).arg(lesson, 4, 10, QChar('0'));
        QFile file(section + "/" + lessonName);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray body = course == 0 && lesson == 0
            ? "<html><body><h1>Needle unique searchable lesson</h1><p>" : "<html><body><h1>Routine lesson</h1><p>";
        body += "A local course lesson with enough prose to exercise the document index and reader. ";
        while (body.size() < 256) body += "Students can read and revisit this course material on their own device. ";
        body += "</p></body></html>";
        QCOMPARE(file.write(body), qint64(body.size()));
      }
    }

    const auto database = temp.path() + "/library.sqlite3";
    {
    QElapsedTimer startup;
    startup.start();
    MainWindow window(database);
    window.show();
    auto* choose = window.findChild<QPushButton*>("chooseRoot");
    QTRY_VERIFY_WITH_TIMEOUT(choose && choose->isEnabled(), 30000);
    const auto uiReadyMs = startup.elapsed();
    QElapsedTimer scan;
    scan.start();
    window.chooseRoot(root);
    auto* courses = window.findChild<QListView*>("courses");
    QTRY_COMPARE_WITH_TIMEOUT(courses->model()->rowCount(), courseCount, 180000);
    const auto scanReadyMs = scan.elapsed();
    qInfo().noquote() << "PERF_PHASE phase=initial_library_ready elapsed_ms=" << runClock.elapsed()
                      << "workload=" << lessonCount << "ui_ready_ms=" << uiReadyMs
                      << "scan_and_courses_ms=" << scanReadyMs;

    if (qEnvironmentVariable("MELEARNER_PERF_MODE") == QLatin1String("initial-open")) {
      auto* outline = window.findChild<QTreeView*>("lessons");
      auto* reader = window.findChild<melearner::CourseDocumentView*>("courseDocumentView");
      QVERIFY(outline && reader);
      QSignalSpy loaded(reader, &melearner::CourseDocumentView::loaded);
      auto* outlineModel = outline->model();
      QVERIFY(outlineModel);
      QElapsedTimer courseToOutline;
      QElapsedTimer courseToDom;
      qint64 outlineSignalMs = -1;
      const auto recordOutlineReady = [&] {
        if (outlineSignalMs < 0 && outlineModel->rowCount() > 0)
          outlineSignalMs = courseToOutline.elapsed();
      };
      courseToOutline.start();
      courseToDom.start();
      connect(outlineModel, &QAbstractItemModel::modelReset, this, recordOutlineReady);
      connect(outlineModel, &QAbstractItemModel::rowsInserted, this,
              [recordOutlineReady](const QModelIndex&, int, int) { recordOutlineReady(); });
      const auto firstCourse = courses->model()->index(0, 0);
      courses->scrollTo(firstCourse);
      QTRY_VERIFY_WITH_TIMEOUT(!courses->visualRect(firstCourse).isEmpty(), 10000);
      QTest::mouseClick(courses->viewport(), Qt::LeftButton, Qt::NoModifier,
                        courses->visualRect(firstCourse).center());
      QTRY_VERIFY_WITH_TIMEOUT(outline->model()->rowCount() > 0, 30000);
      const auto outlinePollingMs = courseToOutline.elapsed();
      QVERIFY(outlineSignalMs >= 0);
      QTRY_VERIFY_WITH_TIMEOUT(loaded.count() > 0, 30000);
      QVERIFY(loaded.last().at(0).toBool());
      auto* browser = window.findChild<QWebEngineView*>("documentBrowser");
      QTRY_VERIFY_WITH_TIMEOUT(browser != nullptr, 10000);
      QVariant renderedText;
      browser->page()->runJavaScript("document.body.innerText", [&renderedText](const QVariant& value) {
        renderedText = value;
      });
      QTRY_VERIFY_WITH_TIMEOUT(renderedText.toString().contains("Needle unique searchable lesson"), 10000);
      QVERIFY(renderedText.toString().contains("Needle unique searchable lesson"));
      qInfo().noquote() << "PERF_CONTROLLED phase=course_first_reader_ready workload=" << lessonCount
                        << "outline_signal_ms=" << outlineSignalMs
                        << "outline_polling_ms=" << outlinePollingMs
                        << "course_to_dom_ready_ms=" << courseToDom.elapsed();
      return;
    }

    QElapsedTimer openCourse;
    openCourse.start();
    courses->setCurrentIndex(courses->model()->index(0, 0));
    QTest::keyClick(courses, Qt::Key_Return);
    auto* outline = window.findChild<QTreeView*>("lessons");
    QTRY_VERIFY_WITH_TIMEOUT(outline && outline->model()->rowCount() > 0, 30000);
    QTRY_VERIFY_WITH_TIMEOUT(outline->model()->index(0, 0).isValid(), 30000);
    const auto courseOpenMs = openCourse.elapsed();
    auto sectionIndex = outline->model()->index(0, 0);
    outline->expand(sectionIndex);
    QTRY_VERIFY_WITH_TIMEOUT(outline->model()->rowCount(sectionIndex) > 0, 30000);
    const auto lessonIndex = outline->model()->index(0, 0, sectionIndex);
    QTRY_VERIFY_WITH_TIMEOUT(lessonIndex.data(Qt::DisplayRole).toString().contains("Needle"), 30000);
    QElapsedTimer readerOpen;
    readerOpen.start();
    auto* courseDocument = window.findChild<melearner::CourseDocumentView*>("courseDocumentView");
    QVERIFY(courseDocument);
    QSignalSpy loaded(courseDocument, &melearner::CourseDocumentView::loaded);
    outline->scrollTo(lessonIndex);
    QTRY_VERIFY_WITH_TIMEOUT(!outline->visualRect(lessonIndex).isEmpty(), 10000);
    QTest::mouseClick(outline->viewport(), Qt::LeftButton, Qt::NoModifier,
                      outline->visualRect(lessonIndex).center());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() > 0, 30000);
    QVERIFY(loaded.last().at(0).toBool());
    QWebEngineView* browser = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((browser = window.findChild<QWebEngineView*>("documentBrowser")) != nullptr, 10000);
    QVariant renderedText;
    browser->page()->runJavaScript("document.body.innerText", [&renderedText](const QVariant& value) {
      renderedText = value;
    });
    QTRY_VERIFY_WITH_TIMEOUT(renderedText.toString().contains("Needle unique searchable lesson"), 10000);
    const auto readerOpenMs = readerOpen.elapsed();
    qInfo().noquote() << "PERF_PHASE phase=course_and_reader_ready elapsed_ms=" << runClock.elapsed()
                      << "course_open_ms=" << courseOpenMs << "reader_open_ms=" << readerOpenMs;

    auto* back = window.findChild<QPushButton*>("backToLibrary");
    QVERIFY(back);
    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(courses->isVisible(), 10000);
    auto* readingPanel = window.findChild<melearner::CourseDocumentView*>("courseDocumentView");
    QVERIFY(readingPanel);
    QSignalSpy cycleReady(readingPanel, &melearner::CourseDocumentView::loaded);
    auto* searchButton = window.findChild<QPushButton*>("searchButton");
    QVERIFY(searchButton);
    QElapsedTimer searches;
    searches.start();
    for (int repeat = 0; repeat < 5; ++repeat) {
      qInfo().noquote() << "PERF_PHASE phase=navigation_cycle elapsed_ms=" << runClock.elapsed()
                        << "index=" << repeat << "state=start";
      QElapsedTimer cycleTimer;
      cycleTimer.start();
      QTRY_VERIFY_WITH_TIMEOUT(searchButton->isVisible(), 10000);
      QTest::mouseClick(searchButton, Qt::LeftButton);
      auto* dialog = window.findChild<SearchDialog*>();
      QTRY_VERIFY_WITH_TIMEOUT(dialog && dialog->isVisible(), 10000);
      auto* query = dialog->findChild<QLineEdit*>("searchQuery");
      auto* results = dialog->findChild<QListView*>("searchResults");
      QVERIFY(query && results);
      query->setText("Needle unique searchable lesson");
      QTRY_COMPARE_WITH_TIMEOUT(results->model()->rowCount(), 1, 30000);
      results->setCurrentIndex(results->model()->index(0, 0));
      cycleReady.clear();
      QTest::keyClick(results, Qt::Key_Return);
      // Search resolution creates the browser surface asynchronously.
      QTRY_VERIFY_WITH_TIMEOUT((browser = window.findChild<QWebEngineView*>("documentBrowser")) != nullptr, 30000);
      QTRY_VERIFY_WITH_TIMEOUT(cycleReady.count() > 0, 30000);
      QVERIFY(cycleReady.last().at(0).toBool());
      renderedText.clear();
      browser->page()->runJavaScript("document.body.innerText", [&renderedText](const QVariant& value) {
        renderedText = value;
      });
      QTRY_VERIFY_WITH_TIMEOUT(renderedText.toString().contains("Needle unique searchable lesson"), 10000);
      QTRY_VERIFY_WITH_TIMEOUT(!window.findChild<SearchDialog*>(), 10000);
      if (repeat < 4) {
        QTest::mouseClick(back, Qt::LeftButton);
        QTRY_VERIFY_WITH_TIMEOUT(courses->isVisible(), 10000);
      }
      qInfo().noquote() << "PERF_PHASE phase=navigation_cycle index=" << repeat
                        << "state=ready cycle_ms=" << cycleTimer.elapsed()
                        << "run_elapsed_ms=" << runClock.elapsed();
    }
    const auto navigationMs = searches.elapsed();
    qInfo().noquote() << "PERF_PHASE phase=navigation_complete cycles=5 total_ms=" << navigationMs;
    qInfo().noquote() << "PERF_PHASE phase=sustained_open_idle seconds=30 state=start elapsed_ms="
                      << runClock.elapsed();
    QTest::qWait(30000);
    qInfo().noquote() << "PERF_PHASE phase=sustained_open_idle seconds=30 state=complete elapsed_ms="
                      << runClock.elapsed();
    QTest::mouseClick(back, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(courses->isVisible(), 10000);
    qInfo().noquote() << "PERF_PHASE phase=reader_closed_warm_idle seconds=30 state=start elapsed_ms="
                      << runClock.elapsed();
    QTest::qWait(30000);
    qInfo().noquote() << "PERF_PHASE phase=reader_closed_warm_idle seconds=30 state=complete elapsed_ms="
                      << runClock.elapsed();
    }

    qInfo().noquote() << "PERF_PHASE phase=cached_database_reopen state=start elapsed_ms=" << runClock.elapsed();
    QElapsedTimer cachedStartup;
    cachedStartup.start();
    MainWindow reopened(database);
    reopened.show();
    auto* reopenedChoose = reopened.findChild<QPushButton*>("chooseRoot");
    QTRY_VERIFY_WITH_TIMEOUT(reopenedChoose && reopenedChoose->isEnabled(), 30000);
    auto* reopenedCourses = reopened.findChild<QListView*>("courses");
    QTRY_COMPARE_WITH_TIMEOUT(reopenedCourses->model()->rowCount(), courseCount, 30000);
    const auto cachedReadyMs = cachedStartup.elapsed();
    qInfo().noquote() << "PERF_PHASE phase=cached_database_first_list_ready elapsed_ms=" << runClock.elapsed()
                      << "cached_ready_ms=" << cachedReadyMs << "courses=" << courseCount
                      << "lessons=" << lessonCount;
  }
};

QTEST_MAIN(LibraryPerformanceTest)
#include "library_performance_test.moc"
