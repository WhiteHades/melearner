#include "course_outline_model.hpp"
#include "library.hpp"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <QtTest/QAbstractItemModelTester>

namespace {

using melearner::CourseOutlineModel;
using melearner::library::CoursePage;
using melearner::library::Library;

bool writeFixture(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
}

bool waitFor(QSignalSpy& spy, int timeout = 10'000) {
    return !spy.isEmpty() || spy.wait(timeout);
}

bool prepareLibrary(Library& library, const QString& root, QString* courseId) {
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanned(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);

    if (library.open() == 0 || !waitFor(opened) || library.scan(root) == 0 || !waitFor(scanned)
        || library.courses() == 0 || !waitFor(coursesReady)) {
        return false;
    }
    const auto courses = qvariant_cast<CoursePage>(coursesReady.takeFirst().at(1));
    if (courses.rows.size() != 1) {
        return false;
    }
    *courseId = courses.rows.front().id;
    return true;
}

void createFixture(const QString& root) {
    const auto course = root + QStringLiteral("/Course");
    QVERIFY(QDir().mkpath(course));
    for (int section = 0; section < 513; ++section) {
        const auto sectionPath = course + QStringLiteral("/Section %1").arg(section, 3, 10, QChar('0'));
        QVERIFY(QDir().mkpath(sectionPath));
        const auto lessonCount = section == 0 ? 1025 : 1;
        for (int lesson = 0; lesson < lessonCount; ++lesson) {
            const auto name = section == 0 && lesson == 0
                ? QStringLiteral("0000 <Lesson>.txt")
                : QStringLiteral("%1 Lesson.txt").arg(lesson, 4, 10, QChar('0'));
            QVERIFY(writeFixture(sectionPath + QLatin1Char('/') + name));
        }
    }
}

}  // namespace

class CourseOutlineModelTest final : public QObject {
    Q_OBJECT

private slots:
    void pagesStayBoundedAndChildCountsStayStable();
    void revealUsesOffsetsAndIgnoresStaleResolution();
};

void CourseOutlineModelTest::pagesStayBoundedAndChildCountsStayStable() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    createFixture(root);

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QString courseId;
    QVERIFY(prepareLibrary(library, root, &courseId));

    CourseOutlineModel empty(library);
    empty.setCourse({});
    QVERIFY(!empty.canFetchMore({}));

    CourseOutlineModel model(library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCourse(courseId);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 513, 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(!model.index(0, 0).data(Qt::UserRole).toString().isEmpty(), 10'000);

    const auto firstSection = model.index(0, 0);
    QCOMPARE(model.rowCount(firstSection), 1025);
    QVERIFY(!model.index(0, 0).data(Qt::DecorationRole).isValid());
    QVERIFY(model.index(0, 0).data(Qt::FontRole).value<QFont>().bold());

    for (const int row : {128, 256, 384, 512}) {
        const auto section = model.index(row, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!section.data(Qt::UserRole).toString().isEmpty(), 10'000);
    }
    QCOMPARE(model.rowCount(model.index(128, 0)), 1);
    QCOMPARE_LE(model.cachedSectionPages(), 4);
    QCOMPARE(model.rowCount(firstSection), 1025);

    for (const int row : {0, 256, 512, 768, 1024}) {
        const auto lesson = model.index(row, 0, firstSection);
        QTRY_VERIFY_WITH_TIMEOUT(!lesson.data(Qt::UserRole).toString().isEmpty(), 10'000);
    }
    QCOMPARE_LE(model.cachedLessonPages(), 4);
    QVERIFY(!model.canFetchMore(firstSection));
    QVERIFY(!model.canFetchMore({}));

    QTRY_VERIFY_WITH_TIMEOUT(
        model.index(0, 0, firstSection).data(Qt::ToolTipRole).toString().contains(QStringLiteral("&lt;Lesson&gt;")),
        10'000);
}

void CourseOutlineModelTest::revealUsesOffsetsAndIgnoresStaleResolution() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    createFixture(root);

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QString courseId;
    QVERIFY(prepareLibrary(library, root, &courseId));

    CourseOutlineModel model(library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    QSignalSpy revealed(&model, &CourseOutlineModel::lessonRevealed);
    model.setCourse(courseId);
    QTRY_COMPARE_WITH_TIMEOUT(model.rowCount(), 513, 10'000);
    const auto firstSection = model.index(0, 0);
    const auto secondSection = model.index(400, 0);
    QTRY_VERIFY_WITH_TIMEOUT(!firstSection.data(Qt::UserRole).toString().isEmpty(), 10'000);
    QTRY_VERIFY_WITH_TIMEOUT(!secondSection.data(Qt::UserRole).toString().isEmpty(), 10'000);

    const auto firstLessonIndex = model.index(700, 0, firstSection);
    QTRY_VERIFY_WITH_TIMEOUT(!firstLessonIndex.data(Qt::UserRole).toString().isEmpty(), 10'000);
    const auto secondLessonIndex = model.index(0, 0, firstSection);
    QTRY_VERIFY_WITH_TIMEOUT(!secondLessonIndex.data(Qt::UserRole).toString().isEmpty(), 10'000);
    const auto firstLesson = model.lesson(firstLessonIndex);
    const auto secondLesson = model.lesson(secondLessonIndex);
    QVERIFY(firstLesson.has_value());
    QVERIFY(secondLesson.has_value());

    model.revealLesson(*firstLesson);
    model.revealLesson(*secondLesson);
    QTRY_COMPARE_WITH_TIMEOUT(revealed.size(), 1, 10'000);
    const auto revealedIndex = qvariant_cast<QModelIndex>(revealed.first().at(0));
    QVERIFY(revealedIndex.isValid());
    QCOMPARE(model.lesson(revealedIndex)->id, secondLesson->id);
    QCOMPARE(revealedIndex.parent().row(), 0);
    QCOMPARE(revealedIndex.row(), 0);

    const auto farLessonIndex = model.index(0, 0, secondSection);
    QTRY_VERIFY_WITH_TIMEOUT(!farLessonIndex.data(Qt::UserRole).toString().isEmpty(), 10'000);
    const auto farLesson = model.lesson(farLessonIndex);
    QVERIFY(farLesson.has_value());
    model.revealLesson(*farLesson);
    QTRY_COMPARE_WITH_TIMEOUT(revealed.size(), 2, 10'000);
    const auto farRevealed = qvariant_cast<QModelIndex>(revealed.last().at(0));
    QCOMPARE(farRevealed.parent().row(), 400);
    QCOMPARE(farRevealed.row(), 0);

    model.revealLesson(*firstLesson);
    QTRY_COMPARE_WITH_TIMEOUT(revealed.size(), 3, 10'000);
    const auto offsetRevealed = qvariant_cast<QModelIndex>(revealed.last().at(0));
    QCOMPARE(offsetRevealed.parent().row(), 0);
    QCOMPARE(offsetRevealed.row(), 700);

    QSignalSpy progressSaved(&library, &Library::progressSaved);
    QVERIFY(library.saveProgress(secondLesson->id, 7'000, 10'000, true) != 0);
    QVERIFY(waitFor(progressSaved));
    QTRY_VERIFY_WITH_TIMEOUT(model.lesson(secondLessonIndex)->completed, 10'000);
    QVERIFY(model.index(0, 0, firstSection).data(Qt::DisplayRole).toString().contains(QStringLiteral("Complete")));
}

QTEST_GUILESS_MAIN(CourseOutlineModelTest)
#include "course_outline_model_test.moc"
