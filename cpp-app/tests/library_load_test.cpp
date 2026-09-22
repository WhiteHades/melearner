#include "library.hpp"
#include "fixtures/parity_fixture.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <algorithm>

namespace lib = melearner::library;

namespace {
qint64 privateResidentKiB() {
  QFile memory("/proc/self/smaps_rollup");
  if (!memory.open(QIODevice::ReadOnly)) return -1;
  qint64 total = 0;
  for (const auto& rawLine : memory.readAll().split('\n')) {
    const auto line = rawLine.simplified();
    if (line.startsWith("Private_Clean:") || line.startsWith("Private_Dirty:") ||
        line.startsWith("Private_Hugetlb:")) total += line.split(' ').value(1).toLongLong();
  }
  return total;
}
}

class LibraryLoadTest final : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() { Q_INIT_RESOURCE(assets); }
  void fullFixtureRemainsResponsive() {
    QTemporaryDir files; QVERIFY(files.isValid());
    const auto generated = melearner::fixtures::generate_recipe_v1({
      .output_root = (files.path() + "/fixture").toStdString(),
      .physical_mode = melearner::fixtures::PhysicalMode::full,
    });
    QCOMPARE(generated.counts.lessons, 100'000U);
    QCOMPARE(generated.physical_lessons_present, 99'802U);
    qInfo("Local diagnostic only; installed release-profile qualification is separate.");
    const auto database = files.path() + "/library.sqlite3";
    lib::Library library(database);
    QSignalSpy opened(&library, &lib::Library::opened);
    QSignalSpy errors(&library, &lib::Library::failed);
    QSignalSpy scanned(&library, &lib::Library::scanFinished);
    QSignalSpy pages(&library, &lib::Library::coursesReady);
    QSignalSpy searches(&library, &lib::Library::searchReady);
    QSignalSpy stats(&library, &lib::Library::statsReady);
    QVERIFY(library.open()); QTRY_COMPARE(opened.size(), 1);

    QElapsedTimer elapsed; elapsed.start();
    qint64 previousTick = 0, worstGap = 0;
    int ticks = 0;
    QTimer heartbeat; heartbeat.setInterval(10);
    connect(&heartbeat, &QTimer::timeout, this, [&] {
      const auto now = elapsed.elapsed();
      worstGap = std::max(worstGap, now - previousTick); previousTick = now; ++ticks;
    });
    heartbeat.start();
    QVERIFY(library.scan(files.path() + "/fixture/library"));
    QTRY_VERIFY_WITH_TIMEOUT(!scanned.isEmpty() || !errors.isEmpty(), 300'000);
    heartbeat.stop();
    QVERIFY2(errors.isEmpty(), errors.isEmpty() ? "" : qPrintable(qvariant_cast<lib::Error>(errors.first().at(1)).message));
    const auto scan = qvariant_cast<lib::ScanResult>(scanned.takeFirst().at(1));
    QCOMPARE(scan.courses, 998U); QCOMPARE(scan.lessons, generated.physical_lessons_present);
    qInfo("Scan: %lld ms; event-loop heartbeat: %d samples, worst gap %lld ms", elapsed.elapsed(), ticks, worstGap);
    QVERIFY(ticks > 0); QVERIFY2(worstGap < 100, "Library scan stalled the event loop");

    quint64 lessons = 0;
    qint64 slowestPage = 0;
    for (quint64 offset = 0; offset < scan.courses; offset += 128) {
      elapsed.restart(); QVERIFY(library.courses(offset, 128));
      QVERIFY(!pages.isEmpty() || pages.wait(10'000));
      slowestPage = std::max(slowestPage, elapsed.elapsed());
      const auto page = qvariant_cast<lib::CoursePage>(pages.takeFirst().at(1));
      QCOMPARE(page.offset, offset); QCOMPARE(page.total, scan.courses);
      QCOMPARE(page.rows.size(), std::min<quint64>(128, scan.courses - offset));
      for (const auto& course : page.rows) lessons += course.lessonCount;
    }
    QCOMPARE(lessons, scan.lessons);
    qInfo("Slowest 128-Course page: %lld ms", slowestPage);
    QVERIFY2(slowestPage < 200, "Course page budget exceeded");

    elapsed.restart(); QVERIFY(library.search("Lesson", 0, 100));
    QVERIFY(!searches.isEmpty() || searches.wait(10'000));
    const auto searchMs = elapsed.elapsed();
    const auto search = qvariant_cast<lib::SearchPage>(searches.takeFirst().at(1));
    QCOMPARE(search.rows.size(), 100); QVERIFY(search.total > 100);
    qInfo("100-result search: %lld ms", searchMs);
    QVERIFY2(searchMs < 200, "Search budget exceeded");

    QVERIFY(library.stats(scan.revision)); QVERIFY(!stats.isEmpty() || stats.wait(10'000));
    QCOMPARE(qvariant_cast<lib::LibraryStats>(stats.takeFirst().at(1)).lessons, scan.lessons);
    const auto privateKiB = privateResidentKiB();
    qInfo("Library process private resident memory: %lld KiB", privateKiB);
    if (privateKiB >= 0) QVERIFY2(privateKiB <= 384 * 1024, "Library private-memory budget exceeded");
    QVERIFY(errors.isEmpty());
    elapsed.restart(); library.close();
    qInfo("Library shutdown: %lld ms", elapsed.elapsed());
    QVERIFY(elapsed.elapsed() < 2000);

    lib::Library reopened(database);
    QSignalSpy restored(&reopened, &lib::Library::opened);
    elapsed.restart(); QVERIFY(reopened.open()); QVERIFY(!restored.isEmpty() || restored.wait(10'000));
    qInfo("Indexed Library reopen: %lld ms", elapsed.elapsed());
    QVERIFY(elapsed.elapsed() < 2000);
    const auto startup = qvariant_cast<lib::Startup>(restored.first().at(1));
    QCOMPARE(startup.courses.size(), 128); QVERIFY(startup.hasMoreCourses);
  }
};
QTEST_GUILESS_MAIN(LibraryLoadTest)
#include "library_load_test.moc"
