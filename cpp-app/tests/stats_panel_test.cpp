#include "library.hpp"
#include "stats_panel.hpp"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>

#include <cstdint>

namespace {

using melearner::StatsPanel;
using melearner::library::CoursePage;
using melearner::library::Library;
using melearner::library::LessonPage;
using melearner::library::ProgressResult;
using melearner::library::ScanResult;
using melearner::library::Startup;

void writeFile(const QString& path) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QVERIFY(file.write("fixture") > 0);
}

bool waitFor(QSignalSpy& spy, int timeoutMs = 5'000) {
    return !spy.isEmpty() || spy.wait(timeoutMs);
}

bool openAndScan(Library& library, const QString& root, std::uint64_t* revision) {
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanned(&library, &Library::scanFinished);
    const auto openedOk = library.open() != 0 && waitFor(opened);
    const auto scanId = openedOk ? library.scan(root) : 0;
    const auto scannedOk = scanId != 0 && waitFor(scanned);
    if (!openedOk || !scannedOk) {
        return false;
    }
    const auto result = qvariant_cast<ScanResult>(scanned.takeFirst().at(1));
    *revision = result.revision;
    return *revision != 0;
}

bool readLesson(Library& library, QString* lessonId, std::uint64_t* revision) {
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    QSignalSpy lessonsReady(&library, &Library::lessonsReady);
    if (library.courses() == 0 || !waitFor(coursesReady)) {
        return false;
    }
    const auto courses = qvariant_cast<CoursePage>(coursesReady.takeFirst().at(1));
    if (courses.rows.isEmpty() || library.lessons(courses.rows.front().id) == 0 || !waitFor(lessonsReady)) {
        return false;
    }
    const auto lessons = qvariant_cast<LessonPage>(lessonsReady.takeFirst().at(1));
    if (lessons.rows.isEmpty()) {
        return false;
    }
    *lessonId = lessons.rows.front().id;
    *revision = lessons.revision;
    return true;
}

QString createFixture(const QString& root, int courseCount = 1) {
    for (int course = 0; course < courseCount; ++course) {
        const auto section = root + QStringLiteral("/Course %1/Section").arg(course)
            + QStringLiteral("/Lessons");
        if (!QDir().mkpath(section)) {
            return {};
        }
        writeFile(section + QStringLiteral("/Lesson %1.mp4").arg(course));
        writeFile(section + QStringLiteral("/Reference %1.pdf").arg(course));
    }
    return root;
}

}  // namespace

class StatsPanelTest final : public QObject {
    Q_OBJECT

private slots:
    void rendersBoundedSnapshotAndZeroFilledActivity();
    void ignoresResultsAfterDeactivation();
    void displaysPositionDerivedActivityOnly();
};

void StatsPanelTest::rendersBoundedSnapshotAndZeroFilledActivity() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    QVERIFY(!createFixture(root, 5).isEmpty());

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    std::uint64_t revision = 0;
    QVERIFY(openAndScan(library, root, &revision));

    StatsPanel panel(library);
    panel.setActive(true, revision);
    panel.show();
    auto* activity = panel.findChild<QTableWidget*>(QStringLiteral("activityGrid"));
    auto* media = panel.findChild<QTableWidget*>(QStringLiteral("mediaTable"));
    auto* topCourses = panel.findChild<QTableWidget*>(QStringLiteral("topCoursesTable"));
    auto* coursesValue = panel.findChild<QLabel*>(QStringLiteral("coursesValue"));
    QVERIFY(activity != nullptr);
    QVERIFY(media != nullptr);
    QVERIFY(topCourses != nullptr);
    QVERIFY(coursesValue != nullptr);

    QTRY_COMPARE_WITH_TIMEOUT(coursesValue->text(), QStringLiteral("5 / 5"), 5'000);
    QCOMPARE(media->rowCount(), 2);
    QCOMPARE(topCourses->rowCount(), 4);
    QCOMPARE(activity->rowCount(), 7);
    QCOMPARE(activity->columnCount(), 12);
    QTRY_VERIFY_WITH_TIMEOUT(activity->item(0, 0) != nullptr, 5'000);
    for (int row = 0; row < activity->rowCount(); ++row) {
        for (int column = 0; column < activity->columnCount(); ++column) {
            const auto* item = activity->item(row, column);
            QVERIFY(item != nullptr);
            QCOMPARE(item->text(), QStringLiteral("0"));
            QVERIFY(item->data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("watched")));
            QVERIFY(item->toolTip().contains(QStringLiteral("completions")));
        }
    }
    QVERIFY(activity->accessibleName().contains(QStringLiteral("activity"), Qt::CaseInsensitive));
}

void StatsPanelTest::ignoresResultsAfterDeactivation() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    QVERIFY(!createFixture(root).isEmpty());

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    std::uint64_t revision = 0;
    QVERIFY(openAndScan(library, root, &revision));

    StatsPanel panel(library);
    panel.setActive(true, revision);
    panel.setActive(false, 0);
    auto* coursesValue = panel.findChild<QLabel*>(QStringLiteral("coursesValue"));
    auto* status = panel.findChild<QLabel*>(QStringLiteral("statsStatus"));
    QVERIFY(coursesValue != nullptr);
    QVERIFY(status != nullptr);
    QTest::qWait(250);
    QCOMPARE(coursesValue->text(), QStringLiteral("—"));
    QVERIFY(!status->text().contains(QStringLiteral("updated"), Qt::CaseInsensitive));
}

void StatsPanelTest::displaysPositionDerivedActivityOnly() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    QVERIFY(!createFixture(root).isEmpty());

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    std::uint64_t revision = 0;
    QVERIFY(openAndScan(library, root, &revision));
    QString lessonId;
    QVERIFY(readLesson(library, &lessonId, &revision));

    QSignalSpy progressSaved(&library, &Library::progressSaved);
    QVERIFY(library.saveProgress(lessonId, 90'000, 300'000, false) != 0);
    QVERIFY(waitFor(progressSaved));
    const auto progress = qvariant_cast<ProgressResult>(progressSaved.takeFirst().at(1));
    QVERIFY(progress.revision != 0);

    StatsPanel panel(library);
    panel.setActive(true, progress.revision);
    auto* activity = panel.findChild<QTableWidget*>(QStringLiteral("activityGrid"));
    QVERIFY(activity != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(activity->item(0, 0) != nullptr, 5'000);

    bool foundWatchedActivity = false;
    for (int row = 0; row < activity->rowCount(); ++row) {
        for (int column = 0; column < activity->columnCount(); ++column) {
            const auto* item = activity->item(row, column);
            QVERIFY(item != nullptr);
            if (item->data(Qt::AccessibleTextRole).toString().contains(QStringLiteral("1m 30s watched"))) {
                foundWatchedActivity = true;
            }
        }
    }
    QVERIFY(foundWatchedActivity);
}

QTEST_MAIN(StatsPanelTest)
#include "stats_panel_test.moc"
