#include "course_outline_model.hpp"
#include "library.hpp"

#include <QAbstractItemModelTester>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <memory>

namespace {

using melearner::CourseOutlineModel;
using melearner::library::CoursePage;
using melearner::library::Library;

/// Scans a tree of files and returns the one Course it holds, by id.
[[nodiscard]] QString scannedCourseId(Library& library, const QString& root) {
    QSignalSpy scanned(&library, &Library::scanFinished);
    if (library.open() == 0 || library.scan(root) == 0 || !scanned.wait(120'000)) {
        return {};
    }
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    if (library.courses() == 0 || !coursesReady.wait(20'000)) {
        return {};
    }
    const auto courses = qvariant_cast<CoursePage>(coursesReady.takeFirst().at(1));
    return courses.rows.size() == 1 ? courses.rows.front().id : QString();
}

void writeFile(const QString& path) {
    QFile file(path);
    if (file.open(QIODevice::WriteOnly)) {
        file.write("A local lesson.\n");
    }
}

}  // namespace

class CourseOutlineModelTest final : public QObject {
    Q_OBJECT

private slots:
    void filesNestUnderTheVideoTheyBelongTo();
    void aDocumentWithNoVideoStandsAlone();
    void aFileIsAChildOfItsVideoAndNothingElse();
    void revealingAFileExpandsItsVideo();
    void lessonPagesStayPaged();
    void aVideoWithNoFilesIsOneLeafRow();
};

/// A file that ships with a video is shown under it, not beside it.
///
/// A lecture and its handout are one name with two extensions, so they are one
/// thing to a reader. Listed as peers they read as two lessons and the reader has
/// to work out which document belongs to which video; nested, the handout is where
/// it was left.
void CourseOutlineModelTest::filesNestUnderTheVideoTheyBelongTo() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(root));
    writeFile(root + QStringLiteral("/01 Intro.mp4"));
    writeFile(root + QStringLiteral("/01 Intro.pdf"));
    writeFile(root + QStringLiteral("/01 Intro handout.docx"));
    writeFile(root + QStringLiteral("/02 Second.mp4"));

    auto library = std::make_unique<Library>(files.path() + QStringLiteral("/library.sqlite3"));
    const auto courseId = scannedCourseId(*library, files.path());
    QVERIFY(!courseId.isEmpty());

    CourseOutlineModel model(*library);
    model.setCourse(courseId);
    QTRY_COMPARE(model.rowCount(), 1);
    const auto section = model.index(0, 0);
    QTRY_COMPARE(model.rowCount(section), 2);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    const auto first = model.index(0, 0, section);
    QVERIFY(first.isValid());
    // The rows are known from the Section, but their names arrive with the first
    // Lesson page, so a name is waited for rather than read.
    QTRY_COMPARE(first.data(Qt::DisplayRole).toString(), QStringLiteral("01 Intro"));
    // The video has files, so it is expandable and its files are addressable.
    QVERIFY(model.hasChildren(first));
    QCOMPARE(model.rowCount(first), 2);
    QTRY_COMPARE(model.data(model.index(0, 0, first), Qt::DisplayRole).toString(), QStringLiteral("01 Intro"));
    QTRY_COMPARE(model.data(model.index(1, 0, first), Qt::DisplayRole).toString(), QStringLiteral("01 Intro handout"));

    const auto second = model.index(1, 0, section);
    QVERIFY(second.isValid());
    QTRY_COMPARE(second.data(Qt::DisplayRole).toString(), QStringLiteral("02 Second"));
    // A video with nothing beside it is a leaf, and says so rather than offering an
    // expander that opens onto nothing.
    QVERIFY(!model.hasChildren(second));
    QCOMPARE(model.rowCount(second), 0);
}

/// A document with no video of the same name is a document in its own right.
void CourseOutlineModelTest::aDocumentWithNoVideoStandsAlone() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(root));
    writeFile(root + QStringLiteral("/Syllabus.pdf"));
    writeFile(root + QStringLiteral("/01 Intro.mp4"));

    auto library = std::make_unique<Library>(files.path() + QStringLiteral("/library.sqlite3"));
    const auto courseId = scannedCourseId(*library, files.path());
    QVERIFY(!courseId.isEmpty());

    CourseOutlineModel model(*library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCourse(courseId);
    QTRY_COMPARE(model.rowCount(), 1);
    const auto section = model.index(0, 0);
    QTRY_COMPARE(model.rowCount(section), 2);
    // Both are their own row: neither nests, because neither has a video above it.
    for (int row = 0; row < 2; ++row) {
        QVERIFY(!model.hasChildren(model.index(row, 0, section)));
    }
}

/// The tree is a tree. A file's parent is its video, and a video's parent is the
/// Section. Getting this wrong makes a view indent rows that have no row above
/// them, which reads as a file that belongs to nothing.
void CourseOutlineModelTest::aFileIsAChildOfItsVideoAndNothingElse() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(root));
    writeFile(root + QStringLiteral("/01 Intro.mp4"));
    writeFile(root + QStringLiteral("/01 Intro.pdf"));
    writeFile(root + QStringLiteral("/02 Second.mp4"));
    writeFile(root + QStringLiteral("/02 Second.pdf"));

    auto library = std::make_unique<Library>(files.path() + QStringLiteral("/library.sqlite3"));
    const auto courseId = scannedCourseId(*library, files.path());
    QVERIFY(!courseId.isEmpty());

    CourseOutlineModel model(*library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCourse(courseId);
    QTRY_COMPARE(model.rowCount(), 1);
    const auto section = model.index(0, 0);
    QTRY_COMPARE(model.rowCount(section), 2);

    for (int group = 0; group < 2; ++group) {
        const auto video = model.index(group, 0, section);
        QVERIFY(video.isValid());
        QCOMPARE(model.parent(video), section);
        const auto file = model.index(0, 0, video);
        QVERIFY(file.isValid());
        QCOMPARE(model.parent(file), video);
        // Going back up from a file has to arrive at the same video, not at a
        // different row that happens to share its name.
        QCOMPARE(model.parent(model.parent(file)), section);
        // A file opens like any other Lesson, so it resolves to one.
        QTRY_VERIFY(model.lesson(file).has_value());
        QCOMPARE(model.lesson(file)->type, QStringLiteral("document"));
        QCOMPARE(model.lesson(file)->name, video.data(Qt::DisplayRole).toString());
        // And a file is a leaf, so it is not expandable.
        QVERIFY(!model.hasChildren(file));
        QCOMPARE(model.rowCount(file), 0);
    }
}

/// Revealing a handout has to open the video it sits under, or the reader is sent
/// to a file they cannot see.
void CourseOutlineModelTest::revealingAFileExpandsItsVideo() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(root));
    writeFile(root + QStringLiteral("/01 Intro.mp4"));
    writeFile(root + QStringLiteral("/01 Intro.pdf"));

    auto library = std::make_unique<Library>(files.path() + QStringLiteral("/library.sqlite3"));
    const auto courseId = scannedCourseId(*library, files.path());
    QVERIFY(!courseId.isEmpty());

    CourseOutlineModel model(*library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCourse(courseId);
    QTRY_COMPARE(model.rowCount(), 1);
    const auto section = model.index(0, 0);
    QTRY_COMPARE(model.rowCount(section), 1);
    const auto video = model.index(0, 0, section);
    QTRY_COMPARE(model.rowCount(video), 1);
    const auto file = model.index(0, 0, video);
    QTRY_COMPARE(file.data(Qt::DisplayRole).toString(), QStringLiteral("01 Intro"));

    QSignalSpy expanded(&model, &CourseOutlineModel::videoExpanded);
    QSignalSpy revealed(&model, &CourseOutlineModel::lessonRevealed);
    model.revealLesson(*model.lesson(file));
    QTRY_COMPARE(expanded.count(), 1);
    QCOMPARE(expanded.first().at(0).value<QModelIndex>(), video);
    QTRY_COMPARE(revealed.count(), 1);
    QCOMPARE(revealed.first().at(0).value<QModelIndex>(), file);
}

/// A Section with more Lessons than one page holds still gets its rows from
/// metadata, so opening it does not read every Lesson.
void CourseOutlineModelTest::lessonPagesStayPaged() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto course = files.path() + QStringLiteral("/Course");
    QVERIFY(QDir().mkpath(course));
    // More than one page of Lessons, so a model that read them all to group them
    // would show it.
    const auto sectionPath = course + QStringLiteral("/Section");
    QVERIFY(QDir().mkpath(sectionPath));
    for (int lesson = 0; lesson < 700; ++lesson) {
        writeFile(sectionPath
            + QStringLiteral("/%1 Lesson.mp4").arg(lesson, 4, 10, QChar('0')));
    }

    auto library = std::make_unique<Library>(files.path() + QStringLiteral("/library.sqlite3"));
    const auto courseId = scannedCourseId(*library, files.path());
    QVERIFY(!courseId.isEmpty());

    CourseOutlineModel model(*library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCourse(courseId);
    QTRY_COMPARE(model.rowCount(), 1);
    const auto section = model.index(0, 0);
    // The row count arrives with the Section, before any Lesson page is read.
    QCOMPARE(model.rowCount(section), 700);
    QCOMPARE(model.cachedLessonPages(), 0);
    QCOMPARE(model.cachedSectionPages(), 1);
}

/// A video with nothing beside it is a leaf, and says so.
void CourseOutlineModelTest::aVideoWithNoFilesIsOneLeafRow() {
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const auto root = files.path() + QStringLiteral("/Course/Section");
    QVERIFY(QDir().mkpath(root));
    writeFile(root + QStringLiteral("/01 Intro.mp4"));
    writeFile(root + QStringLiteral("/02 Second.mp4"));

    auto library = std::make_unique<Library>(files.path() + QStringLiteral("/library.sqlite3"));
    const auto courseId = scannedCourseId(*library, files.path());
    QVERIFY(!courseId.isEmpty());

    CourseOutlineModel model(*library);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::Fatal);
    model.setCourse(courseId);
    QTRY_COMPARE(model.rowCount(), 1);
    const auto section = model.index(0, 0);
    QTRY_COMPARE(model.rowCount(section), 2);
    for (int row = 0; row < 2; ++row) {
        const auto video = model.index(row, 0, section);
        QVERIFY(video.isValid());
        // No expander on a video with nothing under it, rather than an expander that
        // opens onto nothing.
        QVERIFY(!model.hasChildren(video));
        QCOMPARE(model.rowCount(video), 0);
        QVERIFY(!model.index(0, 0, video).isValid());
    }
}

QTEST_GUILESS_MAIN(CourseOutlineModelTest)
#include "course_outline_model_test.moc"
