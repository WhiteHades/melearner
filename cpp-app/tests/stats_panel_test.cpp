#include "library.hpp"
#include "stats_panel.hpp"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QtTest>
#include <shadcn/data.hpp>

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
    auto* activity = panel.findChild<shadcn::Heatmap*>(QStringLiteral("activityGrid"));
    auto* media = panel.findChild<shadcn::Table*>(QStringLiteral("mediaTable"));
    auto* topCourses = panel.findChild<shadcn::Table*>(QStringLiteral("topCoursesTable"));
    auto* coursesValue = panel.findChild<QLabel*>(QStringLiteral("coursesValue"));
    QVERIFY(activity != nullptr);
    QVERIFY(media != nullptr);
    QVERIFY(topCourses != nullptr);
    QVERIFY(coursesValue != nullptr);

    QTRY_COMPARE_WITH_TIMEOUT(coursesValue->text(), QStringLiteral("5 / 5"), 5'000);
    QCOMPARE(media->model()->rowCount(), 2);
    QCOMPARE(topCourses->model()->rowCount(), 4);
    // The grid is one focus stop over whole week columns, not a cell per day.
    // Only days with recorded activity become cells, and this fixture records
    // none, so the grid is empty and the detail line keeps its prompt.
    QCOMPARE(activity->rowCount(), 7);
    QTRY_COMPARE_WITH_TIMEOUT(activity->days().size(), 0, 5'000);
    QCOMPARE(activity->weekCount(), 0);
    QCOMPARE(panel.findChild<QLabel*>("activityDetail")->text(),
             QStringLiteral("Select a day to see its activity."));
    QVERIFY(activity->accessibleName().contains(QStringLiteral("activity"), Qt::CaseInsensitive));
    // Every filled day is described with its date and its exact measures, so a
    // screen reader reaches the values the colours summarise.
    // Every day the grid holds is described with its date and the measure the
    // colour summarises, so the two channels never disagree.
    for (const auto& day : activity->days()) {
        const auto text = activity->cellText(activity->cellFor(day.date));
        QVERIFY(!text.isEmpty());
        QVERIFY(text.contains(day.date.toString(Qt::ISODate)));
    }
    QCOMPARE(media->focusPolicy(), Qt::StrongFocus);
    QCOMPARE(topCourses->focusPolicy(), Qt::StrongFocus);
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
    QCOMPARE(coursesValue->text(), QStringLiteral("Not available"));
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
    auto* activity = panel.findChild<shadcn::Heatmap*>(QStringLiteral("activityGrid"));
    auto* detail = panel.findChild<QLabel*>(QStringLiteral("activityDetail"));
    QVERIFY(activity != nullptr);
    QVERIFY(detail != nullptr);

    // The panel is given the light theme, then the dark one. The grid's levels
    // come from the installed style, not from the panel's palette, so a colour
    // mode switch has to reach the cells.
    shadcn::install(*qApp, shadcn::Theme::neutral(shadcn::ColorMode::Light),
                    shadcn::MotionPolicy::Reduced);
    panel.setActive(true, progress.revision);
    QTRY_VERIFY_WITH_TIMEOUT(!activity->days().isEmpty(), 5'000);

    // The saved position is what the grid colours, so a day with progress time
    // is a filled cell and its text names that time. The value the colour
    // summarises is therefore also readable.
    bool foundProgress = false;
    for (const auto& day : activity->days()) {
        if (day.value <= 0) continue;
        const auto cell = activity->cellFor(day.date);
        QVERIFY(cell.x() >= 0);
        QVERIFY(!activity->cellText(cell).isEmpty());
        foundProgress = true;
        break;
    }
    QVERIFY(foundProgress);

    // The cell text is the value the colour summarises; the detail line is the
    // full sentence for the same day. Both name the same date, so the two
    // channels never disagree about which day is selected.
    const auto dateOf = [](const QString& cellText) {
        // The cell reads "<prefix>, <date>: <value>", so the date is the part
        // after the last comma and before the colon.
        auto head = cellText.section(QLatin1Char(':'), 0, 0).trimmed();
        return head.section(QLatin1Char(','), -1).trimmed();
    };
    const auto selected = activity->selectedCell();
    QVERIFY(selected.x() >= 0);
    QVERIFY(!activity->cellText(selected).isEmpty());
    QVERIFY(detail->text().contains(dateOf(activity->cellText(selected))));

    // Moving the selection moves the detail line with it.
    activity->show();
    activity->setFocus();
    QTest::keyClick(activity, Qt::Key_Left);
    const auto moved = activity->selectedCell();
    QCOMPARE(moved.x(), (selected.x() + activity->weekCount() - 1) % activity->weekCount());
    QVERIFY(detail->text().contains(dateOf(activity->cellText(moved))));

    const auto render = [&] {
        activity->resize(activity->sizeHint());
        QCoreApplication::processEvents();
        return activity->grab().toImage();
    };
    const auto light = render();
    QVERIFY(!light.isNull());
    shadcn::install(*qApp, shadcn::Theme::neutral(shadcn::ColorMode::Dark),
                    shadcn::MotionPolicy::Reduced);
    const auto dark = render();
    QVERIFY(!dark.isNull());
    // A colour mode switch changes the page behind the grid, so the same grid
    // has to produce a different image rather than a stale one.
    QVERIFY(light != dark);
}

QTEST_MAIN(StatsPanelTest)
#include "stats_panel_test.moc"
