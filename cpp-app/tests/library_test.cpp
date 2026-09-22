#include "library.hpp"

#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <sqlite3.h>

using melearner::library::CoursePage;
using melearner::library::CourseEntry;
using melearner::library::ActivityDayPage;
using melearner::library::ErrorCode;
using melearner::library::LessonPage;
using melearner::library::Library;
using melearner::library::ProgressResult;
using melearner::library::ResumePage;
using melearner::library::LibraryStats;
using melearner::library::SearchPage;
using melearner::library::SearchResolution;
using melearner::library::ScanResult;
using melearner::library::SectionPage;
using melearner::library::Startup;

namespace {

void writeFile(const QString& path, QByteArray contents = QByteArrayLiteral("fixture")) {
    QFile file(path);
    QVERIFY2(file.open(QIODevice::WriteOnly), qPrintable(file.errorString()));
    QCOMPARE(file.write(contents), static_cast<qint64>(contents.size()));
}

bool executeSqlite(const QString& databasePath, const QString& sql, QString* errorMessage = nullptr) {
    sqlite3* database = nullptr;
    const auto databaseBytes = databasePath.toUtf8();
    if (sqlite3_open_v2(databaseBytes.constData(), &database, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        if (errorMessage != nullptr) {
            *errorMessage = database == nullptr
                ? QStringLiteral("cannot open SQLite database")
                : QString::fromUtf8(sqlite3_errmsg(database));
        }
        if (database != nullptr) {
            sqlite3_close(database);
        }
        return false;
    }
    const auto sqlBytes = sql.toUtf8();
    char* error = nullptr;
    const auto result = sqlite3_exec(database, sqlBytes.constData(), nullptr, nullptr, &error);
    if (result != SQLITE_OK && errorMessage != nullptr) {
        *errorMessage = error == nullptr ? QStringLiteral("SQLite statement failed") : QString::fromUtf8(error);
    }
    sqlite3_free(error);
    sqlite3_close(database);
    return result == SQLITE_OK;
}

bool waitFor(QSignalSpy& spy, int timeoutMs) {
    return !spy.isEmpty() || spy.wait(timeoutMs);
}

bool openLibrary(Library& library, QSignalSpy& opened, Startup* result) {
    if (library.open() == 0 || !waitFor(opened, 5'000)) {
        return false;
    }
    *result = qvariant_cast<Startup>(opened.takeFirst().at(1));
    return true;
}

bool scanLibrary(Library& library, QSignalSpy& scanFinished, const QString& root, ScanResult* result) {
    if (library.scan(root) == 0 || !waitFor(scanFinished, 10'000)) {
        return false;
    }
    *result = qvariant_cast<ScanResult>(scanFinished.takeFirst().at(1));
    return true;
}

bool readCourses(Library& library, QSignalSpy& coursesReady, CoursePage* result) {
    if (library.courses() == 0 || !waitFor(coursesReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<CoursePage>(coursesReady.takeFirst().at(1));
    return true;
}

bool readSections(
    Library& library,
    QSignalSpy& sectionsReady,
    const QString& courseId,
    std::uint64_t offset,
    std::uint64_t limit,
    SectionPage* result) {
    if (library.sections(courseId, offset, limit) == 0 || !waitFor(sectionsReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<SectionPage>(sectionsReady.takeFirst().at(1));
    return true;
}

bool readLessons(Library& library, QSignalSpy& lessonsReady, const QString& courseId, LessonPage* result) {
    if (library.lessons(courseId) == 0 || !waitFor(lessonsReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<LessonPage>(lessonsReady.takeFirst().at(1));
    return true;
}

bool readSectionLessons(
    Library& library,
    QSignalSpy& lessonsReady,
    const QString& courseId,
    const QString& sectionId,
    std::uint64_t offset,
    std::uint64_t limit,
    LessonPage* result) {
    if (library.sectionLessons(courseId, sectionId, offset, limit) == 0 || !waitFor(lessonsReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<LessonPage>(lessonsReady.takeFirst().at(1));
    return true;
}

bool readCourseEntry(
    Library& library,
    QSignalSpy& courseEntered,
    const QString& courseId,
    const QString& requestedLessonId,
    CourseEntry* result) {
    if (library.enterCourse(courseId, requestedLessonId) == 0 || !waitFor(courseEntered, 5'000)) {
        return false;
    }
    *result = qvariant_cast<CourseEntry>(courseEntered.takeFirst().at(1));
    return true;
}

bool readResume(
    Library& library,
    QSignalSpy& resumeReady,
    std::uint64_t offset,
    std::uint64_t limit,
    ResumePage* result) {
    if (library.resume(offset, limit) == 0 || !waitFor(resumeReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<ResumePage>(resumeReady.takeFirst().at(1));
    return true;
}

bool readStats(Library& library, QSignalSpy& statsReady, std::uint64_t revision, LibraryStats* result) {
    if (library.stats(revision) == 0 || !waitFor(statsReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<LibraryStats>(statsReady.takeFirst().at(1));
    return true;
}

bool readActivity(
    Library& library,
    QSignalSpy& activityReady,
    std::uint64_t revision,
    std::uint64_t offset,
    std::uint64_t limit,
    ActivityDayPage* result) {
    if (library.activity(revision, offset, limit) == 0 || !waitFor(activityReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<ActivityDayPage>(activityReady.takeFirst().at(1));
    return true;
}

bool readSearch(
    Library& library,
    QSignalSpy& searchReady,
    const QString& query,
    std::uint64_t offset,
    std::uint64_t limit,
    SearchPage* result) {
    if (library.search(query, offset, limit) == 0 || !waitFor(searchReady, 5'000)) {
        return false;
    }
    *result = qvariant_cast<SearchPage>(searchReady.takeFirst().at(1));
    return true;
}

}  // namespace

class LibraryTest final : public QObject {
    Q_OBJECT

private slots:
    void scansIntoPagedCourseAndLessonRows();
    void readsLastCommitWhileLargeScanDiscovers();
    void cancellationPreservesLastCommitAndHasOneTerminal();
    void markerConflictIsPreserved();
    void discoveryLimitPreservesLastCommit();
    void readsBoundedSectionOutlineAndScopedLessons();
    void courseEntryAndResumePersist();
    void statsAndActivityUseCanonicalScope();
    void progressReopensWithAtomicActivityInputs();
    void immediateCloseFlushesProgress();
    void markerAndMissingCourseIdentitySurviveRescan();
    void searchesPagedNamesAndResolvesMissingState();
};

void LibraryTest::scansIntoPagedCourseAndLessonRows() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course 10");
    const auto section = course + QStringLiteral("/Section 2");
    QVERIFY(QDir().mkpath(section));
    writeFile(section + QStringLiteral("/Lesson 10.mp4"));
    writeFile(section + QStringLiteral("/Lesson 2.mp4"));
    writeFile(section + QStringLiteral("/Reference.epub"));
    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));

    Library library(database);
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    QSignalSpy lessonsReady(&library, &Library::lessonsReady);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    QVERIFY(startup.root.path.isEmpty());
    QCOMPARE(startup.settings.appearance, QStringLiteral("light"));

    ScanResult scan;
    QVERIFY(scanLibrary(library, scanFinished, root, &scan));
    QCOMPARE(scan.courses, std::uint64_t{1});
    QCOMPARE(scan.lessons, std::uint64_t{3});
    QVERIFY(scan.warnings.isEmpty());
    QVERIFY(QFileInfo::exists(course + QStringLiteral("/.melearner-course.json")));

    CoursePage page;
    QVERIFY(readCourses(library, coursesReady, &page));
    QCOMPARE(page.total, std::uint64_t{1});
    QCOMPARE(page.rows.size(), 1);
    QCOMPARE(page.rows.front().name, QStringLiteral("Course 10"));
    QCOMPARE(page.rows.front().lessonCount, std::uint64_t{3});

    LessonPage lessons;
    QVERIFY(readLessons(library, lessonsReady, page.rows.front().id, &lessons));
    QCOMPARE(lessons.total, std::uint64_t{3});
    QCOMPARE(lessons.rows.size(), 3);
    QCOMPARE(lessons.rows.at(0).name, QStringLiteral("Lesson 2"));
    QCOMPARE(lessons.rows.at(0).sectionName, QStringLiteral("Section 2"));
    QCOMPARE(lessons.rows.at(0).type, QStringLiteral("video"));
    QCOMPARE(lessons.rows.at(1).name, QStringLiteral("Lesson 10"));
    QCOMPARE(lessons.rows.at(2).name, QStringLiteral("Reference"));
    QCOMPARE(lessons.rows.at(2).type, QStringLiteral("document"));
}

void LibraryTest::readsBoundedSectionOutlineAndScopedLessons() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course Outline");
    const auto firstSection = course + QStringLiteral("/Section 2");
    const auto secondSection = course + QStringLiteral("/Section 10");
    QVERIFY(QDir().mkpath(firstSection));
    QVERIFY(QDir().mkpath(secondSection));
    writeFile(firstSection + QStringLiteral("/Lesson 1.mp4"));
    writeFile(secondSection + QStringLiteral("/Lesson 10.mp4"));
    writeFile(secondSection + QStringLiteral("/Lesson 2.mp4"));

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy sectionsReady(&library, &Library::sectionsReady);
    QSignalSpy lessonsReady(&library, &Library::lessonsReady);
    QSignalSpy searchResolved(&library, &Library::searchResolved);
    QSignalSpy failed(&library, &Library::failed);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    ScanResult scan;
    QVERIFY(scanLibrary(library, scanFinished, root, &scan));
    QCOMPARE(scan.courses, std::uint64_t{1});
    QCOMPARE(scan.lessons, std::uint64_t{3});

    QSignalSpy coursesReady(&library, &Library::coursesReady);
    CoursePage courses;
    QVERIFY(readCourses(library, coursesReady, &courses));
    const auto courseId = courses.rows.front().id;

    SectionPage firstPage;
    QVERIFY(readSections(library, sectionsReady, courseId, 0, 1, &firstPage));
    QCOMPARE(firstPage.courseId, courseId);
    QCOMPARE(firstPage.offset, std::uint64_t{0});
    QCOMPARE(firstPage.total, std::uint64_t{2});
    QCOMPARE(firstPage.rows.size(), 1);
    QVERIFY(firstPage.hasMore);
    QCOMPARE(firstPage.rows.front().name, QStringLiteral("Section 2"));
    QCOMPARE(firstPage.rows.front().lessonCount, std::uint64_t{1});

    SectionPage secondPage;
    QVERIFY(readSections(library, sectionsReady, courseId, 1, 128, &secondPage));
    QCOMPARE(secondPage.rows.size(), 1);
    QVERIFY(!secondPage.hasMore);
    QCOMPARE(secondPage.rows.front().name, QStringLiteral("Section 10"));
    QCOMPARE(secondPage.rows.front().orderIndex, std::uint64_t{1});

    LessonPage scopedLessons;
    QVERIFY(readSectionLessons(
        library,
        lessonsReady,
        courseId,
        firstPage.rows.front().id,
        0,
        500,
        &scopedLessons));
    QCOMPARE(scopedLessons.courseId, courseId);
    QCOMPARE(scopedLessons.sectionId, firstPage.rows.front().id);
    QCOMPARE(scopedLessons.total, std::uint64_t{1});
    QCOMPARE(scopedLessons.rows.size(), 1);
    QCOMPARE(scopedLessons.rows.front().name, QStringLiteral("Lesson 1"));

    LessonPage flatLessons;
    QVERIFY(readLessons(library, lessonsReady, courseId, &flatLessons));
    QVERIFY(flatLessons.sectionId.isEmpty());
    QCOMPARE(flatLessons.total, std::uint64_t{3});

    QVERIFY(library.resolveLesson(
                 courseId,
                 firstPage.rows.front().id,
                 scopedLessons.rows.front().id)
            != 0);
    QVERIFY(waitFor(searchResolved, 5'000));
    const auto resolution = qvariant_cast<SearchResolution>(searchResolved.takeFirst().at(1));
    QCOMPARE(resolution.kind, QStringLiteral("lesson"));
    QCOMPARE(resolution.course.id, courseId);
    QCOMPARE(resolution.sectionId, firstPage.rows.front().id);
    QCOMPARE(resolution.lesson.id, scopedLessons.rows.front().id);
    QCOMPARE(resolution.sectionOffset, std::uint64_t{0});
    QCOMPARE(resolution.sectionLessonOffset, std::uint64_t{0});
    QCOMPARE(resolution.lessonOffset, std::uint64_t{0});

    QVERIFY(library.resolveLesson(
                 courseId,
                 secondPage.rows.front().id,
                 flatLessons.rows.at(1).id)
            != 0);
    QVERIFY(waitFor(searchResolved, 5'000));
    const auto secondResolution = qvariant_cast<SearchResolution>(searchResolved.takeFirst().at(1));
    QCOMPARE(secondResolution.sectionOffset, std::uint64_t{1});
    QCOMPARE(secondResolution.sectionLessonOffset, std::uint64_t{0});
    QCOMPARE(secondResolution.lessonOffset, std::uint64_t{1});

    failed.clear();
    QVERIFY(library.resolveLesson(
                 courseId,
                 secondPage.rows.front().id,
                 scopedLessons.rows.front().id)
            != 0);
    QVERIFY(waitFor(failed, 5'000));
    QCOMPARE(
        qvariant_cast<melearner::library::Error>(failed.takeFirst().at(1)).code,
        ErrorCode::invalid_request);
}

void LibraryTest::readsLastCommitWhileLargeScanDiscovers() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const auto committedRoot = temporary.filePath(QStringLiteral("committed-root"));
    const auto committedCourse = committedRoot + QStringLiteral("/Committed Course");
    QVERIFY(QDir().mkpath(committedCourse));
    writeFile(committedCourse + QStringLiteral("/intro.mp4"));

    const auto largeRoot = temporary.filePath(QStringLiteral("large-root"));
    const auto largeCourse = largeRoot + QStringLiteral("/Incoming Course");
    const auto largeSection = largeCourse + QStringLiteral("/Lessons");
    QVERIFY(QDir().mkpath(largeSection));
    constexpr int largeLessonCount = 20'000;
    for (int index = 0; index < largeLessonCount; ++index) {
        writeFile(largeSection + QStringLiteral("/Lesson %1.mp4").arg(index));
    }

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));

    ScanResult committedScan;
    QVERIFY(scanLibrary(library, scanFinished, committedRoot, &committedScan));
    CoursePage committedCourses;
    QVERIFY(readCourses(library, coursesReady, &committedCourses));
    QVERIFY(!committedCourses.rows.isEmpty());
    const auto committedCourseId = committedCourses.rows.front().id;
    const auto committedRevision = committedCourses.revision;

    scanFinished.clear();
    coursesReady.clear();
    const auto scanRequestId = library.scan(largeRoot);
    QVERIFY(scanRequestId != 0);

    // Enqueue the read before discovery can finish. The large fixture keeps this
    // request in the discovery window instead of making it a post-commit read.
    const auto readRequestId = library.courses();
    QVERIFY(readRequestId != 0);
    QVERIFY(waitFor(coursesReady, 5'000));
    QVERIFY(scanFinished.isEmpty());
    const auto duringScan = qvariant_cast<CoursePage>(coursesReady.takeFirst().at(1));
    QCOMPARE(duringScan.revision, committedRevision);
    QCOMPARE(duringScan.total, std::uint64_t{1});
    QCOMPARE(duringScan.rows.front().id, committedCourseId);
    QCOMPARE(coursesReady.count(), 0);
    QVERIFY(scanRequestId != readRequestId);

    QVERIFY(waitFor(scanFinished, 15'000));
    QCOMPARE(qvariant_cast<ScanResult>(scanFinished.takeFirst().at(1)).lessons,
             static_cast<std::uint64_t>(largeLessonCount));
}

void LibraryTest::cancellationPreservesLastCommitAndHasOneTerminal() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const auto committedRoot = temporary.filePath(QStringLiteral("committed-root"));
    const auto committedCourse = committedRoot + QStringLiteral("/Committed Course");
    QVERIFY(QDir().mkpath(committedCourse));
    writeFile(committedCourse + QStringLiteral("/intro.mp4"));

    const auto largeRoot = temporary.filePath(QStringLiteral("cancel-root"));
    const auto largeCourse = largeRoot + QStringLiteral("/Incoming Course");
    const auto largeSection = largeCourse + QStringLiteral("/Lessons");
    QVERIFY(QDir().mkpath(largeSection));
    constexpr int largeLessonCount = 20'000;
    for (int index = 0; index < largeLessonCount; ++index) {
        writeFile(largeSection + QStringLiteral("/Lesson %1.mp4").arg(index));
    }

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy scanProgress(&library, &Library::scanProgress);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    QSignalSpy failed(&library, &Library::failed);
    QSignalSpy settingsSaved(&library, &Library::settingsSaved);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));

    ScanResult committedScan;
    QVERIFY(scanLibrary(library, scanFinished, committedRoot, &committedScan));
    CoursePage committedCourses;
    QVERIFY(readCourses(library, coursesReady, &committedCourses));
    QVERIFY(!committedCourses.rows.isEmpty());
    const auto committedCourseId = committedCourses.rows.front().id;
    const auto committedRevision = committedCourses.revision;
    const auto committedRootPath = committedScan.rootPath;

    scanFinished.clear();
    scanProgress.clear();
    failed.clear();
    const auto scanRequestId = library.scan(largeRoot);
    QVERIFY(scanRequestId != 0);
    QTRY_VERIFY_WITH_TIMEOUT(!scanProgress.isEmpty(), 5'000);
    auto queuedSettings = startup.settings;
    queuedSettings.appearance = QStringLiteral("dark");
    QVERIFY(library.setSettings(queuedSettings) != 0);
    QTest::qWait(25);
    QVERIFY(settingsSaved.isEmpty());
    QVERIFY(library.cancelScan(scanRequestId));
    QVERIFY(!library.cancelScan(scanRequestId));

    QVERIFY(waitFor(failed, 15'000));
    QCOMPARE(failed.count(), 1);
    const auto failure = qvariant_cast<melearner::library::Error>(failed.takeFirst().at(1));
    QCOMPARE(failure.code, ErrorCode::cancelled);
    QCOMPARE(failure.path, QString());
    QCOMPARE(scanFinished.count(), 0);
    QVERIFY(waitFor(settingsSaved, 5'000));
    QCOMPARE(qvariant_cast<melearner::library::Settings>(settingsSaved.takeFirst().at(1)).appearance,
             QStringLiteral("dark"));
    QTest::qWait(25);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(scanFinished.count(), 0);
    QVERIFY(!QFileInfo::exists(largeCourse + QStringLiteral("/.melearner-course.json")));

    opened.clear();
    QVERIFY(library.open() != 0);
    QVERIFY(waitFor(opened, 5'000));
    const auto afterCancel = qvariant_cast<Startup>(opened.takeFirst().at(1));
    QCOMPARE(afterCancel.revision, committedRevision);
    QCOMPARE(afterCancel.root.path, committedRootPath);
    QCOMPARE(afterCancel.settings.appearance, QStringLiteral("dark"));

    CoursePage afterCourses;
    QVERIFY(readCourses(library, coursesReady, &afterCourses));
    QCOMPARE(afterCourses.revision, committedRevision);
    QCOMPARE(afterCourses.total, std::uint64_t{1});
    QCOMPARE(afterCourses.rows.front().id, committedCourseId);
}

void LibraryTest::markerConflictIsPreserved() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course");
    QVERIFY(QDir().mkpath(course));
    writeFile(course + QStringLiteral("/lesson.mp4"));

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    ScanResult first;
    QVERIFY(scanLibrary(library, scanFinished, root, &first));

    const auto markerPath = course + QStringLiteral("/.melearner-course.json");
    QFile marker(markerPath);
    QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray foreignMarker =
        QByteArrayLiteral("{\"version\":1,\"identityId\":\"external-writer\"}");
    QCOMPARE(marker.write(foreignMarker), static_cast<qint64>(foreignMarker.size()));
    marker.close();
    QVERIFY(!QFileInfo(markerPath).isSymLink());

    ScanResult second;
    QVERIFY(scanLibrary(library, scanFinished, root, &second));
    QVERIFY(std::any_of(second.warnings.cbegin(), second.warnings.cend(), [](const QString& warning) {
        return warning.contains(QStringLiteral("Marker identity differs"));
    }));

    QFile preserved(markerPath);
    QVERIFY(preserved.open(QIODevice::ReadOnly));
    QCOMPARE(preserved.readAll(), foreignMarker);
}

void LibraryTest::discoveryLimitPreservesLastCommit() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    const auto committedRoot = temporary.filePath(QStringLiteral("committed-root"));
    const auto committedCourse = committedRoot + QStringLiteral("/Committed Course");
    QVERIFY(QDir().mkpath(committedCourse));
    writeFile(committedCourse + QStringLiteral("/intro.mp4"));

    const auto oversizedRoot = temporary.filePath(QStringLiteral("oversized-root"));
    QVERIFY(QDir().mkpath(oversizedRoot));
    constexpr int oversizedCourseCount = 10'001;
    for (int index = 0; index < oversizedCourseCount; ++index) {
        QVERIFY(QDir().mkpath(oversizedRoot + QStringLiteral("/Course %1").arg(index)));
    }

    Library library(temporary.filePath(QStringLiteral("library.sqlite3")));
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    QSignalSpy failed(&library, &Library::failed);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));

    ScanResult committedScan;
    QVERIFY(scanLibrary(library, scanFinished, committedRoot, &committedScan));
    CoursePage committedCourses;
    QVERIFY(readCourses(library, coursesReady, &committedCourses));
    QVERIFY(!committedCourses.rows.isEmpty());
    const auto committedCourseId = committedCourses.rows.front().id;
    const auto committedRevision = committedCourses.revision;

    scanFinished.clear();
    failed.clear();
    const auto scanRequestId = library.scan(oversizedRoot);
    QVERIFY(scanRequestId != 0);
    QVERIFY(waitFor(failed, 15'000));
    QCOMPARE(failed.count(), 1);
    const auto error = qvariant_cast<melearner::library::Error>(failed.takeFirst().at(1));
    QCOMPARE(error.code, ErrorCode::oversized);
    QVERIFY(error.message.contains(QStringLiteral("Course discovery limit")));
    QCOMPARE(scanFinished.count(), 0);
    QTest::qWait(25);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(scanFinished.count(), 0);

    CoursePage afterFailure;
    QVERIFY(readCourses(library, coursesReady, &afterFailure));
    QCOMPARE(afterFailure.revision, committedRevision);
    QCOMPARE(afterFailure.total, std::uint64_t{1});
    QCOMPARE(afterFailure.rows.front().id, committedCourseId);
    QVERIFY(!QFileInfo::exists(oversizedRoot + QStringLiteral("/Course 0/.melearner-course.json")));
}

void LibraryTest::courseEntryAndResumePersist() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    for (int index = 1; index <= 5; ++index) {
        const auto course = root + QStringLiteral("/Course %1").arg(index);
        QVERIFY(QDir().mkpath(course));
        writeFile(course + QStringLiteral("/Lesson 1.mp4"));
        if (index == 1) {
            writeFile(course + QStringLiteral("/Lesson 2.mp4"));
        }
    }
    QVERIFY(QDir().mkpath(root + QStringLiteral("/Empty Course")));
    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));

    Library library(database);
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    QSignalSpy lessonsReady(&library, &Library::lessonsReady);
    QSignalSpy courseEntered(&library, &Library::courseEntered);
    QSignalSpy resumeReady(&library, &Library::resumeReady);
    QSignalSpy progressSaved(&library, &Library::progressSaved);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    ScanResult scan;
    QVERIFY(scanLibrary(library, scanFinished, root, &scan));

    CoursePage courses;
    QVERIFY(readCourses(library, coursesReady, &courses));
    QCOMPARE(courses.total, std::uint64_t{6});
    auto courseByName = [&courses](const QString& name) {
        for (const auto& course : courses.rows) {
            if (course.name == name) {
                return course;
            }
        }
        return melearner::library::Course{};
    };
    const auto course1 = courseByName(QStringLiteral("Course 1"));
    const auto course2 = courseByName(QStringLiteral("Course 2"));
    const auto course3 = courseByName(QStringLiteral("Course 3"));
    QVERIFY(!course1.id.isEmpty());
    QVERIFY(!course2.id.isEmpty());
    QVERIFY(!course3.id.isEmpty());

    // With no prior access, the resume page starts at the first meaningful
    // available Course and does not include an empty Course.
    ResumePage initialResume;
    QVERIFY(readResume(library, resumeReady, 0, 100, &initialResume));
    QCOMPARE(initialResume.total, std::uint64_t{5});
    QCOMPARE(initialResume.rows.size(), 4);
    QVERIFY(initialResume.hasMore);
    QCOMPARE(initialResume.rows.front().course.name, QStringLiteral("Course 1"));
    QVERIFY(initialResume.rows.front().hasLesson);
    QCOMPARE(initialResume.rows.front().lesson.name, QStringLiteral("Lesson 1"));
    QVERIFY(!initialResume.rows.front().lesson.sectionName.isEmpty());
    QVERIFY(initialResume.rows.front().lesson.sectionName != initialResume.rows.front().lesson.name);
    QCOMPARE(initialResume.rows.front().globalLessonOffset, std::uint64_t{0});

    ResumePage finalResumePage;
    QVERIFY(readResume(library, resumeReady, 4, 100, &finalResumePage));
    QCOMPARE(finalResumePage.offset, std::uint64_t{4});
    QCOMPARE(finalResumePage.total, std::uint64_t{5});
    QCOMPARE(finalResumePage.rows.size(), 1);
    QVERIFY(!finalResumePage.hasMore);
    QCOMPARE(finalResumePage.rows.front().course.name, QStringLiteral("Course 5"));

    LessonPage course1Lessons;
    QVERIFY(readLessons(library, lessonsReady, course1.id, &course1Lessons));
    QCOMPARE(course1Lessons.total, std::uint64_t{2});
    const auto firstLessonId = course1Lessons.rows.at(0).id;
    const auto secondLessonId = course1Lessons.rows.at(1).id;

    CourseEntry firstEntry;
    QVERIFY(readCourseEntry(library, courseEntered, course1.id, {}, &firstEntry));
    QVERIFY(firstEntry.hasLesson);
    QCOMPARE(firstEntry.lesson.id, firstLessonId);
    QCOMPARE(firstEntry.lesson.name, QStringLiteral("Lesson 1"));
    QVERIFY(!firstEntry.lesson.sectionName.isEmpty());
    QVERIFY(firstEntry.lesson.sectionName != firstEntry.lesson.name);
    QCOMPARE(firstEntry.globalLessonOffset, std::uint64_t{0});
    QVERIFY(firstEntry.revision > initialResume.revision);
    QVERIFY(firstEntry.course.lastAccessed > 0);

    QVERIFY(library.saveProgress(firstLessonId, 60'000, 60'000, true) != 0);
    QVERIFY(waitFor(progressSaved, 5'000));
    progressSaved.takeFirst();

    CourseEntry firstIncomplete;
    QVERIFY(readCourseEntry(library, courseEntered, course1.id, {}, &firstIncomplete));
    QVERIFY(firstIncomplete.hasLesson);
    QCOMPARE(firstIncomplete.lesson.id, secondLessonId);
    QCOMPARE(firstIncomplete.globalLessonOffset, std::uint64_t{1});

    QVERIFY(library.saveProgress(secondLessonId, 60'000, 60'000, true) != 0);
    QVERIFY(waitFor(progressSaved, 5'000));
    progressSaved.takeFirst();

    CourseEntry completedFallback;
    QVERIFY(readCourseEntry(library, courseEntered, course1.id, {}, &completedFallback));
    QVERIFY(completedFallback.hasLesson);
    QCOMPARE(completedFallback.lesson.id, firstLessonId);
    QCOMPARE(completedFallback.globalLessonOffset, std::uint64_t{0});

    CourseEntry explicitEntry;
    QVERIFY(readCourseEntry(library, courseEntered, course1.id, secondLessonId, &explicitEntry));
    QVERIFY(explicitEntry.hasLesson);
    QCOMPARE(explicitEntry.lesson.id, secondLessonId);
    QCOMPARE(explicitEntry.globalLessonOffset, std::uint64_t{1});

    // Removing a Course retains its identity and metadata but never opens a
    // stale Lesson or updates its access timestamp.
    QVERIFY(QDir(root + QStringLiteral("/Course 2")).removeRecursively());
    ScanResult missingScan;
    QVERIFY(scanLibrary(library, scanFinished, root, &missingScan));
    CourseEntry missingEntry;
    QVERIFY(readCourseEntry(library, courseEntered, course2.id, secondLessonId, &missingEntry));
    QVERIFY(missingEntry.course.missing);
    QVERIFY(!missingEntry.hasLesson);
    QCOMPARE(missingEntry.course.lastAccessed, std::int64_t{0});

    // A later access reorders the bounded resume page and survives reopen.
    QTest::qWait(2);
    CourseEntry thirdEntry;
    QVERIFY(readCourseEntry(library, courseEntered, course3.id, {}, &thirdEntry));
    QVERIFY(thirdEntry.course.lastAccessed > firstEntry.course.lastAccessed);
    ResumePage reordered;
    QVERIFY(readResume(library, resumeReady, 0, 100, &reordered));
    QCOMPARE(reordered.rows.front().course.id, course3.id);
    QVERIFY(reordered.rows.size() <= 4);
    const auto thirdAccessed = reordered.rows.front().course.lastAccessed;
    QVERIFY(thirdAccessed > 0);
    library.close();

    Library reopened(database);
    QSignalSpy reopenedSignal(&reopened, &Library::opened);
    QSignalSpy reopenedResume(&reopened, &Library::resumeReady);
    Startup reopenedStartup;
    QVERIFY(openLibrary(reopened, reopenedSignal, &reopenedStartup));
    ResumePage persisted;
    QVERIFY(readResume(reopened, reopenedResume, 0, 100, &persisted));
    QCOMPARE(persisted.rows.front().course.id, course3.id);
    QCOMPARE(persisted.rows.front().course.lastAccessed, thirdAccessed);
    QCOMPARE(persisted.rows.front().lesson.name, QStringLiteral("Lesson 1"));
    QVERIFY(persisted.rows.front().lesson.sectionName != persisted.rows.front().lesson.name);
}

void LibraryTest::statsAndActivityUseCanonicalScope() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto courseAPath = root + QStringLiteral("/Course A");
    const auto courseBPath = root + QStringLiteral("/Course B");
    QVERIFY(QDir().mkpath(courseAPath));
    QVERIFY(QDir().mkpath(courseBPath));
    writeFile(courseAPath + QStringLiteral("/Lesson.mp4"));
    writeFile(courseBPath + QStringLiteral("/Lesson.mp4"));
    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));

    QString courseAId;
    QString courseBId;
    QString lessonAId;
    QString lessonBId;
    {
        Library library(database);
        QSignalSpy opened(&library, &Library::opened);
        QSignalSpy scanFinished(&library, &Library::scanFinished);
        QSignalSpy coursesReady(&library, &Library::coursesReady);
        QSignalSpy lessonsReady(&library, &Library::lessonsReady);
        Startup startup;
        QVERIFY(openLibrary(library, opened, &startup));
        ScanResult scan;
        QVERIFY(scanLibrary(library, scanFinished, root, &scan));
        CoursePage courses;
        QVERIFY(readCourses(library, coursesReady, &courses));
        for (const auto& course : courses.rows) {
            if (course.name == QStringLiteral("Course A")) {
                courseAId = course.id;
            } else if (course.name == QStringLiteral("Course B")) {
                courseBId = course.id;
            }
        }
        QVERIFY(!courseAId.isEmpty());
        QVERIFY(!courseBId.isEmpty());
        LessonPage lessonsA;
        QVERIFY(readLessons(library, lessonsReady, courseAId, &lessonsA));
        LessonPage lessonsB;
        QVERIFY(readLessons(library, lessonsReady, courseBId, &lessonsB));
        lessonAId = lessonsA.rows.front().id;
        lessonBId = lessonsB.rows.front().id;

        QVERIFY(QDir(courseBPath).removeRecursively());
        ScanResult missingScan;
        QVERIFY(scanLibrary(library, scanFinished, root, &missingScan));
        library.close();
    }

    QString sqliteError;
    const auto seed = QStringLiteral(
        "UPDATE lessons SET duration = 100, watched_time = 20, completed = 0, file_size = 100 WHERE id = '%3';"
        "UPDATE lessons SET duration = 200, watched_time = 30, completed = 1, file_size = 200 WHERE id = '%4';"
        "INSERT INTO courses (id, identity_id, name, path, fingerprint, last_scanned_at) VALUES "
        "('outside-course', 'outside-identity', 'Outside', '/outside/Course', 'outside-fingerprint', 0);"
        "INSERT INTO sections (id, course_id, name, order_index) VALUES "
        "('outside-section', 'outside-course', 'Outside', 0);"
        "INSERT INTO lessons (id, course_id, section_id, name, path, relative_path, type, duration, "
        "watched_time, file_size, order_index, updated_at) VALUES "
        "('outside-lesson', 'outside-course', 'outside-section', 'Outside lesson', "
        "'/outside/Course/lesson.mp3', 'lesson.mp3', 'audio', 999, 999, 999, 0, 0);"
        "INSERT INTO lesson_activity (id, course_id, lesson_id, activity_date, watched_seconds, completed, created_at) VALUES "
        "('activity-a-old', '%1', '%3', date('now', '-83 days'), 11, 0, 0),"
        "('activity-a-today', '%1', '%3', date('now'), 13, 1, 0),"
        "('activity-a-too-old', '%1', '%3', date('now', '-84 days'), 97, 1, 0),"
        "('activity-b-today', '%2', '%4', date('now'), 7, 0, 0),"
        "('activity-outside', 'outside-course', 'outside-lesson', date('now'), 500, 1, 0);"
    ).arg(courseAId, courseBId, lessonAId, lessonBId);
    QVERIFY2(executeSqlite(database, seed, &sqliteError), qPrintable(sqliteError));

    Library library(database);
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy statsReady(&library, &Library::statsReady);
    QSignalSpy activityReady(&library, &Library::activityReady);
    QSignalSpy failed(&library, &Library::failed);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    QCOMPARE(startup.revision, std::uint64_t{1});

    LibraryStats stats;
    if (!readStats(library, statsReady, startup.revision, &stats)) {
        QVERIFY2(!failed.isEmpty(), "stats returned neither success nor failure");
        const auto error = qvariant_cast<melearner::library::Error>(failed.takeFirst().at(1));
        QFAIL(qPrintable(error.message));
    }
    QCOMPARE(stats.revision, startup.revision);
    QCOMPARE(stats.totalCourses, std::uint64_t{2});
    QCOMPARE(stats.availableCourses, std::uint64_t{1});
    QCOMPARE(stats.missingCourses, std::uint64_t{1});
    QCOMPARE(stats.sections, std::uint64_t{2});
    QCOMPARE(stats.lessons, std::uint64_t{2});
    QCOMPARE(stats.completedLessons, std::uint64_t{1});
    QCOMPARE(stats.completionPercent, std::uint32_t{50});
    QCOMPARE(stats.bytes, std::uint64_t{300});
    QCOMPARE(stats.watchedSeconds, std::uint64_t{50});
    QCOMPARE(stats.totalSeconds, std::uint64_t{300});
    QCOMPARE(stats.mediaTypes.size(), 1);
    QCOMPARE(stats.mediaTypes.front().type, QStringLiteral("video"));
    QCOMPARE(stats.mediaTypes.front().lessons, std::uint64_t{2});
    QCOMPARE(stats.mediaTypes.front().bytes, std::uint64_t{300});
    QCOMPARE(stats.mediaTypes.front().completed, std::uint64_t{1});
    QCOMPARE(stats.mediaTypes.front().watchedSeconds, std::uint64_t{50});
    QCOMPARE(stats.topCourses.size(), 2);
    QCOMPARE(stats.topCourses.at(0).id, courseBId);
    QCOMPARE(stats.topCourses.at(1).id, courseAId);

    ActivityDayPage activity;
    QVERIFY(readActivity(library, activityReady, startup.revision, 0, 84, &activity));
    QCOMPARE(activity.revision, startup.revision);
    QCOMPARE(activity.total, std::uint64_t{2});
    QCOMPARE(activity.rows.size(), 2);
    const auto today = QDateTime::currentDateTimeUtc().date();
    QCOMPARE(activity.throughDate, today.toString(Qt::ISODate));
    QCOMPARE(activity.rows.at(0).date, today.addDays(-83).toString(Qt::ISODate));
    QCOMPARE(activity.rows.at(0).watchedSeconds, std::uint64_t{11});
    QCOMPARE(activity.rows.at(0).lessonsTouched, std::uint64_t{1});
    QCOMPARE(activity.rows.at(0).completions, std::uint64_t{0});
    QCOMPARE(activity.rows.at(1).date, today.toString(Qt::ISODate));
    QCOMPARE(activity.rows.at(1).watchedSeconds, std::uint64_t{20});
    QCOMPARE(activity.rows.at(1).lessonsTouched, std::uint64_t{2});
    QCOMPARE(activity.rows.at(1).completions, std::uint64_t{1});

    ActivityDayPage activityTail;
    QVERIFY(readActivity(library, activityReady, startup.revision, 1, 1, &activityTail));
    QCOMPARE(activityTail.offset, std::uint64_t{1});
    QCOMPARE(activityTail.total, std::uint64_t{2});
    QCOMPARE(activityTail.rows.size(), 1);
    QCOMPARE(activityTail.rows.front().date, today.toString(Qt::ISODate));

    failed.clear();
    QVERIFY(library.stats(startup.revision + 1) != 0);
    QVERIFY(waitFor(failed, 5'000));
    QCOMPARE(qvariant_cast<melearner::library::Error>(failed.takeFirst().at(1)).code, ErrorCode::stale_revision);
}

void LibraryTest::progressReopensWithAtomicActivityInputs() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course");
    QVERIFY(QDir().mkpath(course));
    writeFile(course + QStringLiteral("/intro.mp4"));
    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));

    QString lessonId;
    {
        Library library(database);
        QSignalSpy opened(&library, &Library::opened);
        QSignalSpy scanFinished(&library, &Library::scanFinished);
        QSignalSpy coursesReady(&library, &Library::coursesReady);
        QSignalSpy lessonsReady(&library, &Library::lessonsReady);
        QSignalSpy progressSaved(&library, &Library::progressSaved);
        Startup startup;
        QVERIFY(openLibrary(library, opened, &startup));
        ScanResult scan;
        QVERIFY(scanLibrary(library, scanFinished, root, &scan));
        CoursePage courses;
        QVERIFY(readCourses(library, coursesReady, &courses));
        LessonPage lessons;
        QVERIFY(readLessons(library, lessonsReady, courses.rows.front().id, &lessons));
        lessonId = lessons.rows.front().id;

        QVERIFY(library.saveProgress(lessonId, 12'345, 60'000, false) != 0);
        QTRY_COMPARE_WITH_TIMEOUT(progressSaved.count(), 1, 5'000);
        const auto firstProgress = qvariant_cast<ProgressResult>(progressSaved.takeFirst().at(1));
        QCOMPARE(firstProgress.watchedTime, std::uint64_t{12});
        QCOMPARE(firstProgress.lastPosition, 12.345);
        QVERIFY(!firstProgress.completed);

        QVERIFY(library.saveProgress(lessonId, 60'000, 60'000, true) != 0);
        QTRY_COMPARE_WITH_TIMEOUT(progressSaved.count(), 1, 5'000);
        const auto completedProgress = qvariant_cast<ProgressResult>(progressSaved.takeFirst().at(1));
        QCOMPARE(completedProgress.watchedTime, std::uint64_t{60});
        QVERIFY(completedProgress.completed);

        LessonPage updatedLessons;
        QVERIFY(readLessons(library, lessonsReady, courses.rows.front().id, &updatedLessons));
        QCOMPARE(updatedLessons.rows.front().watchedTime, std::uint64_t{60});
        QCOMPARE(updatedLessons.rows.front().lastPosition, 60.0);
        QVERIFY(updatedLessons.rows.front().completed);
        library.close();
    }

    Library reopened(database);
    QSignalSpy reopenedSignal(&reopened, &Library::opened);
    QSignalSpy reopenedCourses(&reopened, &Library::coursesReady);
    QSignalSpy reopenedLessons(&reopened, &Library::lessonsReady);
    Startup startup;
    QVERIFY(openLibrary(reopened, reopenedSignal, &startup));
    CoursePage courses;
    QVERIFY(readCourses(reopened, reopenedCourses, &courses));
    LessonPage lessons;
    QVERIFY(readLessons(reopened, reopenedLessons, courses.rows.front().id, &lessons));
    QCOMPARE(lessons.rows.front().id, lessonId);
    QCOMPARE(lessons.rows.front().watchedTime, std::uint64_t{60});
    QVERIFY(lessons.rows.front().completed);
}

void LibraryTest::immediateCloseFlushesProgress() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course");
    const auto section = course + QStringLiteral("/Section");
    QVERIFY(QDir().mkpath(section));
    writeFile(section + QStringLiteral("/lesson.mp4"));

    // Keep a scan worker active while the progress request is accepted. This
    // makes the shutdown path deterministic: the progress job is queued behind
    // scan work instead of racing the worker's next dequeue.
    const auto busyRoot = temporary.filePath(QStringLiteral("busy-root"));
    const auto busySection = busyRoot + QStringLiteral("/Busy course/Section");
    QVERIFY(QDir().mkpath(busySection));
    for (int index = 0; index < 8'192; ++index) {
        writeFile(QDir(busySection).filePath(QStringLiteral("lesson-%1.mp4").arg(index)));
    }

    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));
    QString lessonId;
    {
        Library library(database);
        QSignalSpy opened(&library, &Library::opened);
        QSignalSpy scanFinished(&library, &Library::scanFinished);
        QSignalSpy scanProgress(&library, &Library::scanProgress);
        QSignalSpy coursesReady(&library, &Library::coursesReady);
        QSignalSpy lessonsReady(&library, &Library::lessonsReady);
        Startup startup;
        QVERIFY(openLibrary(library, opened, &startup));

        ScanResult initialScan;
        QVERIFY(scanLibrary(library, scanFinished, root, &initialScan));
        CoursePage courses;
        QVERIFY(readCourses(library, coursesReady, &courses));
        QVERIFY(!courses.rows.isEmpty());
        LessonPage lessons;
        QVERIFY(readLessons(library, lessonsReady, courses.rows.front().id, &lessons));
        QVERIFY(!lessons.rows.isEmpty());
        lessonId = lessons.rows.front().id;

        scanProgress.clear();
        QVERIFY(library.scan(busyRoot) != 0);
        QVERIFY(waitFor(scanProgress, 5'000));
        QVERIFY(library.saveProgress(lessonId, 45'000, 60'000, false) != 0);
        library.close();
    }

    Library reopened(database);
    QSignalSpy reopenedSignal(&reopened, &Library::opened);
    QSignalSpy reopenedCourses(&reopened, &Library::coursesReady);
    QSignalSpy reopenedLessons(&reopened, &Library::lessonsReady);
    Startup startup;
    QVERIFY(openLibrary(reopened, reopenedSignal, &startup));
    CoursePage courses;
    QVERIFY(readCourses(reopened, reopenedCourses, &courses));
    LessonPage lessons;
    QVERIFY(readLessons(reopened, reopenedLessons, courses.rows.front().id, &lessons));
    QVERIFY(!lessons.rows.isEmpty());
    const auto saved = std::find_if(lessons.rows.cbegin(), lessons.rows.cend(), [&lessonId](const auto& lesson) {
        return lesson.id == lessonId;
    });
    QVERIFY(saved != lessons.rows.cend());
    QCOMPARE(saved->watchedTime, std::uint64_t{45});
    QCOMPARE(saved->lastPosition, 45.0);
    QVERIFY(!saved->completed);
}

void LibraryTest::markerAndMissingCourseIdentitySurviveRescan() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course");
    QVERIFY(QDir().mkpath(course));
    writeFile(course + QStringLiteral("/lesson.mp4"));
    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));

    Library library(database);
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy coursesReady(&library, &Library::coursesReady);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    ScanResult firstScan;
    QVERIFY(scanLibrary(library, scanFinished, root, &firstScan));
    CoursePage first;
    QVERIFY(readCourses(library, coursesReady, &first));
    const auto id = first.rows.front().id;

    const auto moved = root + QStringLiteral("/Course moved");
    QVERIFY(QDir().rename(course, moved));
    ScanResult movedScan;
    QVERIFY(scanLibrary(library, scanFinished, root, &movedScan));
    CoursePage movedPage;
    QVERIFY(readCourses(library, coursesReady, &movedPage));
    QCOMPARE(movedPage.rows.front().id, id);
    QCOMPARE(movedPage.rows.front().name, QStringLiteral("Course moved"));
    QVERIFY(!movedPage.rows.front().missing);

    QVERIFY(QDir(moved).removeRecursively());
    ScanResult missingScan;
    QVERIFY(scanLibrary(library, scanFinished, root, &missingScan));
    CoursePage missingPage;
    QVERIFY(readCourses(library, coursesReady, &missingPage));
    QCOMPARE(missingPage.rows.front().id, id);
    QVERIFY(missingPage.rows.front().missing);
}

void LibraryTest::searchesPagedNamesAndResolvesMissingState() {
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto root = temporary.filePath(QStringLiteral("root"));
    const auto course = root + QStringLiteral("/Course 10");
    const auto section = course + QStringLiteral("/Section 2");
    QVERIFY(QDir().mkpath(section));
    writeFile(section + QStringLiteral("/Lesson 2.mp4"));
    const auto database = temporary.filePath(QStringLiteral("library.sqlite3"));

    Library library(database);
    QSignalSpy opened(&library, &Library::opened);
    QSignalSpy scanFinished(&library, &Library::scanFinished);
    QSignalSpy searchReady(&library, &Library::searchReady);
    QSignalSpy searchResolved(&library, &Library::searchResolved);
    QSignalSpy failed(&library, &Library::failed);
    Startup startup;
    QVERIFY(openLibrary(library, opened, &startup));
    ScanResult scan;
    QVERIFY(scanLibrary(library, scanFinished, root, &scan));

    SearchPage courseSearch;
    QVERIFY(readSearch(library, searchReady, QStringLiteral("Course 10"), 0, 500, &courseSearch));
    QCOMPARE(courseSearch.total, std::uint64_t{1});
    QCOMPARE(courseSearch.rows.size(), 1);
    QCOMPARE(courseSearch.rows.front().kind, QStringLiteral("course"));
    QCOMPARE(courseSearch.rows.front().name, QStringLiteral("Course 10"));
    QVERIFY(!courseSearch.rows.front().missing);

    SearchPage sectionSearch;
    QVERIFY(readSearch(library, searchReady, QStringLiteral("Section 2"), 0, 100, &sectionSearch));
    QCOMPARE(sectionSearch.total, std::uint64_t{1});
    QCOMPARE(sectionSearch.rows.front().kind, QStringLiteral("section"));
    QCOMPARE(sectionSearch.rows.front().courseName, QStringLiteral("Course 10"));

    SearchPage lessonSearch;
    QVERIFY(readSearch(library, searchReady, QStringLiteral("Lesson"), 0, 100, &lessonSearch));
    QCOMPARE(lessonSearch.total, std::uint64_t{1});
    QCOMPARE(lessonSearch.rows.front().kind, QStringLiteral("lesson"));
    QCOMPARE(lessonSearch.rows.front().courseName, QStringLiteral("Course 10"));
    QCOMPARE(lessonSearch.rows.front().sectionName, QStringLiteral("Section 2"));
    QVERIFY(lessonSearch.rows.size() <= 100);

    QVERIFY(library.resolveSearch(lessonSearch.rows.front().kind, lessonSearch.rows.front().id) != 0);
    if (!waitFor(searchResolved, 5'000)) {
        QVERIFY2(!failed.isEmpty(), "search resolution returned neither success nor failure");
        const auto error = qvariant_cast<melearner::library::Error>(failed.takeFirst().at(1));
        QFAIL(qPrintable(error.message));
    }
    const auto resolution = qvariant_cast<SearchResolution>(searchResolved.takeFirst().at(1));
    QVERIFY(resolution.hasLesson);
    QCOMPARE(resolution.course.name, QStringLiteral("Course 10"));
    QCOMPARE(resolution.lesson.name, QStringLiteral("Lesson 2"));
    QCOMPARE(resolution.lessonOffset, std::uint64_t{0});

    QVERIFY(QDir(course).removeRecursively());
    ScanResult missingScan;
    QVERIFY(scanLibrary(library, scanFinished, root, &missingScan));
    SearchPage missingSearch;
    QVERIFY(readSearch(library, searchReady, QStringLiteral("Course 10"), 0, 100, &missingSearch));
    QCOMPARE(missingSearch.total, std::uint64_t{1});
    QVERIFY(missingSearch.rows.front().missing);

    // FTS syntax characters are treated as literal token input, not executable MATCH syntax.
    SearchPage escapedSearch;
    QVERIFY(readSearch(library, searchReady, QStringLiteral("Course\" OR *"), 0, 100, &escapedSearch));
    QCOMPARE(escapedSearch.query, QStringLiteral("Course\" OR *"));
}

QTEST_MAIN(LibraryTest)
#include "library_test.moc"
